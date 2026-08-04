// ─────────────────────────────────────────────────────────────────────────────
// Stage 3 — touch as an LVGL input device.  pio run -e t3_touchui -t upload
//
// Stage 2 proved the panel reads and calibrates against TFT_eSPI directly.
// This stage proves the next thing up: that those readings drive real widgets
// through LVGL, that a target of UI_MIN_TOUCH_PX is genuinely hittable across
// the whole panel, and that a deliberate drag and a shaky tap are told apart.
//
// It replaces the old Stage 3, which brought up four push buttons. Those were
// removed from the design (PLAN.md section 5) after a floating GPIO34 fired a
// panic SOS on its own, so what used to be the button stage is now the stage
// where touch becomes the sole input.
//
// Exit criteria (PLAN.md stage 3):
//   - touch registered as LV_INDEV_TYPE_POINTER
//   - every target >= UI_MIN_TOUCH_PX is hit first time across the panel
//   - scroll and tap are distinguishable; no accidental scroll eats a tap
//   - an uncalibrated panel routes into calibration, not an unreachable UI
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

#include "Touch.h"
#include "TouchCal.h"
#include "app_config.h"
#include "lvgl_port.h"
#include "pins.h"

static TFT_eSPI tft;
static Touch    touch;

// A 3x5 grid of minimum-size targets covering the whole panel. Hit rate is
// counted per cell, because a resistive sheet is least linear at its edges -
// an average across the panel would hide exactly the failure that matters.
static constexpr uint8_t kCols = 3;
static constexpr uint8_t kRows = 5;

static uint16_t hits[kCols * kRows] = {0};
static uint32_t totalHits = 0;
static uint32_t scrollEvents = 0;
static lv_obj_t *statusLabel = nullptr;

static void onCellClicked(lv_event_t *e) {
    lv_obj_t *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const uintptr_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));

    hits[idx]++;
    totalHits++;

    // Recolour on hit, so coverage is readable off the panel itself rather
    // than only from the serial log.
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x15803d), LV_PART_MAIN);

    char buf[48];
    snprintf(buf, sizeof(buf), "hits %lu   scrolls %lu",
             (unsigned long)totalHits, (unsigned long)scrollEvents);
    if (statusLabel) lv_label_set_text(statusLabel, buf);

    Serial.printf("  cell %2u  (col %u row %u)  hits=%u  total=%lu\n",
                  (unsigned)idx, (unsigned)(idx % kCols), (unsigned)(idx / kCols),
                  hits[idx], (unsigned long)totalHits);
}

// A tap that turns into a scroll is the failure this stage is watching for:
// on a noisy sheet a stationary finger wanders, and if that wander exceeds
// LVGL's scroll threshold the tap is swallowed and the control never fires.
static void onScrolled(lv_event_t *) {
    scrollEvents++;
    Serial.printf("  scroll #%lu  (a tap became a drag - watch the ratio)\n",
                  (unsigned long)scrollEvents);
}

static void buildGrid() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, "TAP EVERY CELL");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x7dd3fc), LV_PART_MAIN);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);

    statusLabel = lv_label_create(scr);
    lv_label_set_text(statusLabel, "hits 0   scrolls 0");
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x94a3b8), LV_PART_MAIN);
    lv_obj_align(statusLabel, LV_ALIGN_BOTTOM_MID, 0, -4);

    const int32_t w = lv_display_get_horizontal_resolution(nullptr);
    const int32_t h = lv_display_get_vertical_resolution(nullptr);
    const int32_t cellW = w / kCols;
    const int32_t cellH = (h - 48) / kRows;

    for (uint8_t r = 0; r < kRows; r++) {
        for (uint8_t c = 0; c < kCols; c++) {
            const uint8_t idx = r * kCols + c;

            lv_obj_t *btn = lv_button_create(scr);
            lv_obj_set_size(btn, cellW - 4, cellH - 4);
            lv_obj_set_pos(btn, c * cellW + 2, 24 + r * cellH + 2);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a2230), LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2563eb),
                                      LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_radius(btn, 2, LV_PART_MAIN);
            lv_obj_set_user_data(btn, reinterpret_cast<void *>(
                                          static_cast<uintptr_t>(idx)));
            lv_obj_add_event_cb(btn, onCellClicked, LV_EVENT_CLICKED, nullptr);
            lv_obj_add_event_cb(btn, onScrolled, LV_EVENT_SCROLL_BEGIN, nullptr);

            char n[4];
            snprintf(n, sizeof(n), "%u", idx);
            lv_obj_t *l = lv_label_create(btn);
            lv_label_set_text(l, n);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_center(l);
        }
    }

    Serial.printf("  grid ............... %ux%u cells of %ldx%ld px "
                  "(minimum is %d)\n",
                  kCols, kRows, (long)(cellW - 4), (long)(cellH - 4),
                  UI_MIN_TOUCH_PX);
    if (cellW - 4 < UI_MIN_TOUCH_PX || cellH - 4 < UI_MIN_TOUCH_PX) {
        Serial.println("  WARNING: cells are below UI_MIN_TOUCH_PX - this grid");
        Serial.println("  is harder to hit than anything the real UI presents.");
    }
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 3 - touch as an LVGL input device");
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

    touch.begin();
    if (!touch.loadCal()) {
        Serial.println("  touch .............. uncalibrated -> calibrating now");
        while (!TouchCalUI::run(tft, touch)) {
            Serial.println("  rejected, retrying");
        }
    }

    if (!lvglPortInitTouch(touch)) {
        Serial.println("  FATAL: touch would not register - no input at all.");
        while (true) delay(1000);
    }
    Serial.println("  touch .............. active (LV_INDEV_TYPE_POINTER)");

    buildGrid();

    Serial.println("-----------------------------------------------------");
    Serial.println("  Tap every cell, including all four corners.");
    Serial.println("  A cell turns green once it has been hit.");
    Serial.println("  Scrolls should stay near zero: a tap that registers as");
    Serial.println("  a drag is a tap the real UI would have swallowed.");
    Serial.println("-----------------------------------------------------");
}

void loop() {
    static uint32_t lastReport = 0;
    lvglPortTask();

    const uint32_t now = millis();
    if (now - lastReport >= 10000) {
        lastReport = now;

        uint8_t covered = 0;
        for (uint8_t i = 0; i < kCols * kRows; i++) {
            if (hits[i]) covered++;
        }
        Serial.printf("  coverage %u/%u cells | %lu taps | %lu scrolls\n",
                      covered, kCols * kRows, (unsigned long)totalHits,
                      (unsigned long)scrollEvents);

        if (covered == kCols * kRows && scrollEvents * 4 < totalHits) {
            Serial.println("  Every cell hit and scrolls stayed rare:");
            Serial.println("  Stage 3 passes. Next: pio run -e t4_uart -t upload");
        }
    }
}
