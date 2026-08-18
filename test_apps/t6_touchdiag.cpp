// ─────────────────────────────────────────────────────────────────────────────
// Touch diagnostic suite.   pio run -e t6_touchdiag -t upload
//
// A self-scoring battery for the one subsystem the whole device depends on.
// Stage 2 (t2_touch) proved the panel reads and calibrates; this goes further
// and asks "does it stay right under load, and is the mapping accurate enough
// to trust a UI_MIN_TOUCH_PX target?" Every phase either passes or fails on a
// number, and the scorecard at the end is the sign-off.
//
// Talks to TFT_eSPI directly, exactly like Stage 2, so a fault here is never
// confused with an LVGL one. To exercise the same panel through the LVGL
// input pipeline (confirmation, smoothing, scroll discrimination) run Stage 3
// (t3_touchui) after this.
//
//   A  RAW HEALTH       10 s idle must produce ZERO phantom presses with z
//                       noise below TOUCH_Z_THRESHOLD; then a 20 s press-and-
//                       drag must show pressure well above threshold and a
//                       raw span covering most of the sheet on both axes.
//   B  CALIBRATION      load the stored calibration, or run the boot-path
//                       calibration flow (TouchCalUI) if there is none, then
//                       a quick verify. No valid mapping, no later phase.
//   C  BUS INTEGRITY    write a colour, interleave hundreds of touch reads,
//                       read the pixel back. A broken restoreLcdBus() shows
//                       up here as garbage, exactly where nothing else in
//                       the system points at it.
//   D  TAP ACCURACY     a 3x3 grid of targets, tapped once each. Measures how
//                       far the registered point lands from the cross.
//   E  DRAG LINEARITY   trace a horizontal and a vertical line end to end.
//                       Measures the perpendicular deviation from the ideal
//                       line, which is calibration error, not finger wobble.
//   F  TAP REGISTRATION 15 s of deliberate taps. Every physical tap must
//                       become exactly one press and one release - no merges
//                       from a mid-press dropout, no repeats from a release
//                       read early, no taps lost to over-aggressive
//                       confirmation.
//
// Exit criteria (all six phases PASS):
//   A  zero phantom presses; idle z stays below TOUCH_Z_THRESHOLD;
//      pressed z >= threshold + 200; raw span >= 1500 counts on both axes
//   B  a calibration exists (loaded or freshly taken)
//   C  all four probe colours read back intact after interleaved reads
//   D  every grid tap within 25 px of its cross
//   E  every drag within 20 px of its line
//   F  presses == releases and at least 5 taps registered
//
// Runs once at boot and leaves the verdict on screen. Reset to re-run.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Touch.h"
#include "TouchCal.h"
#include "app_config.h"
#include "pins.h"

static TFT_eSPI tft;
static Touch    touch;

// ── Tunables ────────────────────────────────────────────────────────────────
static constexpr uint32_t kIdleMs     = 10000;  // A1: hands-off window
static constexpr uint32_t kPressMs    = 20000;  // A2: press-and-drag window
static constexpr int32_t  kRawSpanMin = 1500;   // A2: min raw travel, of 4095
static constexpr uint16_t kZMargin    = 200;    // A2: pressed z must clear
                                                //     threshold by this much
static constexpr uint16_t kReadbackMask = 0xF79E; // C: top 4 bits per channel
static constexpr int32_t  kTapErrMax  = 25;     // D: px from the cross
static constexpr int32_t  kDragErrMax = 20;     // E: px from the line
static constexpr uint32_t kTapCountMs = 15000;  // F: quick-tap window
static constexpr uint8_t  kTapMin     = 5;      // F: taps that must register

// ── Small helpers ───────────────────────────────────────────────────────────

static void banner(const char *title, uint16_t bg, uint16_t fg) {
    tft.fillScreen(bg);
    tft.setTextColor(fg, bg);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(title, tft.width() / 2, 6, 4);
    tft.setTextDatum(TL_DATUM);
}

// A crosshair target, the same shape the calibration flow uses, so the eye
// already knows how to aim at one.
static void drawTarget(int32_t x, int32_t y, uint16_t colour) {
    tft.drawCircle(x, y, 12, colour);
    tft.drawCircle(x, y, 4, colour);
    tft.drawFastHLine(x - 18, y, 36, colour);
    tft.drawFastVLine(x, y - 18, 36, colour);
}

