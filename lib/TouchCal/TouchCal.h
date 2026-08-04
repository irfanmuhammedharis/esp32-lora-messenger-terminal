#pragma once
//
// The touch calibration flow, as a reusable routine.
//
// It lives here rather than inside the Stage 2 app because touch is now the
// only input (PLAN.md section 4.1), which makes calibration a boot path, not
// a bring-up step: an uncalibrated panel cannot reach a single widget, so a
// UI drawn on one is simply unreachable. Risk R4d. Every app that puts up a
// UI therefore has to be able to run this itself.
//
// It draws with TFT_eSPI directly and never touches LVGL. That is what makes
// it usable when nothing else is: it works in raw ADC space and asks for taps
// at known screen coordinates, so it needs no calibration to run and no
// widget tree to receive events.

#include <TFT_eSPI.h>

#include "Touch.h"

namespace TouchCalUI {

// Targets sit this far in from each edge. A resistive panel is least linear
// at its very edge and a corner cannot be tapped reliably, so the routine
// samples inside the corners and extrapolates outward.
static constexpr int32_t kInset = 30;

// Run the four-target sequence, solve, store into `touch` and persist to NVS.
//
// Blocks until all four targets have been tapped - deliberately, because
// there is nothing useful the caller can do in the meantime and no other
// input to service. Returns false only if the result fails its own sanity
// check, in which case nothing is saved and the caller should retry.
bool run(TFT_eSPI &tft, Touch &touch);

// Draw where the user touches, so the mapping can be judged by eye. Returns
// when `timeoutMs` has elapsed. Used by Stage 2 and after a boot-time
// calibration, so a bad calibration is caught immediately rather than by
// discovering that nothing on the UI responds.
void verify(TFT_eSPI &tft, Touch &touch, uint32_t timeoutMs);

}  // namespace TouchCalUI
