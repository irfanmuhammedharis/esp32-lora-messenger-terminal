#include "lvgl_port.h"

#include <Arduino.h>

#include "app_config.h"

// Prove include/lv_conf.h is actually the config in force.
//
// LVGL falls back to a full set of built-in defaults if it cannot find
// lv_conf.h, so a misplaced config does not fail the build - it silently
// compiles a differently-configured library. That would show up much later as
// a wrong colour depth or an exhausted memory pool. These three values differ
// from LVGL's defaults, so they only hold if our file was read.
static_assert(LV_COLOR_DEPTH == 16, "lv_conf.h not in effect: LV_COLOR_DEPTH");
static_assert(LV_MEM_SIZE == (48 * 1024U), "lv_conf.h not in effect: LV_MEM_SIZE");
static_assert(LV_DPI_DEF == 167, "lv_conf.h not in effect: LV_DPI_DEF");

static TFT_eSPI     *s_tft  = nullptr;
static lv_display_t *s_disp = nullptr;
static uint8_t      *s_buf  = nullptr;
static size_t        s_bufBytes = 0;

// RGB565 is 2 bytes per pixel.
//
// This deserves a comment because it is the one thing that reliably breaks a
// port from LVGL v8: in v9 `lv_color_t` is a 3-byte RGB888 struct no matter
// what LV_COLOR_DEPTH says. Sizing this buffer with sizeof(lv_color_t) - as
// every v8 example does - allocates 50% too much and then hands LVGL a stride
// it does not agree with, which shows up as skewed or torn output rather than
// as a clean failure. Always size draw buffers from the colour *format*.
static constexpr size_t kBytesPerPx = 2;

// ── Tick ────────────────────────────────────────────────────────────────────
// v9 can pull the time itself rather than having lv_tick_inc() pushed at it
// from a timer ISR. Fewer moving parts, and no chance of the tick stalling
// because loop() blocked somewhere.
static uint32_t tickCb() { return millis(); }

// ── Flush ───────────────────────────────────────────────────────────────────
// Push one rendered strip to the panel.
//
// The `true` on pushColors is the byte swap: LVGL renders RGB565 in the
// ESP32's native little-endian order, while the ILI9341 wants each pixel
// most-significant byte first. Getting this wrong does not fail loudly - the
// image appears with wildly wrong colours, which is easy to misread as the
// BGR panel-order problem from Stage 1b. They are different faults: this one
// scrambles colours per-pixel, BGR order swaps red and blue cleanly.
static int32_t  s_covX1 = INT32_MAX, s_covY1 = INT32_MAX;
static int32_t  s_covX2 = INT32_MIN, s_covY2 = INT32_MIN;
static uint32_t s_covFlushes = 0;

void lvglPortResetCoverage() {
    s_covX1 = s_covY1 = INT32_MAX;
    s_covX2 = s_covY2 = INT32_MIN;
    s_covFlushes = 0;
}

void lvglPortGetCoverage(int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2,
                         uint32_t &flushCount) {
    x1 = s_covX1; y1 = s_covY1; x2 = s_covX2; y2 = s_covY2;
    flushCount = s_covFlushes;
}

static void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint32_t w = lv_area_get_width(area);
    const uint32_t h = lv_area_get_height(area);

    if (area->x1 < s_covX1) s_covX1 = area->x1;
    if (area->y1 < s_covY1) s_covY1 = area->y1;
    if (area->x2 > s_covX2) s_covX2 = area->x2;
    if (area->y2 > s_covY2) s_covY2 = area->y2;
    s_covFlushes++;

    s_tft->startWrite();
    s_tft->setAddrWindow(area->x1, area->y1, w, h);
    s_tft->pushColors(reinterpret_cast<uint16_t *>(px_map), w * h, true);
    s_tft->endWrite();

    // The parallel bus has no DMA, so pushColors() has already finished by the
    // time it returns and this can be signalled immediately. A DMA-capable
    // transport would instead call this from the transfer-complete interrupt.
    lv_display_flush_ready(disp);
}

