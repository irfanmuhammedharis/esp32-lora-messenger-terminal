#pragma once
//
// 4-wire resistive touch panel on the 2.4" shield.
//
// There is no touch controller. The panel is two transparent resistive sheets
// facing each other, and its four corners are wired - inside the shield -
// straight onto four lines that are also the LCD's:
//
//     XP = LCD_D0 (GPIO13)      XM = LCD_RS (GPIO25)
//     YM = LCD_D1 (GPIO14)      YP = LCD_CS (GPIO26)
//
// Reading a coordinate means driving two opposite corners to make a voltage
// divider across one sheet, then measuring where the other sheet contacts it.
// That requires reconfiguring those four pins as analog inputs and outputs -
// which leaves the display bus in a state TFT_eSPI does not expect. Every
// public read here restores the bus before returning; nothing else in the
// project has to know that touch and display share pins.
//
// Because reads corrupt the bus mid-transaction, they must happen BETWEEN
// frames, never inside a draw. In practice this is free: LVGL polls input
// devices at the top of lv_timer_handler(), before any rendering.

#include <Arduino.h>

#include "app_config.h"   // TOUCH_CAL_VERSION, thresholds, sample counts

// Raw ADC readings, before calibration. x/y are 0..TOUCH_ADC_MAX in the
// panel's own axes, which stay fixed to the glass regardless of screen
// rotation. z is pressure: ~0 when open, higher the harder the press.
struct TouchRaw {
    uint16_t x;
    uint16_t y;
    uint16_t z;
};

// Maps raw ADC readings onto screen pixels for one specific rotation.
struct TouchCal {
    uint32_t version;                  // TOUCH_CAL_VERSION; embeds the rotation
    int32_t  xRawMin, xRawMax;         // raw values at screen x = 0 and w-1
    int32_t  yRawMin, yRawMax;         // raw values at screen y = 0 and h-1
    bool     swapAxes;                 // screen X is driven by the raw Y axis
    uint16_t zThreshold;
};

class Touch {
public:
    // Configures the ADC. Safe to call after tft.init().
    void begin();

    // One raw sample. Returns true if the panel is being pressed.
    // Restores the LCD bus before returning.
    bool readRaw(TouchRaw &out);

    // A calibrated sample in screen pixels for a w x h display.
    // Returns false if not pressed, or if no calibration is loaded.
    bool read(int32_t w, int32_t h, int32_t &sx, int32_t &sy);

    bool loadCal();                    // from NVS; false if absent or stale
    void saveCal() const;              // to NVS
    void clearCal();                   // forget it, forcing recalibration
    void setCal(const TouchCal &c) { cal_ = c; }
    const TouchCal &cal() const { return cal_; }
    bool calibrated() const { return cal_.version == TOUCH_CAL_VERSION; }

    // Derive a calibration from four corner samples taken at known screen
    // points. Works out on its own whether the raw axes are swapped or
    // inverted relative to the screen, so it is rotation-agnostic.
    // rawTL/TR/BR/BL must be raw samples taken at the four target points.
    static TouchCal solve(const TouchRaw &rawTL, const TouchRaw &rawTR,
                          const TouchRaw &rawBR, const TouchRaw &rawBL,
                          int32_t targetInset, int32_t w, int32_t h);

private:
    void restoreLcdBus() const;
    TouchCal cal_{};
};
