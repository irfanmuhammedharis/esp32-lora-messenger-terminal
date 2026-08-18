#include "TouchCal.h"

#include <Arduino.h>

#include "app_config.h"

namespace TouchCalUI {

static void drawTarget(TFT_eSPI &tft, int32_t x, int32_t y, uint16_t colour) {
    tft.drawCircle(x, y, 12, colour);
    tft.drawCircle(x, y, 4, colour);
    tft.drawFastHLine(x - 18, y, 36, colour);
    tft.drawFastVLine(x, y - 18, 36, colour);
}

// Wait for a press that holds still, rather than taking the first sample.
//
// The first reading after contact is always the noisiest - the two sheets are
// still settling - and a calibration built from four of those is visibly off
// everywhere. Requiring eight consecutive samples within 150 counts of the
// running mean costs the user nothing and removes the whole class of "the
// crosshair is consistently 8 px high" complaints.
static bool awaitStableTap(TFT_eSPI &tft, Touch &touch, int32_t tx, int32_t ty,
                           const char *label, TouchRaw &out) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(label, tft.width() / 2, 8, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("tap the centre, then lift", tft.width() / 2, 36, 2);
    tft.setTextDatum(TL_DATUM);
    drawTarget(tft, tx, ty, TFT_YELLOW);

    Serial.printf("    tap %-13s (%3ld,%3ld) ... ", label, (long)tx, (long)ty);

    while (true) {
        TouchRaw r;
        if (!touch.readRaw(r)) { delay(10); continue; }

        uint32_t sx = 0, sy = 0;
        uint8_t got = 0;
        bool steady = true;
        for (uint8_t i = 0; i < 8 && steady; i++) {
            TouchRaw s;
            if (!touch.readRaw(s)) { steady = false; break; }
            if (got && (abs((int)s.x - (int)(sx / got)) > 150 ||
                        abs((int)s.y - (int)(sy / got)) > 150)) {
                steady = false;
                break;
            }
            sx += s.x; sy += s.y; got++;
            delay(8);
        }
        if (!steady || got < 8) continue;

        out.x = static_cast<uint16_t>(sx / got);
        out.y = static_cast<uint16_t>(sy / got);
        out.z = r.z;
        break;
    }

    drawTarget(tft, tx, ty, TFT_GREEN);
    Serial.printf("raw x=%4u y=%4u\n", out.x, out.y);

    // Wait for release, or the next target captures the same press.
    while (true) {
        TouchRaw r;
        if (!touch.readRaw(r)) break;
        delay(10);
    }
    delay(250);
    return true;
}

bool run(TFT_eSPI &tft, Touch &touch) {
    const int32_t w = tft.width(), h = tft.height();

    Serial.println();
    Serial.println("  [calibration] tap each of the four targets squarely.");

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("CALIBRATION", w / 2, h / 2 - 30, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("tap the four targets", w / 2, h / 2 + 4, 2);
    tft.setTextDatum(TL_DATUM);
    delay(1400);

    TouchRaw tl, tr, br, bl;
    awaitStableTap(tft, touch, kInset,         kInset,         "TOP-LEFT", tl);
    awaitStableTap(tft, touch, w - 1 - kInset, kInset,         "TOP-RIGHT", tr);
    awaitStableTap(tft, touch, w - 1 - kInset, h - 1 - kInset, "BOTTOM-RIGHT", br);
    awaitStableTap(tft, touch, kInset,         h - 1 - kInset, "BOTTOM-LEFT", bl);

    // Axis cross-check BEFORE trusting the solve. The top edge (TL->TR, only
    // screen X varies) and the left edge (TL->BL, only screen Y varies) must
    // both identify the same orientation. A disagreement means at least one
    // of the four taps was sloppy - saving that would map taps to the wrong
    // place (the exact fault this fix is for) even though the solved ranges
    // look plausible. Reject and let the user retry rather than persist it.
    {
        const int32_t dxTop  = abs((int32_t)tr.x - (int32_t)tl.x);
        const int32_t dyTop  = abs((int32_t)tr.y - (int32_t)tl.y);
        const int32_t dxLeft = abs((int32_t)bl.x - (int32_t)tl.x);
        const int32_t dyLeft = abs((int32_t)bl.y - (int32_t)tl.y);
        const bool swapTop  = dyTop > dxTop;
        const bool swapLeft = dxLeft > dyLeft;
        if (swapTop != swapLeft) {
            Serial.printf("    REJECTED: axis evidence disagrees (top %s,"
                          " left %s)\n", swapTop ? "swapped" : "normal",
                          swapLeft ? "swapped" : "normal");
            Serial.println("    Tap all four targets more squarely, then retry.");
            tft.fillScreen(TFT_RED);
            tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("TAPS OFF - RETRY", w / 2, h / 2, 4);
            tft.setTextDatum(TL_DATUM);
            delay(2000);
            return false;
        }
    }

    const TouchCal c = Touch::solve(tl, tr, br, bl, kInset, w, h);

    // Sanity check before trusting it. A span this small means the four taps
    // did not actually differ - a stuck sheet, or someone tapping the same
    // spot four times - and saving it would produce a calibration that maps
    // the whole panel onto a few pixels, which is indistinguishable from
    // dead touch and much harder to diagnose.
    const int32_t spanX = abs(c.xRawMax - c.xRawMin);
    const int32_t spanY = abs(c.yRawMax - c.yRawMin);
    if (spanX < 200 || spanY < 200) {
        Serial.printf("    REJECTED: raw spans too small (x %ld, y %ld)\n",
                      (long)spanX, (long)spanY);
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("CALIBRATION FAILED", w / 2, h / 2, 4);
        tft.setTextDatum(TL_DATUM);
        delay(2000);
        return false;
    }

    touch.setCal(c);
    touch.saveCal();

    Serial.printf("    swapAxes %s | x %ld->%ld | y %ld->%ld  saved to NVS\n",
                  c.swapAxes ? "yes" : "no",
                  (long)c.xRawMin, (long)c.xRawMax,
                  (long)c.yRawMin, (long)c.yRawMax);
    return true;
}

void verify(TFT_eSPI &tft, Touch &touch, uint32_t timeoutMs) {
    const int32_t w = tft.width(), h = tft.height();

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("VERIFY", w / 2, 6, 4);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("draw - corners matter most", w / 2, 34, 2);
    tft.setTextDatum(TL_DATUM);
    tft.drawRect(0, 0, w, h, TFT_DARKGREY);

    Serial.println("  [verify] the dot should sit under your fingertip.");

    const uint32_t t0 = millis();
    uint32_t lastPrint = 0;
    while (millis() - t0 < timeoutMs) {
        int32_t x, y;
        if (!touch.read(w, h, x, y)) { delay(8); continue; }

        tft.fillCircle(x, y, 3, TFT_GREEN);
        if (millis() - lastPrint >= 300) {
            lastPrint = millis();
            char buf[32];
            snprintf(buf, sizeof(buf), "x=%3ld y=%3ld ", (long)x, (long)y);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString(buf, 6, h - 22, 2);
        }
    }
}

}  // namespace TouchCalUI