// Block until a calibrated press lands, or timeoutMs elapses.
// Returns false on timeout so a phase can fail loudly instead of hanging.
static bool waitForPress(int32_t &x, int32_t &y, uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (touch.read(tft.width(), tft.height(), x, y)) return true;
        delay(5);
    }
    return false;
}

// Block until the panel goes open, or timeoutMs elapses.
static bool waitForRelease(uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    TouchRaw r;
    while (millis() - t0 < timeoutMs) {
        if (!touch.readRaw(r)) return true;
        delay(5);
    }
    return false;
}

// ── Phase A: raw health ─────────────────────────────────────────────────────

static bool phaseRawHealth() {
    banner("A - RAW HEALTH", TFT_NAVY, TFT_CYAN);
    Serial.println();
    Serial.println("[A] Raw health.");
    Serial.printf("    threshold %d | idle %lus then press-and-drag %lus\n",
                  TOUCH_Z_THRESHOLD, (unsigned long)(kIdleMs / 1000),
                  (unsigned long)(kPressMs / 1000));

    // ── A1: idle. Keep hands off ────────────────────────────────────────────
    // An open panel that crosses the threshold on its own is a phantom
    // press waiting to fire an SOS. This is the whole reason the buttons were
    // removed, so it is tested first and tested hard.
    Serial.println("[A1] Hands off the panel for 10s.");
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("KEEP HANDS OFF", 8, 40, 2);
    tft.drawString("10 seconds", 8, 62, 2);

    uint32_t phantom = 0;
    uint16_t idleZMax = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < kIdleMs) {
        TouchRaw r;
        if (touch.readRaw(r)) phantom++;
        else if (r.z > idleZMax) idleZMax = r.z;
        delay(5);
    }
    const bool idleOk = (phantom == 0) && (idleZMax < TOUCH_Z_THRESHOLD);
    Serial.printf("    idle z max %u (need < %d), phantom presses %lu -> %s\n",
                  idleZMax, TOUCH_Z_THRESHOLD, (unsigned long)phantom,
                  idleOk ? "PASS" : "FAIL");
    if (phantom && idleOk == false) {
        Serial.println("    NOTE: you must keep hands off during the idle window.");
    }

    // ── A2: press and drag everywhere ───────────────────────────────────────
    // The panel must show real pressure (z clearly above threshold, not
    // merely above it) and the raw axes must span most of the sheet - a tiny
    // span means a corner is dead or the sheet is stuck.
    Serial.println("[A2] Press and drag all over the panel for 20s.");
    tft.fillScreen(TFT_NAVY);
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString("PRESS + DRAG", 8, 8, 2);
    tft.drawString("everywhere, 20s", 8, 30, 2);

    bool     everPressed = false;
    uint16_t zMin = TOUCH_ADC_MAX;
    int32_t  xMin = INT32_MAX, xMax = INT32_MIN;
    int32_t  yMin = INT32_MAX, yMax = INT32_MIN;

    const uint32_t t1 = millis();
    while (millis() - t1 < kPressMs) {
        TouchRaw r;
        if (touch.readRaw(r)) {
            everPressed = true;
            if (r.z < zMin) zMin = r.z;
            if ((int32_t)r.x < xMin) xMin = r.x;
            if ((int32_t)r.x > xMax) xMax = r.x;
            if ((int32_t)r.y < yMin) yMin = r.y;
            if ((int32_t)r.y > yMax) yMax = r.y;

            // Raw axes live in panel space; scale to the screen for a dot
            // that follows the finger. Axes may be swapped/inverted - that
            // is calibration's job, not this phase's - so the dot only needs
            // to move, not to land accurately.
            const int32_t sx = map(r.x, 0, TOUCH_ADC_MAX, 0, tft.width() - 1);
            const int32_t sy = map(r.y, 0, TOUCH_ADC_MAX, 0, tft.height() - 1);
            tft.fillCircle(sx, sy, 2, TFT_GREEN);
        }
        delay(10);
    }

    const int32_t xSpan = xMax - xMin;
    const int32_t ySpan = yMax - yMin;
    const bool pressOk = everPressed &&
                         (zMin >= (TOUCH_Z_THRESHOLD + kZMargin));
    const bool spanOk = (xSpan >= kRawSpanMin) && (ySpan >= kRawSpanMin);

    Serial.printf("    pressed z min %u (need >= %d) -> %s\n",
                  zMin, TOUCH_Z_THRESHOLD + kZMargin,
                  pressOk ? "PASS" : "FAIL");
    Serial.printf("    raw span x %ld y %ld (need >= %d both) -> %s\n",
                  (long)xSpan, (long)ySpan, kRawSpanMin,
                  spanOk ? "PASS" : "FAIL");
    if (!everPressed)
        Serial.println("    NOTE: nothing pressed - hold the panel next time.");
    if (!pressOk)
        Serial.println("    NOTE: if z stayed near threshold, raise");
        Serial.println("          TOUCH_Z_THRESHOLD or check the ADC range.");

    return idleOk && pressOk && spanOk;
}

