// ─────────────────────────────────────────────────────────────────────────────
// Stage 2 — resistive touch bring-up.   pio run -e t2_touch -t upload
//
// Uses TFT_eSPI directly rather than LVGL. That is deliberate: this stage has
// to prove the touch panel and the LCD can share a bus without corrupting each
// other, and putting a graphics framework in between would only add a second
// suspect. LVGL gets the panel wired in as an input device once it passes.
//
// Three phases, run in order:
//
//   A  RAW      live x/y/z over serial and on screen. Proves the panel reads
//               at all, and shows what pressure this specific sheet produces
//               so TOUCH_Z_THRESHOLD can be set from evidence.
//   B  CALIBRATE  tap four targets; derives the raw->pixel mapping and works
//               out on its own whether the axes are swapped or inverted.
//   C  VERIFY   draw where you touch, with a live error readout. Saves to NVS
//               only after this passes.
//
// Hold any touch during the first two seconds of phase A to discard a stored
// calibration and force a fresh one.
//
// Exit criteria (PLAN.md stage 2):
//   - z reads ~0 open and clearly above threshold when pressed
//   - no display corruption after thousands of reads (the bus restore works)
//   - crosshair tracks a finger within ~5 px across the whole panel
//   - calibration survives a power cycle
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Touch.h"
#include "TouchCal.h"
#include "app_config.h"
#include "pins.h"

static TFT_eSPI tft;
static Touch    touch;

static void banner(const char *l1, const char *l2) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(l1, tft.width() / 2, 6, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(l2, tft.width() / 2, 34, 2);
    tft.setTextDatum(TL_DATUM);
}

// ── Phase A: raw ────────────────────────────────────────────────────────────
// Also the bus-integrity test. It hammers readRaw() continuously while
// redrawing text every frame; if restoreLcdBus() were wrong, the display would
// visibly fall apart within a second or two rather than failing subtly later.
static bool phaseRaw(uint32_t ms) {
    banner("A - RAW", "press anywhere; watch z rise");
    Serial.println();
    Serial.println("[A] Raw readout. Untouched z should sit near 0.");
    Serial.printf("    Current threshold: %d\n", TOUCH_Z_THRESHOLD);

    const uint32_t t0 = millis();
    uint32_t reads = 0, lastPrint = 0;
    bool heldEarly = false;
    uint16_t zOpenMax = 0, zPressMin = TOUCH_ADC_MAX;

    while (millis() - t0 < ms) {
        TouchRaw r;
        const bool pressed = touch.readRaw(r);
        reads++;

        if (pressed && millis() - t0 < 2000) heldEarly = true;
        if (pressed) { if (r.z < zPressMin) zPressMin = r.z; }
        else         { if (r.z > zOpenMax)  zOpenMax  = r.z; }

        if (millis() - lastPrint >= 250) {
            lastPrint = millis();
            char buf[64];
            snprintf(buf, sizeof(buf), "x=%4u y=%4u z=%4u %s",
                     r.x, r.y, r.z, pressed ? "PRESSED" : "open   ");
            tft.setTextColor(pressed ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
            tft.drawString(buf, 8, 70, 4);
            Serial.printf("    %s\n", buf);
        }
    }

    Serial.printf("    %lu reads in %lums, no display corruption = bus restore OK\n",
                  (unsigned long)reads, (unsigned long)ms);
    Serial.printf("    open z max %u | pressed z min %u\n", zOpenMax,
                  zPressMin == TOUCH_ADC_MAX ? 0 : zPressMin);
    if (zOpenMax >= TOUCH_Z_THRESHOLD)
        Serial.println("    WARNING: idle noise exceeds the threshold - raise it.");
    return heldEarly;
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 2 - resistive touch bring-up");
    Serial.println("=====================================================");
    Serial.printf("  XP=GPIO%-2d (LCD_D0)   XM=GPIO%-2d (LCD_RS, ADC)\n",
                  PIN_TOUCH_XP, PIN_TOUCH_XM);
    Serial.printf("  YM=GPIO%-2d (LCD_D1)   YP=GPIO%-2d (LCD_CS, ADC)\n",
                  PIN_TOUCH_YM, PIN_TOUCH_YP);

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);
    Serial.printf("  panel .............. %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);

    touch.begin();

    const bool had = touch.loadCal();
    Serial.printf("  stored calibration . %s\n",
                  had ? "loaded" : "none for this rotation");
    Serial.println("-----------------------------------------------------");

    const bool forceRecal = phaseRaw(8000);
    if (had && !forceRecal) {
        Serial.println("[B] Skipped - using the stored calibration.");
        Serial.println("    Hold a touch during the first 2s of phase A to redo it.");
    } else {
        if (forceRecal) touch.clearCal();
        // The calibration flow itself lives in lib/TouchCal so the app can run
        // it at boot too - with touch the only input, an uncalibrated panel is
        // an unreachable device, so this is a recovery path and not just a
        // bring-up step (PLAN.md risk R4d).
        while (!TouchCalUI::run(tft, touch)) {
            Serial.println("    rejected, try again");
        }
    }

    TouchCalUI::verify(tft, touch, 20000);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 2 complete. If the dot tracked your finger and");
    Serial.println(" the display never glitched, touch is signed off.");
    Serial.println(" Next:  pio run -e t1c_lvgl -t upload   (touch in LVGL)");
    Serial.println("=====================================================");
}

void loop() {
    // Verify runs again on demand so corners can be re-checked without a
    // power cycle.
    TouchCalUI::verify(tft, touch, 20000);
}
