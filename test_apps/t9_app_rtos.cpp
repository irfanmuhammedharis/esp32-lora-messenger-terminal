// LoRa Messenger Terminal — RTOS variant of the integrated application.
//
//     pio run -e t9_app_rtos -t upload
//
// Converts the super-loop in t8_app_cal into three FreeRTOS tasks, each with
// exactly one responsibility:
//
//   radioTask  (pri 3)  owns UART2 + LoraLink parser. Polls the UART, parses
//                       lines, drops beacons, and posts LinkEvents to the UI
//                       task through a bounded queue. Owns the TX queue too.
//   uiTask     (pri 2)  owns LVGL, Touch, MessageStore and the UI. Drains the
//                       event queue, updates the store and screens, ticks the
//                       UI, and runs lv_timer_handler. Feeds the watchdog.
//   statusTask (pri 1)  every 30 s prints the heap / link / queue / message
//                       report from telemetry published by the other tasks.
//
// Why RTOS makes this device better than the loop:
//   * UART is serviced at its own priority, so a long render can never delay
//     a radio line and a burst of log lines cannot starve the UI - the old
//     256-byte per-call drain budget existed only because one loop had to do
//     both jobs.
//   * LVGL + Touch run on exactly one task, so a touch read that corrupts the
//     LCD bus can never race a draw. The loop version was single-threaded and
//     therefore safe by accident; this is safe by construction.
//   * Tasks block on vTaskDelay instead of spinning, so idle CPU drops to
//     near zero; the UI task paces LVGL from its own timer return value.
//   * Radio and UI are decoupled by the queue: the radio never blocks on
//     rendering, and a burst of traffic queues up instead of delaying frames.
//   * Everything is statically allocated - no heap traffic after boot.
//
// The boot path (calibration + alignment check, unchanged from t8_app_cal)
// still runs before the tasks start, so the UI is only ever presented on a
// proven-aligned panel.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

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
// so a stall past kWdtTimeoutS reboots into a working terminal instead. The
// UI task is the one that must never stall, so it feeds the watchdog; a spin
// in the higher-priority radio task starves the UI and trips it too.
static constexpr uint32_t kWdtTimeoutS = 10;

// ── RTOS primitives - all statically allocated, no heap after boot ─────────

#define EVENT_Q_LEN 8            // radio -> UI; LoRa messages are seconds apart
static StaticQueue_t s_eventQStorage;
static uint8_t       s_eventQBuffer[EVENT_Q_LEN * sizeof(LinkEvent)];
static QueueHandle_t s_eventQ;

// Guards LoraLink between the UI task (send) and the radio task (poll).
// send() only copies into the TX ring and bumps a counter, so the lock is
// held for a few dozen cycles, never across a blocking operation.
static StaticSemaphore_t s_nrfLockStorage;
static SemaphoreHandle_t s_nrfLock;

static StaticTask_t s_radioTcb, s_uiTcb, s_statusTcb;
// NOTE: this framework defines StackType_t as uint8_t (see portmacro.h:
// #define portSTACK_TYPE uint8_t), NOT uint32_t. So array size in elements
// IS the size in bytes - a "4096-element" array is 4 KB, not 16 KB. The
// first build declared s_uiStack[4096] expecting 16 KB and got 4 KB, which
// the LVGL render overflowed (stack canary -> wrote into the LVGL pool ->
// TLSF allocator crash). These sizes are real bytes:
static StackType_t  s_radioStack[8192];   // 8 KB - parser + printf live here
static StackType_t  s_uiStack[16384];     // 16 KB - LVGL render lives here
static StackType_t  s_statusStack[4096];  // 4 KB - printf-heavy report

// ── Telemetry published across tasks (monitoring only) ──────────────────────
// Written by the owning task, read by the consumer for the Status screen and
// the periodic report. volatile is sufficient on ESP32 for aligned 8/32-bit
// reads; a momentary skew between fields is acceptable for a status display.
static volatile bool     s_linkUp    = false;
static volatile uint8_t  s_linkState = 0;   // 0 down, 1 searching, 2 up
                                             // (PLAN.md 4.1a)
static volatile uint8_t  s_qDepth    = 0;
static volatile uint8_t  s_qCapacity = NRF_TXQ_LEN;
static volatile uint32_t s_lines     = 0;
static volatile uint32_t s_events    = 0;
static volatile uint32_t s_overruns  = 0;
static volatile uint8_t  s_msgCount  = 0;
static volatile uint8_t  s_msgUnread = 0;