bool lvglPortInit(TFT_eSPI &tft) {
    s_tft = &tft;

    lv_init();
    lv_tick_set_cb(tickCb);

    // Take the resolution from the driver rather than from a constant, so a
    // rotation set by the caller cannot leave LVGL and the panel disagreeing.
    const int32_t hor = tft.width();
    const int32_t ver = tft.height();

    s_bufBytes = static_cast<size_t>(hor) * LVGL_BUF_LINES * kBytesPerPx;
    s_buf = static_cast<uint8_t *>(malloc(s_bufBytes));
    if (!s_buf) {
        s_bufBytes = 0;
        return false;
    }

    s_disp = lv_display_create(hor, ver);
    if (!s_disp) {
        free(s_buf);
        s_buf = nullptr;
        s_bufBytes = 0;
        return false;
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flushCb);

    // PARTIAL: render in strips through one small buffer. DIRECT and FULL
    // both want a screen-sized buffer - 150 KB at 240x320x2 - which would eat
    // half the heap on a WROOM for no benefit, since the bus is synchronous
    // and there is nothing to overlap the second buffer with.
    lv_display_set_buffers(s_disp, s_buf, nullptr, s_bufBytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    return true;
}

uint32_t lvglPortTask() { return lv_timer_handler(); }

size_t lvglPortDrawBufBytes() { return s_bufBytes; }

// ── Touch input device ──────────────────────────────────────────────────────

static Touch      *s_touch = nullptr;
static lv_indev_t *s_indev = nullptr;

// LVGL polls this; it must report the *current* state, and on release it must
// still report the last known coordinate. Reporting (0,0) on release would
// make every lift-off look like a drag to the top-left corner, which cancels
// taps and throws scrolls in the wrong direction.
static uint32_t s_lastTouchMs = 0;
static bool     s_swallow     = false;

// ── Press confirmation and smoothing ────────────────────────────────────────
//
// Nothing is reported to LVGL until TOUCH_CONFIRM_READS consecutive reads
// agree within TOUCH_JITTER_PX of each other. This is what stops wrong taps.
//
// The reason it is needed: the first reading after contact is always the
// worst one. The two sheets are still settling, contact resistance is still
// falling, and the coordinate it produces can be anywhere on the panel. Report
// that sample and LVGL activates whatever widget it happened to land on -
// which is indistinguishable, from the operator's side, from the UI being
// wired up wrong. Holding fire for one extra poll (~30 ms, imperceptible)
// throws that sample away before it can do damage.
//
// Release is debounced the other way: a resistive sheet loses contact briefly
// mid-press, especially near the edges, and without TOUCH_RELEASE_READS a
// single dropout splits one tap into two or ends a drag halfway.
static int32_t s_x = 0, s_y = 0;        // reported coordinate (smoothed)
static int32_t s_candX = 0, s_candY = 0; // candidate being confirmed
static uint8_t s_confirm  = 0;
static uint8_t s_released = 0;
static bool    s_pressed  = false;       // currently reporting PRESSED

static void touchReadCb(lv_indev_t *, lv_indev_data_t *data) {
    int32_t x, y;
    const bool raw = s_touch->read(lv_display_get_horizontal_resolution(nullptr),
                                   lv_display_get_vertical_resolution(nullptr),
                                   x, y);

    if (raw) {
        // Activity is recorded on the RAW read, before confirmation, so the
        // idle-blank timer wakes on the very first contact even though that
        // sample is never reported as a press.
        s_lastTouchMs = millis();
        s_released = 0;

        if (s_pressed) {
            // Exponential smoothing while held. Flattens the wander a bare
            // finger produces, which is what would otherwise cross LVGL's
            // scroll threshold and turn a tap into a swallowed drag.
            s_x = (x + (TOUCH_SMOOTHING_DEN - 1) * s_x) / TOUCH_SMOOTHING_DEN;
            s_y = (y + (TOUCH_SMOOTHING_DEN - 1) * s_y) / TOUCH_SMOOTHING_DEN;
        } else if (s_confirm == 0) {
            s_candX = x;
            s_candY = y;
            s_confirm = 1;
        } else if (abs(x - s_candX) <= TOUCH_JITTER_PX &&
                   abs(y - s_candY) <= TOUCH_JITTER_PX) {
            s_candX = (s_candX + x) / 2;
            s_candY = (s_candY + y) / 2;
            if (++s_confirm >= TOUCH_CONFIRM_READS) {
                s_pressed = true;
                s_x = s_candX;
                s_y = s_candY;
            }
        } else {
            // Too far from the candidate to be the same contact. Start over
            // rather than averaging two unrelated points into a third that
            // matches neither - the classic way a filter invents a tap
            // halfway between two real ones.
            s_candX = x;
            s_candY = y;
            s_confirm = 1;
        }
    } else {
        s_confirm = 0;
        if (s_pressed && ++s_released >= TOUCH_RELEASE_READS) {
            s_pressed  = false;
            s_released = 0;
        }
    }

    data->state = (s_pressed && !s_swallow) ? LV_INDEV_STATE_PRESSED
                                            : LV_INDEV_STATE_RELEASED;
    // On release LVGL must still see the last known point. Reporting (0,0)
    // would make every lift-off look like a drag to the top-left corner,
    // cancelling taps and throwing scrolls the wrong way.
    data->point.x = s_x;
    data->point.y = s_y;
}

uint32_t lvglPortLastTouchMs() { return s_lastTouchMs; }

void lvglPortSetTouchSwallow(bool swallow) { s_swallow = swallow; }

bool lvglPortInitTouch(Touch &touch) {
    if (!touch.calibrated()) return false;

    s_touch = &touch;
    s_indev = lv_indev_create();
    if (!s_indev) return false;

    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touchReadCb);
    lv_indev_set_display(s_indev, s_disp);

    // The default 10 px scroll threshold turns finger wander into accidental
    // scrolls that swallow taps. The smoothing in touchReadCb() already
    // removes most of that, but this stays generous: a tap misread as a drag
    // is silently discarded, which is the worse of the two failure modes.
    lv_indev_set_scroll_limit(s_indev, TOUCH_SCROLL_LIMIT_PX);

    return true;
}

bool lvglPortTouchActive() { return s_indev != nullptr; }