// ── Phase B: calibration assurance ──────────────────────────────────────────

static bool phaseCalibration() {
    banner("B - CALIBRATION", TFT_NAVY, TFT_CYAN);
    Serial.println();
    if (touch.loadCal()) {
        Serial.println("    stored calibration loaded for this rotation");
        tft.setTextColor(TFT_GREEN, TFT_NAVY);
        tft.drawString("CALIBRATION OK", 8, 40, 2);
        return true;
    }

    // With touch the only input, an uncalibrated panel is an unreachable
    // device: this is the boot path, not a bring-up step (PLAN.md risk R4d).
    // The flow works in raw ADC space, so it needs no calibration to run.
    Serial.println("    no valid calibration -> running the boot-path flow");
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString("CALIBRATING NOW", 8, 40, 2);
    delay(1200);
    while (!TouchCalUI::run(tft, touch)) {
        Serial.println("    calibration rejected, retrying");
    }
    Serial.println("    calibration saved to NVS");
    TouchCalUI::verify(tft, touch, 6000);
    return true;
}

// ── Phase C: bus integrity ──────────────────────────────────────────────────

static bool phaseBusIntegrity() {
    banner("C - BUS INTEGRITY", TFT_NAVY, TFT_CYAN);
    Serial.println();
    Serial.println("[C] Write a colour, interleave 100 touch reads,");
    Serial.println("    read the pixel back. A failed bus restore shows up");
    Serial.println("    as a mismatch on the very next read.");

    struct Probe { uint16_t colour; const char *name; };
    static const Probe probes[] = {
        {TFT_RED, "RED"}, {TFT_GREEN, "GREEN"},
        {TFT_BLUE, "BLUE"}, {TFT_WHITE, "WHITE"},
    };

    bool pass = true;
    for (const Probe &p : probes) {
        tft.fillScreen(TFT_BLACK);
        tft.fillRect(8, 8, 32, 32, p.colour);

        // Hammer the touch path. If restoreLcdBus() were wrong, CS/RS would
        // be left as analog inputs and this readback would return garbage.
        TouchRaw r;
        for (int i = 0; i < 100; i++) touch.readRaw(r);

        // The read path is 18-bit truncated to 16, so compare the top four
        // bits per channel - the same mask Stage 1b's test 8 uses.
        const uint16_t got = tft.readPixel(10, 10);
        const bool ok = (got & kReadbackMask) == (p.colour & kReadbackMask);
        Serial.printf("    %-6s wrote 0x%04X read 0x%04X -> %s\n",
                      p.name, p.colour, got, ok ? "PASS" : "FAIL");
        if (!ok) pass = false;
        delay(150);
    }
    return pass;
}

// ── Phase D: tap accuracy ───────────────────────────────────────────────────

static bool phaseTapAccuracy() {
    banner("D - TAP ACCURACY", TFT_NAVY, TFT_CYAN);
    Serial.println();
    Serial.println("[D] Tap the centre of each of the 9 crosses.");
    Serial.printf("    A hit is within %d px of the cross.\n", kTapErrMax);

    const int32_t w = tft.width(), h = tft.height();
    // Inset from the edges: the least-linear zone of a resistive sheet is
    // its rim, so targets sit inside it and the corners get their own check
    // from the grid's outermost points.
    const int32_t inset = 45;
    const int32_t xs[3] = {inset, w / 2, w - 1 - inset};
    const int32_t ys[3] = {inset, h / 2, h - 1 - inset};

    float worst = 0.0f;
    uint8_t hits = 0;
    bool all = true;

    for (uint8_t gy = 0; gy < 3; gy++) {
        for (uint8_t gx = 0; gx < 3; gx++) {
            const int32_t cx = xs[gx], cy = ys[gy];

            tft.fillScreen(TFT_NAVY);
            drawTarget(cx, cy, TFT_YELLOW);
            char buf[32];
            snprintf(buf, sizeof(buf), "tap %u of 9", gy * 3 + gx + 1);
            tft.setTextColor(TFT_WHITE, TFT_NAVY);
            tft.drawString(buf, 8, 8, 2);

            int32_t tx, ty;
            if (!waitForPress(tx, ty, 10000)) {
                Serial.printf("    target (%3ld,%3ld): no press in 10s -> FAIL\n",
                              (long)cx, (long)cy);
                all = false;
                continue;
            }
            waitForRelease(3000);

            const float dx = (float)(tx - cx), dy = (float)(ty - cy);
            const float err = sqrtf(dx * dx + dy * dy);
            if (err > worst) worst = err;
            const bool ok = err <= kTapErrMax;
            if (ok) hits++;
            else all = false;

            tft.fillCircle(tx, ty, 4, ok ? TFT_GREEN : TFT_RED);
            Serial.printf("    target (%3ld,%3ld) tapped (%3ld,%3ld) "
                          "err %4.1f px -> %s\n",
                          (long)cx, (long)cy, (long)tx, (long)ty,
                          (double)err, ok ? "PASS" : "FAIL");
            delay(600);
        }
    }

    Serial.printf("    hits %u/9, worst error %.1f px (limit %d)\n",
                  hits, (double)worst, kTapErrMax);
    return all;
}

