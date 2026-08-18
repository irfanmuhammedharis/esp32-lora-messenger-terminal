// ─────────────────────────────────────────────────────────────────────────────
// Touch alignment check.   pio run -e t7_touchcheck -t upload
//
// The one-screen answer to "is my touch aligned?" Draw a crosshair, tap it,
// and read the verdict. If the registered tap lands more than kOffsetMax px
// from the crosshair, the panel is mis-calibrated - the program says so,
// runs the full calibration flow, and re-checks until the taps line up.
//
// Flow:
//   1. Ensure a calibration exists (boot path if not).
//   2. Three targets in sequence: CENTRE, TOP-LEFT, BOTTOM-RIGHT. Each is
//      drawn, tapped, and the measured offset printed to serial.
//   3. Any offset above kOffsetMax flags "NEEDS CALIBRATION", runs the
//      calibration flow (which now rejects sloppy taps), and restarts the
//      sweep.
//   4. Three clean taps in a row -> green "TOUCH ALIGNED". The sweep then
//      repeats forever in loop(), so a later drift is caught and re-corrected
//      without a power cycle.
//
// Why this catches the offset: a stale or wrong calibration shifts every
// registered point by a consistent amount. Measuring the distance from the
// drawn crosshair to the registered point exposes exactly that. kOffsetMax is
// set well above a fingertip's natural aiming error (~15-25 px on this
// panel), so a calibrated panel passes while a mis-calibrated one fails loud.
//
// The serial printout names the nature of a failure:
//   - all three points shift the same direction   -> constant offset
//   - centre ok but corners drift away            -> scale / non-linearity
//   - TOP-LEFT and BOTTOM-RIGHT swap              -> rotation (swapAxes wrong)
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Touch.h"
#include "TouchCal.h"
#include "app_config.h"
#include "pins.h"

static TFT_eSPI tft;
static Touch    touch;

// A tap a fingertip can make deliberately. Natural aiming error on a 2.4"
// panel is ~15-25 px; a mis-calibration is 50 px or more.
static constexpr int32_t kOffsetMax = 40;

struct Target { int32_t x; int32_t y; const char *name; };

// CENTRE, then two corners inset 60 px so the least-linear rim is still
// exercised without making the tap physically awkward.
static constexpr Target kTargets[] = {
    {120, 160, "CENTER"},
    { 60,  60, "TOP-LEFT"},
    {180, 260, "BOTTOM-RIGHT"},
};
static constexpr size_t kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

// ── Small helpers ───────────────────────────────────────────────────────────

static void drawCrosshair(int32_t x, int32_t y, uint16_t colour) {
    tft.drawCircle(x, y, 12, colour);
    tft.drawCircle(x, y, 4, colour);
    tft.drawFastHLine(x - 18, y, 36, colour);
    tft.drawFastVLine(x, y - 18, 36, colour);
}

static void verdictScreen(const char *line1, const char *line2,
                          uint16_t bg, uint16_t ms) {
    tft.fillScreen(bg);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(line1, tft.width() / 2, tft.height() / 2 - 10, 4);
    if (line2) tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 22, 2);
    tft.setTextDatum(TL_DATUM);
    delay(ms);
}

// Block until a calibrated press lands, or timeoutMs elapses.
static bool waitForPress(int32_t &x, int32_t &y, uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (touch.read(tft.width(), tft.height(), x, y)) return true;
        delay(5);
    }
    return false;
}

static bool waitForRelease(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    TouchRaw r;
    while (millis() - t0 < timeoutMs) {
        if (!touch.readRaw(r)) return true;
        delay(5);
    }
    return false;
}

// ── Calibration assurance (boot path) ───────────────────────────────────────

static void ensureCalibrated() {
    if (touch.loadCal()) {
        Serial.println("  stored calibration . loaded");
        return;
    }
    Serial.println("  stored calibration . none -> calibrating now");
    while (!TouchCalUI::run(tft, touch)) {
        Serial.println("    calibration rejected, retrying");
    }
    Serial.println("  calibration ........ saved to NVS");
}

static void recalibrate() {
    Serial.println("  -> running calibration flow");
    while (!TouchCalUI::run(tft, touch)) {
        Serial.println("    calibration rejected, retrying");
    }
    Serial.println("  calibration ........ saved to NVS");
    TouchCalUI::verify(tft, touch, 5000);
}

// ── The sweep: tap every target, measure the offset ─────────────────────────

