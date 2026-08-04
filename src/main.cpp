// LoRa Messenger Terminal — Stage 7, the integrated application.
//
//     pio run -e app -t upload
//
// Wiring only. Every piece below has already been proven on its own:
//
//   Stage 1b  the panel                      lib/lvgl_port
//   Stage 1c  the LVGL port
//   Stage 2   touch + calibration            lib/Touch
//   Stage 3   touch as an LVGL input device   lib/UI
//   Stage 4   the UART link and its parser   lib/LoraLink
//   Stage 5   the whole UI on fake data      lib/UI
//   Stage 6   the logic, on the host         pio test -e native
//
// So the only new thing in the system at this point is one substitution: the
// mock generator in t5_ui.cpp becomes onLinkEvent() below, and mockSend()
// becomes realSend(). If something misbehaves here, every layer underneath it
// has already been signed off and the bug has nowhere to hide.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <lvgl.h>

#include "LoraLink.h"
#include "MessageStore.h"
#include "Touch.h"
#include "TouchCal.h"
#include "UI.h"
#include "app_config.h"
#include "lvgl_port.h"
#include "pins.h"

static TFT_eSPI      tft;
static Touch         touch;
static MessageStore  store;
static LoraLink      nrfLink;
static UI            ui;

// The watchdog exists because this device's whole job is showing an emergency
// message. A wedged UI that still looks alive is the worst failure available,
// so a stall past WDT_TIMEOUT_S reboots into a working terminal instead.
static constexpr uint32_t kWdtTimeoutS = 10;

// ── The radio, arriving ─────────────────────────────────────────────────────

static void onLinkEvent(const LinkEvent &ev, void *) {
    const uint32_t now = millis();

    switch (ev.type) {
        case LinkEventType::Rx: {
            // The node beacons "hello <n>" every 10 s (reference/nrf.cpp:314).
            // Those are link liveness, not messages: showing them would bury
            // real traffic under housekeeping. They still refresh the link
            // timer, because merely receiving one proves the link is up.
            if (strncmp(ev.text, "hello ", 6) == 0) {
                Serial.printf("  [beacon] node %u seq %u\n", ev.src, ev.seq);
                return;
            }

            store.addReceived(ev.src, ev.seq, ev.rssi, ev.snr, ev.text, now);
            Serial.printf("  RX node %u seq %u %ddBm/%ddB \"%s\"\n",
                          ev.src, ev.seq, ev.rssi, ev.snr, ev.text);

            const Message *m = store.at(0);
            if (m) ui.onMessageArrived(*m, now);
            break;
        }

        case LinkEventType::TxConfirm:
            // +TX confirms TRANSMISSION, not reception. The mesh is
            // fire-and-forget, so this is shown as "sent", never "delivered".
            // PLAN.md risk R7.
            store.addSent(ev.seq, ev.text, now);
            ui.setLastTxSeq(ev.seq);
            Serial.printf("  TX confirmed seq %u \"%s\"\n", ev.seq, ev.text);
            if (ui.current() == Screen::Inbox) {
                const Message *m = store.at(0);
                if (m) ui.onMessageArrived(*m, now);
            }
            break;

        case LinkEventType::None:
            break;
    }
}

// The one call that replaces Stage 5's mock. Everything above it is unchanged.
static bool realSend(const char *text, void *) {
    const bool ok = nrfLink.send(text);
    Serial.printf("  -> send \"%s\" %s\n", text, ok ? "queued" : "REJECTED");
    return ok;
}

// ── Boot ────────────────────────────────────────────────────────────────────

static void reportResetReason() {
    const char *r;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   r = "power-on";                      break;
        case ESP_RST_SW:        r = "software";                      break;
        case ESP_RST_PANIC:     r = "PANIC (exception)";             break;
        case ESP_RST_INT_WDT:   r = "interrupt watchdog";            break;
        case ESP_RST_TASK_WDT:  r = "TASK WATCHDOG - UI stalled";    break;
        case ESP_RST_BROWNOUT:  r = "BROWNOUT - check the 3V3 rail"; break;
        case ESP_RST_DEEPSLEEP: r = "deep sleep wake";               break;
        default:                r = "other";                         break;
    }
    Serial.printf("  reset reason ....... %s\n", r);
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" LoRa Messenger Terminal");
    Serial.println("=====================================================");
    reportResetReason();

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);

    if (!lvglPortInit(tft)) {
        // Nothing useful can happen without a display on a device whose only
        // output is the display. Say so on the one channel still working.
        Serial.println("  FATAL: LVGL draw buffer allocation failed.");
        Serial.println("  Lower LVGL_BUF_LINES or LV_MEM_SIZE and rebuild.");
        while (true) delay(1000);
    }
    Serial.printf("  panel .............. %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);

    // Touch is the only input, so this is a boot path, not a bring-up step: a
    // UI drawn on an uncalibrated panel cannot be reached at all, and there is
    // no button left to rescue it with. PLAN.md risk R4d.
    //
    // The calibration flow itself works in raw ADC space and asks for taps at
    // known screen coordinates, so it needs no calibration to run - which is
    // exactly why it can be the recovery path.
    touch.begin();
    if (!touch.loadCal()) {
        Serial.println("  touch .............. no calibration -> calibrating");
        while (!TouchCalUI::run(tft, touch)) {
            Serial.println("  calibration rejected, retrying");
        }
    }

    if (lvglPortInitTouch(touch)) {
        Serial.println("  touch .............. active");
    } else {
        // Nothing can be reached. Say so loudly rather than presenting a UI
        // that silently ignores every tap.
        Serial.println("  touch .............. FAILED - device has no input");
    }

    nrfLink.begin();
    nrfLink.setEventHandler(onLinkEvent, nullptr);

    ui.begin(store, realSend, nullptr);
    ui.noteActivity(millis());

    esp_task_wdt_init(kWdtTimeoutS, /*panic=*/true);
    esp_task_wdt_add(nullptr);

    Serial.printf("  link ............... UART%d @ %d (TX%d/RX%d)\n",
                  NRF_UART_NUM, NRF_BAUD, PIN_NRF_TX, PIN_NRF_RX);
    Serial.printf("  watchdog ........... %lus\n", (unsigned long)kWdtTimeoutS);
    Serial.println("-----------------------------------------------------");
}

void loop() {
    static uint32_t lastReport = 0;
    static bool     lastLinkUp = false;

    const uint32_t now = millis();

    esp_task_wdt_reset();

    nrfLink.poll(now);

    const bool up = nrfLink.linkUp(now);
    if (up != lastLinkUp) {
        lastLinkUp = up;
        ui.setLinkUp(up);
        Serial.printf("  link %s\n", up ? "UP" : "DOWN - nRF not responding");
    }

    ui.setQueue(nrfLink.queueDepth(), nrfLink.queueCapacity());
    const LoraLinkParser &p = nrfLink.parser();
    ui.setParserStats(p.linesSeen(), p.eventsParsed(), p.overruns());

    ui.tick(now);
    lvglPortTask();

    if (now - lastReport >= 30000) {
        lastReport = now;
        Serial.printf("  t=%lus link %s msgs %u/%u queue %u/%u heap %lu B\n",
                      (unsigned long)(now / 1000), up ? "UP" : "DOWN",
                      store.unreadCount(), store.count(),
                      nrfLink.queueDepth(), nrfLink.queueCapacity(),
                      (unsigned long)ESP.getFreeHeap());
    }
}
