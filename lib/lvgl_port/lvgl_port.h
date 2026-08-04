#pragma once
//
// LVGL <-> hardware glue for the LoRa Messenger Terminal.
//
// Everything platform-specific about driving LVGL lives here: the draw
// buffer, the flush callback that pushes pixels through TFT_eSPI, and the
// millisecond tick. Screens built on top of this never touch TFT_eSPI, and
// never need to know the panel is on a parallel bus.
//
// There is exactly one input device: the resistive panel, registered as an
// LV_INDEV_TYPE_POINTER by lvglPortInitTouch(). The push buttons that were
// once going to add a keypad were dropped from the design (PLAN.md 4.1).

#include <TFT_eSPI.h>
#include <lvgl.h>

#include "Touch.h"

// Height of the draw buffer in scanlines. LVGL renders the screen in
// horizontal strips this tall and calls the flush callback per strip; it is
// the main RAM/latency knob. LVGL wants >= 1/10 of the screen: ~32 lines in
// portrait (240x320), ~24 in landscape, so 40 covers both with headroom. The
// buffer is sized from tft.width(), so it tracks whichever rotation is set.
#ifndef LVGL_BUF_LINES
#define LVGL_BUF_LINES 40
#endif

// Bring up LVGL against an already-initialised TFT_eSPI instance.
// Call tft.init() and tft.setRotation() BEFORE this - the port reads the
// panel's dimensions from the driver rather than assuming them, so that a
// rotation change cannot silently desynchronise LVGL from the display.
// Returns false if the draw buffer could not be allocated.
bool lvglPortInit(TFT_eSPI &tft);

// Pump LVGL. Call from loop() as often as possible; it returns the number of
// milliseconds until it next wants to be called.
uint32_t lvglPortTask();

// Size of the allocated draw buffer, in bytes. For reporting.
size_t lvglPortDrawBufBytes();

// Union of every area passed to the flush callback since the last reset.
//
// Answers "is the whole panel actually being painted?" with a number instead
// of a squint. It separates the two things that look identical on the bench:
// a layout that does not reach the edges (coverage is full, pixels are just
// background) from a port that is not addressing the full panel (coverage is
// short). Cheap enough to leave compiled in.
void lvglPortResetCoverage();
void lvglPortGetCoverage(int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2,
                         uint32_t &flushCount);

// Register the resistive panel as an LV_INDEV_TYPE_POINTER.
//
// The Touch object must outlive the port (a global or a static), and must
// already hold a valid calibration - run Stage 2 first. Returns false and
// registers nothing if it is uncalibrated, so an uncalibrated device shows a
// UI that simply ignores touch rather than one that jumps to wrong widgets.
bool lvglPortInitTouch(Touch &touch);

// True once lvglPortInitTouch() has succeeded.
bool lvglPortTouchActive();

// millis() of the last press the panel actually reported, swallowed or not.
// With no buttons left this is the only evidence of user activity, so it is
// what drives the idle-blank timer.
uint32_t lvglPortLastTouchMs();

// Keep reading the panel but report every sample as released.
//
// This is how the blanked screen wakes without acting: the tap still updates
// lvglPortLastTouchMs(), so the UI knows to un-blank, but LVGL never sees a
// press and cannot activate whatever happened to be under a finger on a panel
// the operator could not see. Disabling the indev outright would swallow the
// wake evidence along with the press.
void lvglPortSetTouchSwallow(bool swallow);

// There is no keypad input device. The four push buttons were removed from
// the design (PLAN.md section 4.1), and with only a pointer left LVGL's focus
// model earns nothing: a tap addresses a widget directly, so focus never has
// to be moved to it first. No lv_group_t, no key translation, no second read
// callback - which is the main dividend of going touch-only.