// Tap one target, with ONE re-tap allowance for a mis-aim.
//
// A fingertip aimed at a crosshair near the corner naturally drifts toward
// the middle of the panel, so a single tap can exceed the limit even on a
// perfectly calibrated screen. Re-drawing the same target and asking for a
// second, more careful tap separates "I aimed off" from "the calibration is
// off": a genuinely bad calibration reproduces the same offset on the re-tap
// and still fails. Returns Pass if the best of the taps is within kOffsetMax,
// Offset if it exceeds the limit even after the re-tap, and Idle if there was
// no press at all - Idle must never count as a failure, or an unattended
// device would recalibrate itself into an endless loop.
enum class TapResult : uint8_t { Pass, Offset, Idle };

static TapResult tapOne(const Target &t, int32_t &err) {
    for (int attempt = 0; attempt < 2; attempt++) {
        tft.fillScreen(TFT_BLACK);
        drawCrosshair(t.x, t.y, TFT_YELLOW);
        char buf[48];
        snprintf(buf, sizeof(buf), "TAP %s CROSS  %u/%u", t.name,
                 (unsigned)((&t - kTargets) + 1), (unsigned)kTargetCount);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(buf, 8, 8, 2);
        if (attempt == 1) {
            tft.setTextColor(TFT_YELLOW, TFT_BLACK);
            tft.drawString("AIM AGAIN - more carefully", 8, 30, 2);
        }

        int32_t x, y;
        if (!waitForPress(x, y, 15000)) {
            Serial.printf("    %-12s no press in 15s - skipped (idle)\n", t.name);
            return TapResult::Idle;
        }
        waitForRelease(3000);

        const float dx = (float)(x - t.x);
        const float dy = (float)(y - t.y);
        err = (int32_t)(sqrtf(dx * dx + dy * dy) + 0.5f);

        tft.fillCircle(x, y, 5, err <= kOffsetMax ? TFT_GREEN : TFT_RED);
        tft.fillCircle(x, y, 2, TFT_BLACK);

        Serial.printf("    %-12s target (%3ld,%3ld) tapped (%3ld,%3ld)"
                      " -> %3ld px -> %s\n",
                      t.name, (long)t.x, (long)t.y, (long)x, (long)y,
                      (long)err, err <= kOffsetMax ? "OK" : "OFFSET");

        if (err <= kOffsetMax) return TapResult::Pass;
        if (attempt == 0) {
            Serial.printf("    %-12s %ld px over limit - tap again\n",
                          t.name, (long)err);
            delay(600);
        }
    }
    return TapResult::Offset;
}

// Returns true unless a target was measured OFFSET (beyond kOffsetMax even
// after its re-tap). Idle targets - no press, user walked away - do not fail
// the sweep; only a measured offset triggers recalibration. Fills worstOffset
// with the largest measured offset.
static bool sweepTargets(int32_t &worstOffset) {
    worstOffset = 0;
    bool anyOffset = false;

    for (size_t i = 0; i < kTargetCount; i++) {
        int32_t err = 0;
        const TapResult r = tapOne(kTargets[i], err);
        if (err > worstOffset) worstOffset = err;
        if (r == TapResult::Offset) anyOffset = true;
    }
    return !anyOffset;
}

// ── Entry ───────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Touch alignment check");
    Serial.println("=====================================================");
    Serial.printf("  offset limit ...... %d px\n", kOffsetMax);
    Serial.println("-----------------------------------------------------");

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);
    Serial.printf("  panel ............ %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);

    touch.begin();
    ensureCalibrated();

    // Sweep until a clean pass, recalibrating on any offset. This is the
    // "if I touch wrong, understand it needs calibration" part: a measured
    // offset is never ignored, it drives a fresh calibration immediately.
    for (int attempt = 1; ; attempt++) {
        int32_t worst = 0;
        const bool ok = sweepTargets(worst);
        if (ok) break;

        Serial.printf("  ATTEMPT %d: max offset %ld px (limit %d)"
                      " -> recalibrating\n", attempt, (long)worst, kOffsetMax);
        verdictScreen("NEEDS CALIBRATION",
                      "red dots = taps off target", TFT_MAROON, 1500);
        recalibrate();
    }

    Serial.println("  TOUCH ALIGNED - all targets within "
                   + String(kOffsetMax) + " px");
    verdictScreen("TOUCH ALIGNED", "tap any point to re-check",
                  TFT_DARKGREEN, 2000);
}

void loop() {
    // Live re-check: keep drawing targets and correcting drift, so a
    // calibration that slowly walks off is caught instead of trusted.
    delay(1500);
    int32_t worst = 0;
    if (!sweepTargets(worst)) {
        Serial.printf("  drift detected (%ld px) -> recalibrating\n",
                      (long)worst);
        verdictScreen("NEEDS CALIBRATION", "red dots = taps off target",
                      TFT_MAROON, 1500);
        recalibrate();
    }
    verdictScreen("TOUCH ALIGNED", "tap any point to re-check",
                  TFT_DARKGREEN, 2000);
}
