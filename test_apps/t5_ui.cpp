// ─────────────────────────────────────────────────────────────────────────────
// Stage 5 — the whole UI, on fake data.   pio run -e t5_ui -t upload
//
// Every screen, both input devices, and a message generator standing in for
// the radio. NO nRF is involved and none needs to be: the point of this stage
// is that the UI is finished and debugged before the radio is ever attached,
// so that at Stage 7 the only new thing in the system is one function call
// replacing the generator.
//
// Exit criteria (PLAN.md stage 5):
//   - all 7 screens navigable by button AND by touch
//   - driven by a fake message generator, no radio involved
//   - the Vitals screen and inbox strip render from fake vitals (stage 4c)
//
// Sending is mocked: trySend() reports success and the message lands in the
// store as an outgoing entry, exactly as a +TX confirmation would.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <math.h>

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
static UI            ui;

static uint16_t mockSeq = 1;

// Stand-in for LoraLink::send(). Returns true so the UI takes the "queued"
// path; Stage 7 swaps this one function for the real link.
static bool mockSend(const char *text, void *) {
    Serial.printf("  [mock TX] \"%s\"\n", text);
    store.addSent(mockSeq++, text, millis());
    return true;
}

// Injects traffic that looks like a real deployment: mostly ordinary presets,
// with an occasional SOS so the full-screen takeover gets exercised without
// having to wait for a real emergency.
static void mockGenerator(uint32_t nowMs) {
    static uint32_t next = 8000;
    static uint8_t  n = 0;

    if (nowMs < next) return;
    next = nowMs + 7000;

    struct Fake { uint8_t src; const char *text; };
    static const Fake fakes[] = {
        {2, "IM HERE"},
        {4, "NEED REINFORCEMENT"},
        {7, "ALL CLEAR"},
        {3, "HOLD POSITION"},
        {5, "SOS MEDIC NEEDED"},      // triggers the takeover
        {2, "MOVING OUT"},
        {9, "RETURNING TO BASE"},
    };

    const Fake &f = fakes[n % (sizeof(fakes) / sizeof(fakes[0]))];
    n++;

    const int16_t rssi = -60 - (int16_t)(n * 7 % 50);
    const int8_t  snr  = (int8_t)(11 - (n * 3 % 14));

    store.addReceived(f.src, mockSeq++, rssi, snr, f.text, nowMs);
    Serial.printf("  [mock RX] node %u  %d dBm  \"%s\"\n", f.src, rssi, f.text);

    const Message *m = store.at(0);
    if (m) ui.onMessageArrived(*m, nowMs);

    // The first fake message is the mesh-traffic witness: it raises the link
    // from SEARCHING to UP, exercising the three-state path (PLAN.md 4.1a).
    if (n == 1) ui.setLinkState(LinkState::Up);
}

// Synthetic vitals, ~1 Hz: a generated 60 bpm PPG wave with plausible
// values. Generated, not recorded - the same deal as the messages, so no
// sensor hardware is involved in this stage (PLAN.md stage 4c).
static void mockVitals(uint32_t nowMs) {
    static uint32_t next = 1500;
    if (nowMs < next) return;
    next = nowMs + 1000;

    static uint32_t phase = 0;
    int32_t wave[200];
    for (uint32_t i = 0; i < 200; i++) {
        // One Gaussian systolic bump every 100 samples = 60 bpm at 100 Hz.
        const double d = (double)((phase + i) % 100) - 50.0;
        wave[i] = 60000 + (int32_t)(4000.0 * exp(-(d * d) / (2.0 * 8.0 * 8.0)));
    }
    phase = (phase + 100) % 100;   // advance exactly one second of samples

    ui.setVitals(72, 98, 36600, true, true, true, wave, 200);
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 5 - full UI on mock data");
    Serial.println("=====================================================");

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);

    if (!lvglPortInit(tft)) {
        Serial.println("  FATAL: draw buffer allocation failed.");
        while (true) delay(1000);
    }
    Serial.printf("  panel .............. %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);

    // Touch is the only input, so calibration is a boot path rather than a
    // bring-up step: a UI drawn on an uncalibrated panel cannot be reached at
    // all. PLAN.md risk R4d.
    touch.begin();
    if (!touch.loadCal()) {
        Serial.println("  touch .............. no calibration -> calibrating now");
        while (!TouchCalUI::run(tft, touch)) {
            Serial.println("  retrying calibration...");
        }
        TouchCalUI::verify(tft, touch, 6000);
    }

    if (lvglPortInitTouch(touch)) {
        Serial.println("  touch .............. active (POINTER)");
    } else {
        Serial.println("  touch .............. FAILED to register - no input!");
    }

    ui.begin(store, mockSend, nullptr);
    // Start in SEARCHING: the fake UART is alive but no fake peer has spoken
    // yet; the first mock message raises it to UP (PLAN.md 4.1a).
    ui.setLinkState(LinkState::Searching);
    ui.noteActivity(millis());

    Serial.println("-----------------------------------------------------");
    Serial.println("  Touch only. SOS is bottom-left on every screen, one tap.");
    Serial.println("  BACK is bottom-right everywhere except the inbox.");
    Serial.println("  A fake message arrives every 7 s; one of them is an SOS.");
    Serial.println("  VITALS row + inbox strip run on fake vitals (72 bpm).");
    Serial.println("-----------------------------------------------------");
}

void loop() {
    static uint32_t lastReport = 0;
    const uint32_t now = millis();

    mockGenerator(now);
    mockVitals(now);
    ui.tick(now);
    lvglPortTask();

    if (now - lastReport >= 10000) {
        lastReport = now;
        lv_mem_monitor_t m;
        lv_mem_monitor(&m);
        Serial.printf("  t=%3lus  screen %u  msgs %u (%u unread)  "
                      "pool %2u%% frag %2u%%  heap %lu B\n",
                      (unsigned long)(now / 1000),
                      (unsigned)ui.current(), store.count(), store.unreadCount(),
                      (unsigned)m.used_pct, (unsigned)m.frag_pct,
                      (unsigned long)ESP.getFreeHeap());
    }
}