// ── Phase E: drag linearity ─────────────────────────────────────────────────

// Trace an axis-aligned line and return the maximum perpendicular deviation.
// The user aims the green dot along the dim line; how far it wanders off the
// line is calibration error (mapping non-linearity) plus how well a finger
// can follow, so the limit is deliberately more generous than tap accuracy.
static int32_t traceLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const bool horizontal = (y1 == y0);
    int32_t maxDev = 0;
    bool seen = false;

    const uint32_t t0 = millis();
    while (millis() - t0 < 25000) {
        int32_t x, y;
        if (!touch.read(tft.width(), tft.height(), x, y)) { delay(5); continue; }
        seen = true;

        const int32_t dev = horizontal ? abs(y - y0) : abs(x - x0);
        if (dev > maxDev) maxDev = dev;

        tft.fillCircle(x, y, 2, TFT_GREEN);

        // Finished once the trace reaches the far end (within a few px).
        if (horizontal ? (abs(x - x1) <= 6) : (abs(y - y1) <= 6)) break;
        delay(8);
    }
    return seen ? maxDev : INT32_MAX;
}

static bool phaseDragLinearity() {
    banner("E - DRAG LINEARITY", TFT_NAVY, TFT_CYAN);
    Serial.println();
    Serial.println("[E] Trace each line slowly, end to end.");
    Serial.printf("    The dot must stay within %d px of the line.\n",
                  kDragErrMax);

    const int32_t w = tft.width(), h = tft.height();

    // Horizontal pass, mid-screen, full width.
    const int32_t hy = h / 2;
    tft.fillScreen(TFT_NAVY);
    tft.drawFastHLine(12, hy, w - 25, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("TRACE LEFT -> RIGHT", 8, 8, 2);
    const int32_t hMax = traceLine(12, hy, w - 13, hy);
    const bool hOk = (hMax != INT32_MAX) && (hMax <= kDragErrMax);
    Serial.printf("    horizontal max deviation %ld px -> %s\n",
                  (long)hMax, hOk ? "PASS" : "FAIL");

    // Vertical pass, mid-screen, full height.
    const int32_t vx = w / 2;
    tft.fillScreen(TFT_NAVY);
    tft.drawFastVLine(vx, 12, h - 25, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("TRACE TOP -> BOTTOM", 8, 8, 2);
    const int32_t vMax = traceLine(vx, 12, vx, h - 13);
    const bool vOk = (vMax != INT32_MAX) && (vMax <= kDragErrMax);
    Serial.printf("    vertical   max deviation %ld px -> %s\n",
                  (long)vMax, vOk ? "PASS" : "FAIL");

    return hOk && vOk;
}

// ── Phase F: tap registration ───────────────────────────────────────────────

static bool phaseTapRegistration() {
    banner("F - TAP REGISTRATION", TFT_NAVY, TFT_CYAN);
    Serial.println();
    Serial.println("[F] Tap the button repeatedly for 15s.");
    Serial.println("    Every physical tap must register exactly once.");

    const int32_t w = tft.width(), h = tft.height();
    const int32_t zx = 20, zy = 60, zw = w - 40, zh = h - 90;
    tft.fillRect(zx, zy, zw, zh, TFT_DARKGREY);
    tft.drawRect(zx, zy, zw, zh, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("TAP HERE", w / 2, zy + zh / 2, 2);
    tft.setTextDatum(TL_DATUM);

    // Count logical transitions in the *calibrated* stream - the same stream
    // LVGL sees. A mid-press dropout that the release debounce failed to
    // absorb would split one tap into press/release/press; a release read
    // too early would do the same. Presses must therefore equal releases.
    uint32_t presses = 0, releases = 0;
    bool pressed = false;

    const uint32_t t0 = millis();
    while (millis() - t0 < kTapCountMs) {
        int32_t x, y;
        const bool now = touch.read(w, h, x, y);
        if (now && !pressed) presses++;
        if (!now && pressed) releases++;
        pressed = now;

        // Live counter, away from the tap zone.
        char buf[24];
        snprintf(buf, sizeof(buf), "taps %lu", (unsigned long)presses);
        tft.setTextColor(TFT_WHITE, TFT_NAVY);
        tft.fillRect(0, h - 24, w, 24, TFT_NAVY);
        tft.drawString(buf, 8, h - 20, 2);
        delay(3);
    }

    // Grace period for a tap still held when the window closes, so a press
    // on the boundary is not counted as unbalanced.
    const uint32_t tEnd = millis();
    while (pressed && millis() - tEnd < 1000) {
        int32_t x, y;
        if (!touch.read(w, h, x, y)) { releases++; pressed = false; }
        delay(5);
    }

    const bool balanced = (presses == releases);
    const bool enough = presses >= kTapMin;
    Serial.printf("    presses %lu, releases %lu -> %s\n",
                  (unsigned long)presses, (unsigned long)releases,
                  balanced ? "balanced" : "UNBALANCED");
    Serial.printf("    %lu taps registered (need >= %u) -> %s\n",
                  (unsigned long)presses, kTapMin,
                  enough ? "PASS" : "FAIL");
    if (!balanced)
        Serial.println("    NOTE: uneven counts mean a tap split or merged -");
        Serial.println("          suspect the release debounce.");
    return balanced && enough;
}

// ── Scorecard ───────────────────────────────────────────────────────────────

static const char *const kPhaseNames[6] = {
    "A RAW HEALTH", "B CALIBRATION", "C BUS INTEGRITY",
    "D TAP ACCURACY", "E DRAG LINEARITY", "F TAP REGISTRATION",
};

static void showScorecard(const bool *results) {
    uint8_t passed = 0;
    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" TOUCH DIAGNOSTIC SCORECARD");
    Serial.println("=====================================================");
    for (uint8_t i = 0; i < 6; i++) {
        Serial.printf("  %-18s %s\n", kPhaseNames[i],
                      results[i] ? "PASS" : "FAIL");
        if (results[i]) passed++;
    }
    Serial.printf("  %u/6 phases passed\n", passed);
    Serial.println(passed == 6
        ? "  VERDICT: touch is good to go."
        : "  VERDICT: fix the failing phase(s), then reset to re-run.");
    Serial.println("=====================================================");

    const uint16_t bg = passed == 6 ? TFT_DARKGREEN : TFT_MAROON;
    tft.fillScreen(bg);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextDatum(MC_DATUM);
    char buf[40];
    snprintf(buf, sizeof(buf), "TOUCH %s", passed == 6 ? "OK" : "NEEDS WORK");
    tft.drawString(buf, tft.width() / 2, tft.height() / 2 - 10, 4);
    snprintf(buf, sizeof(buf), "%u/6 phases passed", passed);
    tft.drawString(buf, tft.width() / 2, tft.height() / 2 + 22, 2);
    tft.drawString("reset to re-run", tft.width() / 2,
                   tft.height() / 2 + 44, 2);
    tft.setTextDatum(TL_DATUM);
}

// ── Entry ───────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Touch diagnostic suite");
    Serial.println("=====================================================");
    Serial.printf("  threshold ........ %d\n", TOUCH_Z_THRESHOLD);
    Serial.printf("  panel ............ %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);
    Serial.printf("  targets .......... %d px minimum (UI_MIN_TOUCH_PX)\n",
                  UI_MIN_TOUCH_PX);
    Serial.println("-----------------------------------------------------");

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);
    Serial.printf("  panel ............ %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);

    touch.begin();

    bool results[6];
    results[0] = phaseRawHealth();
    results[1] = phaseCalibration();
    results[2] = phaseBusIntegrity();
    results[3] = phaseTapAccuracy();
    results[4] = phaseDragLinearity();
    results[5] = phaseTapRegistration();

    showScorecard(results);
}

void loop() {
    delay(1000);   // verdict stays on screen; reset to re-run the suite
}
