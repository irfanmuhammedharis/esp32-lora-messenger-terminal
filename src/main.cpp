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
#include <Wire.h>

#include "Health.h"
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
static HealthSensor  health;
static UI            ui;

// When mesh traffic (an Rx or a Tx confirmation) was last seen. This is the
// second half of the three-state link model (PLAN.md 4.1a): UART liveness
// comes from LoraLink (beacons count), traffic presence from here.
static uint32_t      lastTrafficMs = 0;

// The watchdog exists because this device's whole job is showing an emergency
// message. A wedged UI that still looks alive is the worst failure available,
// so a stall past WDT_TIMEOUT_S reboots into a working terminal instead.
static constexpr uint32_t kWdtTimeoutS = 10;

// ── The radio, arriving ─────────────────────────────────────────────────────

static void onLinkEvent(const LinkEvent &ev, void *) {
    const uint32_t now = millis();

    switch (ev.type) {
        case LinkEventType::Rx: {
            // Every node beacons "hello <n>" every 10 s (reference/nrf.cpp:483).
            // A peer's beacon is not a message - showing them would bury real
            // traffic under housekeeping - but it is proof a peer is in range,
            // so it counts as mesh traffic and ends SEARCHING (PLAN.md 4.1a).
            if (isBeacon(ev)) {
                lastTrafficMs = now;
                Serial.printf("  [beacon] node %u seq %u\n", ev.src, ev.seq);
                return;
            }

            store.addReceived(ev.src, ev.seq, ev.rssi, ev.snr, ev.text, now);
            lastTrafficMs = now;
            Serial.printf("  RX node %u seq %u %ddBm/%ddB \"%s\"\n",
                          ev.src, ev.seq, ev.rssi, ev.snr, ev.text);

            const Message *m = store.at(0);
            if (m) ui.onMessageArrived(*m, now);
            break;
        }

        case LinkEventType::TxConfirm:
            // Our own node's beacon going out proves nothing about peers - a
            // lone node sends one every 10 s - so it is neither a sent
            // message nor mesh traffic. Counting it kept a node with nobody
            // in range at UP instead of SEARCHING.
            if (isBeacon(ev)) {
                Serial.printf("  [beacon] own seq %u sent\n", ev.seq);
                return;
            }

            // +TX confirms TRANSMISSION, not reception. The mesh is
            // fire-and-forget, so this is shown as "sent", never "delivered".
            // PLAN.md risk R7.
            store.addSent(ev.seq, ev.text, now);
            lastTrafficMs = now;
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

    // Health is optional by design: the radio is the core function, and a
    // missing or broken sensor must not brick the terminal (PLAN.md 2.3).
    // The UI simply never shows vitals it has not been given - the Vitals
    // screen says "sensor not connected".
    if (health.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL)) {
        Serial.printf("  health ............. MAX30102 %s, MAX30205 %s\n",
                      health.max30102Present() ? "present" : "absent",
                      health.max30205Present() ? "present" : "absent");
    } else {
        Serial.println("  health ............. no sensor found - vitals disabled");
    }

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
    static uint32_t lastVitals = 0;
    static LinkState lastLinkState = LinkState::Down;

    const uint32_t now = millis();

    esp_task_wdt_reset();

    nrfLink.poll(now);
    health.poll(now);

    // Three-state link model (PLAN.md 4.1a): UART liveness is any complete
    // line within NRF_LINK_TIMEOUT_MS - beacons included, because a beacon
    // proves the wire. Traffic presence is lastTrafficMs. DOWN is reserved
    // for a silent UART; SEARCHING means the wire works but no peer has
    // spoken; UP means mesh traffic flows.
    const bool uartAlive = nrfLink.linkUp(now);

    // Signed difference on purpose: onLinkEvent() stamps lastTrafficMs with
    // millis() during poll(), a few ms AFTER this loop's `now`. Unsigned,
    // now - lastTrafficMs wraps to ~4e9, reads as stale, and the indicator
    // flashed SEARCHING for one loop after every event.
    const bool trafficFresh =
        lastTrafficMs != 0 &&
        (int32_t)(now - lastTrafficMs) < (int32_t)NRF_LINK_TIMEOUT_MS;
    const LinkState st =
        !uartAlive    ? LinkState::Down
        : trafficFresh ? LinkState::Up
                       : LinkState::Searching;
    if (st != lastLinkState) {
        lastLinkState = st;
        ui.setLinkState(st);
        Serial.printf("  link %s\n",
                      st == LinkState::Up        ? "UP"
                      : st == LinkState::Searching ? "SEARCHING FOR NETWORK"
                                                       : "DOWN - UART silent");
    }

    ui.setQueue(nrfLink.queueDepth(), nrfLink.queueCapacity());
    const LoraLinkParser &p = nrfLink.parser();
    ui.setParserStats(p.linesSeen(), p.eventsParsed(), p.overruns());

    // Vitals at ~1 Hz: the HealthCore window is analysed on this cadence and
    // the UI renders from the cached result - the FIFO drain inside poll()
    // already ran at 100 Hz between draws, same as touch (PLAN.md 4.1).
    if (health.max30102Present() && now - lastVitals >= 1000) {
        lastVitals = now;
        HealthReading r = health.latest(now);

        int32_t wave[HealthCore::kWindow];
        const uint16_t n = health.core().copyIrWave(wave, HealthCore::kWindow);
        ui.setVitals(r.hr, r.spo2, r.tempMilliC, r.hrValid, r.spo2Valid,
                     r.tempValid, wave, n);
    }

    ui.tick(now);
    lvglPortTask();

    if (now - lastReport >= 30000) {
        lastReport = now;
        Serial.printf("  t=%lus link %s msgs %u/%u queue %u/%u heap %lu B\n",
                      (unsigned long)(now / 1000),
                      st == LinkState::Up ? "UP"
                      : st == LinkState::Searching ? "SCAN" : "DOWN",
                      store.unreadCount(), store.count(),
                      nrfLink.queueDepth(), nrfLink.queueCapacity(),
                      (unsigned long)ESP.getFreeHeap());
    }
}
