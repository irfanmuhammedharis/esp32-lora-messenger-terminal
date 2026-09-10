//
// LVGL v9.5 configuration for the LoRa Messenger Terminal.
//
// ESP32-WROOM-32 (no PSRAM) + 2.4" ILI9342 (320x240 native, driven portrait
// at 240x320) on an 8-bit parallel bus.
//
// Only the settings that differ from LVGL's defaults are listed. Everything
// else falls through to src/lv_conf_internal.h, which supplies a default for
// every option - so this file stays short enough to actually review.
//
// Reached by -DLV_CONF_INCLUDE_SIMPLE in platformio.ini, which makes LVGL do
// a plain #include "lv_conf.h"; PlatformIO already has include/ on the path.
//
// ── Memory budget (measured free heap at Stage 1b: ~350 KB) ─────────────────
//   draw buffer     240 x 40 px x 2 B  =  19.2 KB   (in lvgl_port.cpp)
//   LV_MEM_SIZE                        =  48   KB
//                                        ---------
//                                          67.2 KB, leaving ~280 KB
// The draw buffer is sized from tft.width(), so it follows the rotation:
// 19.2 KB in portrait, 25.6 KB in landscape. This panel is an ILI9342, whose
// native orientation is landscape, so PORTRAIT is TFT_ROTATION 1 here - the
// opposite of a real ILI9341. See include/app_config.h.
// The message store, UART buffers and the UI's own objects all come out of
// that remainder, so there is a lot of room - but LV_MEM_SIZE is the number
// to raise first if lv_mem_monitor() shows the pool filling up.

#ifndef LV_CONF_H
#define LV_CONF_H

// ── Colour ──────────────────────────────────────────────────────────────────
// RGB565 to match the panel. Note that in v9 lv_color_t is a 3-byte RGB888
// struct regardless of this setting - it describes the *render* format, not
// the type. Draw buffers must be sized in bytes, never with sizeof(lv_color_t).
#define LV_COLOR_DEPTH 16

// ── Memory ──────────────────────────────────────────────────────────────────
// LVGL's own pool rather than the Arduino heap: a fixed pool cannot fragment
// the system heap, and lv_mem_monitor() gives a number we can actually watch.
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
#define LV_MEM_SIZE             (48 * 1024U)

// ── Timing ──────────────────────────────────────────────────────────────────
// 30 fps target. Stage 1b measured 51.7 fps for *full-screen* fills, and LVGL
// only ever redraws dirty rectangles, so this is comfortable.
#define LV_DEF_REFR_PERIOD      33

// No RTOS - lv_timer_handler() is pumped from loop().
#define LV_USE_OS               LV_OS_NONE

// The tick comes from millis() via lv_tick_set_cb() in lvgl_port.cpp.

// ── Geometry ────────────────────────────────────────────────────────────────
// sqrt(240^2 + 320^2) / 2.4" = 400 px / 2.4" = 167 dpi. LVGL sizes padding,
// scrollbars and default widget dimensions from this, so a wrong value here
// makes every stock widget subtly the wrong size on a small panel.
#define LV_DPI_DEF              167

// ── Rendering ───────────────────────────────────────────────────────────────
#define LV_DRAW_BUF_ALIGN       4

// ── Fonts ───────────────────────────────────────────────────────────────────
// 14 is the default body font; 16 for list rows, 20 for titles, 28 for the
// SOS alert. Each one costs flash, so this list is deliberately short.
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_28   1

// ── Widgets ─────────────────────────────────────────────────────────────────
// LVGL enables everything by default. Turning off what this UI will never use
// keeps the build small and, more usefully, keeps the API surface small
// enough to reason about. Re-enable individually if a screen needs one.
#define LV_USE_CANVAS           0
// The Vitals screen's PPG sparkline (PLAN.md 4.1) is an lv_chart, so this is
// the one widget beyond the default set this UI keeps enabled.
#define LV_USE_CHART            1
#define LV_USE_ANIMIMG          0
#define LV_USE_CALENDAR         0
#define LV_USE_TABVIEW          0
#define LV_USE_TILEVIEW         0
#define LV_USE_WIN              0
#define LV_USE_SPAN             0
#define LV_USE_SPINBOX          0
#define LV_USE_IMAGEBUTTON      0
#define LV_USE_TABLE            0
#define LV_USE_LED              0
#define LV_USE_MENU             0
#define LV_USE_SCALE            0
#define LV_USE_ARC              0
#define LV_USE_SPINNER          0   // requires ARC, so must go with it
#define LV_USE_ROLLER           0
#define LV_USE_SLIDER           0
#define LV_USE_SWITCH           0
#define LV_USE_CHECKBOX         0
#define LV_USE_DROPDOWN         0

// Widget dependencies LVGL enforces with #error, worth knowing before
// trimming further: spinner->arc, slider->bar, spinbox->textarea,
// dropdown/table/roller/image->label, animimg->image, list/menu->flex.

// Kept on, because the planned screens need them:
//   LABEL/BUTTON/BUTTONMATRIX  everywhere
//   LIST                       inbox
//   TEXTAREA + KEYBOARD        compose screen (32-char limit)
//   MSGBOX                     send confirmation, SOS alert
//   BAR                        RSSI strength, TX queue depth
//   IMAGE                      required by LIST for its row icons

// ── Theme ───────────────────────────────────────────────────────────────────
// Dark theme: this is a field device, and a bright panel at night is a
// liability. LV_USE_THEME_SIMPLE stays off - the default theme is what the
// widgets are designed against.
#define LV_USE_THEME_DEFAULT    1
#define LV_THEME_DEFAULT_DARK   1
#define LV_USE_THEME_SIMPLE     0

// ── Logging ─────────────────────────────────────────────────────────────────
// On during bring-up, at WARN so it does not drown the stage diagnostics.
// printf lands on UART0, which is the same USB serial we already watch.
// Turn LV_USE_LOG to 0 for the field build.
#define LV_USE_LOG              1
#define LV_LOG_LEVEL            LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF           1

// ── Asserts ─────────────────────────────────────────────────────────────────
// A failed allocation must be loud. Silently dropping a widget on a device
// whose whole job is showing emergency messages is the worst failure mode
// available, so this one stays on even in the field build.
#define LV_USE_ASSERT_NULL      1
#define LV_USE_ASSERT_MALLOC    1
#define LV_USE_ASSERT_STYLE     0
#define LV_USE_ASSERT_OBJ       0

// ── Not building LVGL's own demos or examples ───────────────────────────────
#define LV_BUILD_EXAMPLES       0

#endif /*LV_CONF_H*/
