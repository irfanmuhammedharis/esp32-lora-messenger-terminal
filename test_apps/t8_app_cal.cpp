// LoRa Messenger Terminal — integrated application, SELF-CALIBRATING variant.
//
//     pio run -e t8_app_cal -t upload
//
// A duplicate of src/main.cpp with one addition in the boot path: before the
// UI is presented, the panel is asked to prove its alignment with a single
// tap at screen centre. If the tap registers more than kBootCalOffsetMax px
// away, the device does not trust the stored calibration — it recalibrates
// and re-checks until the tap lands where it should.
//
// Why this is worth having as its own build: the original wrong-position bug
// was a stale calibration that loaded silently and presented a UI every tap
// missed. With touch the only input (risk R4d), a mis-calibrated panel is
// indistinguishable from dead touch. This variant closes that hole at boot,
// so a bad calibration is caught the moment it would matter, and the device
// fixes itself instead of being unusable.
//
// Everything else is byte-for-byte the Stage 7 app: the radio path, the UI,
// the watchdog. Only the calibration handling differs.
//
//   pio run -e app        -t upload   -> the plain Stage 7 app
//   pio run -e t8_app_cal -t upload   -> this self-calibrating variant

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

// When mesh traffic (Rx or Tx confirmation) was last seen - the second half
// of the three-state link model (PLAN.md 4.1a).
static uint32_t      lastTrafficMs = 0;

// The watchdog exists because this device's whole job is showing an emergency
// message. A wedged UI that still looks alive is the worst failure available,
// so a stall past WDT_TIMEOUT_S reboots into a working terminal instead.
static constexpr uint32_t kWdtTimeoutS = 10;

// ── Boot-time alignment check ───────────────────────────────────────────────
// A fingertip on a 2.4" panel aims within ~15-25 px of a crosshair it can
// see; a wrong calibration is 50 px or more. This separates "the user aimed
// slightly off" from "the calibration is wrong" and only the latter is
// allowed to trigger a recalibration.
static constexpr int32_t kBootCalOffsetMax = 40;

static bool waitForCalPress(int32_t &x, int32_t &y, uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (touch.read(tft.width(), tft.height(), x, y)) return true;
        delay(5);
    }
    return false;
}

static bool waitForCalRelease(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    TouchRaw r;
    while (millis() - t0 < timeoutMs) {
        if (!touch.readRaw(r)) return true;
        delay(5);
    }
    return false;
}

// One tap at screen centre, measured. One re-tap is allowed for a mis-aim:
// a genuinely bad calibration reproduces the offset on the re-tap and fails,
// while a stray aim does not force the user through a full recalibration.
// Returns true if aligned (or if nobody tapped within the timeout - an idle
// boot must not hang or brick the device).
static bool checkCenterAlignment(int32_t &err) {
    const int32_t cx = tft.width() / 2;
    const int32_t cy = tft.height() / 2;

    for (int attempt = 0; attempt < 2; attempt++) {
        tft.fillScreen(TFT_BLACK);
        tft.drawCircle(cx, cy, 12, TFT_YELLOW);
        tft.drawCircle(cx, cy, 4, TFT_YELLOW);
        tft.drawFastHLine(cx - 18, cy, 36, TFT_YELLOW);
        tft.drawFastVLine(cx, cy - 18, 36, TFT_YELLOW);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("TAP THE CROSS", 8, 8, 2);
        if (attempt == 1) {
            tft.setTextColor(TFT_YELLOW, TFT_BLACK);
            tft.drawString("AIM AGAIN - more carefully", 8, 30, 2);
        }

        int32_t x, y;
        if (!waitForCalPress(x, y, 15000)) {
            Serial.println("  alignment .......... no tap - proceeding as-is");
            return true;   // idle boot must not block forever
        }
        waitForCalRelease(3000);

        const float dx = (float)(x - cx);
        const float dy = (float)(y - cy);
        err = (int32_t)(sqrtf(dx * dx + dy * dy) + 0.5f);

        const bool ok = err <= kBootCalOffsetMax;
        Serial.printf("  alignment .......... centre tap %3ld px off -> %s\n",
                      (long)err, ok ? "OK" : "OFFSET");
        if (ok) return true;
        if (attempt == 0) {
            Serial.printf("  alignment .......... %ld px over limit - tap again\n",
                          (long)err);
        }
    }
    return false;
}

// The self-healing boot path: a calibration must exist AND be demonstrably
// aligned before the UI appears. Loops: ensure calibration -> probe with one
// tap -> recalibrate on any measured offset -> repeat until aligned.
static void ensureCalibratedAndAligned() {
    for (int attempt = 1; ; attempt++) {
        if (!touch.loadCal()) {
            Serial.println("  touch .............. no calibration -> calibrating");
            while (!TouchCalUI::run(tft, touch)) {
                Serial.println("  calibration rejected, retrying");
            }
        }

        int32_t err = 0;
        if (checkCenterAlignment(err)) return;

        Serial.printf("  ATTEMPT %d: centre tap %ld px off (limit %d)"
                      " -> recalibrating\n",
                      attempt, (long)err, kBootCalOffsetMax);
        while (!TouchCalUI::run(tft, touch)) {
            Serial.println("  calibration rejected, retrying");
        }
    }
}

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
            lastTrafficMs = now;
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
    Serial.println(" LoRa Messenger Terminal (self-calibrating)");
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

    // Touch is the only input, so calibration is a boot path, not a bring-up
    // step: a UI drawn on an uncalibrated OR mis-calibrated panel cannot be
    // reached at all (PLAN.md risk R4d). This variant goes one step further
    // than the plain app and PROVES the loaded calibration with a centre tap
    // before presenting the UI, recalibrating on any measured offset.
    touch.begin();
    ensureCalibratedAndAligned();

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
    static LinkState lastLinkState = LinkState::Down;

    const uint32_t now = millis();

    esp_task_wdt_reset();

    nrfLink.poll(now);

    // Three-state link model (PLAN.md 4.1a).
    const bool uartAlive = nrfLink.linkUp(now);
    // Signed: onLinkEvent() stamps lastTrafficMs with millis() during poll(),
    // after `now`, and an unsigned wrap flashed SEARCHING (see src/main.cpp).
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