// ── Boot-time alignment check (unchanged from t8_app_cal) ──────────────────
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
            return true;
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
//
// Runs in radioTask context, invoked by nrfLink.poll(). The radio task must
// not touch UI state, so real messages are handed to the UI task through the
// queue. Beacons are dropped here - they are link liveness, not traffic, and
// keeping them out of the queue saves the UI task work on every 10 s tick.
// If the queue is full the event is dropped: the UI is behind, and on a
// fire-and-forget mesh a newer message is always more valuable than an older
// one. The link is still proven alive by the act of sending.
static void onLinkEvent(const LinkEvent &ev, void *) {
    if (ev.type == LinkEventType::None) return;

    if (ev.type == LinkEventType::Rx &&
        strncmp(ev.text, "hello ", 6) == 0) {
        Serial.printf("  [beacon] node %u seq %u\n", ev.src, ev.seq);
        return;
    }

    xQueueSend(s_eventQ, &ev, 0);
}

static void radioTask(void *) {
    while (true) {
        const uint32_t now = millis();

        if (xSemaphoreTake(s_nrfLock, pdMS_TO_TICKS(50)) == pdTRUE) {
            nrfLink.poll(now);
            const bool up = nrfLink.linkUp(now);
            s_linkUp    = up;
            s_qDepth    = nrfLink.queueDepth();
            const LoraLinkParser &p = nrfLink.parser();
            s_lines    = p.linesSeen();
            s_events   = p.eventsParsed();
            s_overruns = p.overruns();
            xSemaphoreGive(s_nrfLock);
        }

        // 5 ms poll: a 160-byte line at 115200 baud takes ~14 ms on the wire,
        // so this assembles it promptly without busy-spinning the CPU.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ── The UI ──────────────────────────────────────────────────────────────────
//
// The single task that owns LVGL, Touch and MessageStore. Everything the
// store and the screens need arrives through the event queue; nothing else
// touches them.

static void applyRx(const LinkEvent &ev, uint32_t now) {
    store.addReceived(ev.src, ev.seq, ev.rssi, ev.snr, ev.text, now);
    Serial.printf("  RX node %u seq %u %ddBm/%ddB \"%s\"\n",
                  ev.src, ev.seq, ev.rssi, ev.snr, ev.text);

    const Message *m = store.at(0);
    if (m) ui.onMessageArrived(*m, now);
}

static void applyTx(const LinkEvent &ev, uint32_t now) {
    store.addSent(ev.seq, ev.text, now);
    ui.setLastTxSeq(ev.seq);
    Serial.printf("  TX confirmed seq %u \"%s\"\n", ev.seq, ev.text);
    if (ui.current() == Screen::Inbox) {
        const Message *m = store.at(0);
        if (m) ui.onMessageArrived(*m, now);
    }
}

static void uiTask(void *) {
    esp_task_wdt_add(NULL);   // this task is the one the watchdog watches

    LinkState lastLinkState = LinkState::Down;
    uint32_t lastTrafficMs = 0;   // mesh traffic witness (PLAN.md 4.1a)

    while (true) {
        const uint32_t now = millis();

        // 1. Drain every pending link event before rendering, so a frame
        //    never shows stale data.
        LinkEvent ev;
        while (xQueueReceive(s_eventQ, &ev, 0) == pdTRUE) {
            if (ev.type == LinkEventType::Rx)         applyRx(ev, now);
            else if (ev.type == LinkEventType::TxConfirm) applyTx(ev, now);
            lastTrafficMs = now;
        }

        // 2. Refresh link + telemetry for the header and Status screen.
        //    Three states (PLAN.md 4.1a): UART liveness from the radio task
        //    (beacons count), traffic presence from the queue drain above.
        const bool trafficFresh = lastTrafficMs != 0 &&
                                  now - lastTrafficMs < NRF_LINK_TIMEOUT_MS;
        const LinkState st =
            !s_linkUp   ? LinkState::Down
            : trafficFresh ? LinkState::Up
                           : LinkState::Searching;
        s_linkState = (uint8_t)st;
        if (st != lastLinkState) {
            lastLinkState = st;
            ui.setLinkState(st);
            Serial.printf("  link %s\n",
                          st == LinkState::Up ? "UP"
                          : st == LinkState::Searching ? "SEARCHING FOR NETWORK"
                                                           : "DOWN - UART silent");
        }
        ui.setQueue(s_qDepth, s_qCapacity);
        ui.setParserStats(s_lines, s_events, s_overruns);
        s_msgCount  = store.count();
        s_msgUnread = store.unreadCount();

        // 3. Pump the UI and LVGL. lv_timer_handler() returns the time until
        //    it next needs to run; we sleep that long (capped) instead of
        //    busy-polling, which is the main idle-CPU saving over the loop.
        ui.tick(now);
        uint32_t d = lvglPortTask();
        if (d == 0) d = 2;
        if (d > 15) d = 15;   // keep event/touch latency bounded
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

// ── Status report ───────────────────────────────────────────────────────────
// Lowest priority: reads only published telemetry, so it can never contend
// with the radio or the UI, and it is deliberately background work.
static void statusTask(void *) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        const char *st = s_linkState == 2 ? "UP"
                       : s_linkState == 1 ? "SCAN" : "DOWN";
        Serial.printf("  t=%lus link %s msgs %u/%u queue %u/%u heap %lu B\n",
                      (unsigned long)(millis() / 1000), st,
                      s_msgUnread, s_msgCount,
                      s_qDepth, s_qCapacity,
                      (unsigned long)ESP.getFreeHeap());
    }
}

// ── Sending ─────────────────────────────────────────────────────────────────
// The UI's send callback. nrfLink is owned by the radio task, so the brief
// lock protects the TX ring; send() only copies into the ring and bumps a
// count, never blocking on the UART.
static bool realSend(const char *text, void *) {
    bool ok = false;
    if (xSemaphoreTake(s_nrfLock, pdMS_TO_TICKS(50)) == pdTRUE) {
        ok = nrfLink.send(text);
        xSemaphoreGive(s_nrfLock);
    }
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
    Serial.println(" LoRa Messenger Terminal (RTOS)");
    Serial.println("=====================================================");
    reportResetReason();

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);

    if (!lvglPortInit(tft)) {
        Serial.println("  FATAL: LVGL draw buffer allocation failed.");
        Serial.println("  Lower LVGL_BUF_LINES or LV_MEM_SIZE and rebuild.");
        while (true) delay(1000);
    }
    Serial.printf("  panel .............. %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);

    // Calibration + alignment are blocking and run before any task exists,
    // so there is no concurrency to worry about here - the UI task inherits
    // a proven-aligned panel (PLAN.md risk R4d).
    touch.begin();
    ensureCalibratedAndAligned();

    if (lvglPortInitTouch(touch)) {
        Serial.println("  touch .............. active");
    } else {
        Serial.println("  touch .............. FAILED - device has no input");
    }

    // ── RTOS objects, static allocation ────────────────────────────────────
    s_eventQ = xQueueCreateStatic(EVENT_Q_LEN, sizeof(LinkEvent),
                                  s_eventQBuffer, &s_eventQStorage);
    s_nrfLock = xSemaphoreCreateMutexStatic(&s_nrfLockStorage);

    nrfLink.begin();
    nrfLink.setEventHandler(onLinkEvent, nullptr);

    // ui.begin() is safe here: no task exists yet, so LVGL is still ours
    // exclusively. From the moment uiTask starts it owns LVGL.
    ui.begin(store, realSend, nullptr);
    ui.noteActivity(millis());

    esp_task_wdt_init(kWdtTimeoutS, /*panic=*/true);

    xTaskCreateStatic(radioTask,  "radio",  sizeof(s_radioStack)  / sizeof(StackType_t),
                      nullptr, 3, s_radioStack,  &s_radioTcb);
    xTaskCreateStatic(uiTask,     "ui",     sizeof(s_uiStack)     / sizeof(StackType_t),
                      nullptr, 2, s_uiStack,     &s_uiTcb);
    xTaskCreateStatic(statusTask, "status", sizeof(s_statusStack) / sizeof(StackType_t),
                      nullptr, 1, s_statusStack, &s_statusTcb);

    Serial.printf("  rtos ............... 3 tasks (radio/ui/status), static\n");
    Serial.printf("  link ............... UART%d @ %d (TX%d/RX%d)\n",
                  NRF_UART_NUM, NRF_BAUD, PIN_NRF_TX, PIN_NRF_RX);
    Serial.printf("  watchdog ........... %lus on the UI task\n",
                  (unsigned long)kWdtTimeoutS);
    Serial.println("-----------------------------------------------------");
}

void loop() {
    // The Arduino loopTask is no longer the application. Keep it alive and
    // fed in case this core registered it with the watchdog, but do nothing.
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
}
