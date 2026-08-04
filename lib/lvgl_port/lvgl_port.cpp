#include "lvgl_port.h"

#include <Arduino.h>

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

static void touchReadCb(lv_indev_t *, lv_indev_data_t *data) {
    static int32_t lastX = 0, lastY = 0;

    int32_t x, y;
    if (s_touch->read(lv_display_get_horizontal_resolution(nullptr),
                      lv_display_get_vertical_resolution(nullptr), x, y)) {
        lastX = x;
        lastY = y;
        s_lastTouchMs = millis();
        data->state = s_swallow ? LV_INDEV_STATE_RELEASED
                                : LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = lastX;
    data->point.y = lastY;
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

    // A resistive sheet read through an ADC is noisy, and a bare finger on
    // glass wanders. The default 10 px scroll threshold turns that jitter into
    // accidental scrolls that swallow taps; 18 px is enough to keep a
    // deliberate drag feeling immediate while a shaky tap stays a tap.
    lv_indev_set_scroll_limit(s_indev, 18);

    return true;
}

bool lvglPortTouchActive() { return s_indev != nullptr; }
