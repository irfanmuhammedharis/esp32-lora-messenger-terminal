#pragma once
//
// Tunables and hard limits for the LoRa Messenger Terminal.
//
// The one number here that is not a preference is MSG_MAX_LEN. It is dictated
// by MAX_PAYLOAD_LEN in the nRF52840 firmware (reference/nrf.cpp), which drops
// every character past it without complaint. Changing it here alone silently
// truncates messages on air.

#include <stdint.h>

// ── Link to the nRF52840 ────────────────────────────────────────────────────
#define NRF_UART_NUM        2
#define NRF_BAUD            115200
#define NRF_LINE_MAX        160     // longest line we will buffer from the nRF
                                    // (Zephyr log lines are wordy; anything
                                    //  longer is discarded up to the newline)

// Hard cap from reference/nrf.cpp:32 - MAX_PAYLOAD_LEN. Do not raise without
// raising it there too.
#define MSG_MAX_LEN         32

// If no line at all arrives from the nRF for this long, the UI shows the link
// as down. The node beacons every 10 s (reference/nrf.cpp:36), so silence past
// ~3 missed beacons is a real fault, not just a quiet channel.
#define NRF_LINK_TIMEOUT_MS 35000

// Outgoing lines waiting to go down the UART. At SF10 the radio needs a good
// fraction of a second per packet, so a user tapping presets can outrun it.
// A bounded queue makes that visible (send() fails) instead of silently
// growing; 8 is more than any realistic burst of deliberate presses.
#define NRF_TXQ_LEN         8

// Minimum gap between lines handed to the nRF. Its serial_poll() assembles a
// line per loop iteration between 250 ms listen slices, so pushing lines back
// to back just fills its buffer; spacing them costs nothing and keeps the
// node's own framing simple.
#define NRF_TX_GAP_MS       120

// ── Message store ───────────────────────────────────────────────────────────
#define MSG_HISTORY_LEN     32      // ring buffer; oldest is overwritten

// ── Touch (4-wire resistive, no controller, shares the LCD bus) ─────────────
// The panel is a bare resistive sheet: reading it means driving two of the
// four corners and measuring the voltage the divider produces at a third.
// There is no controller to debounce or calibrate for us.
#define TOUCH_ADC_MAX        4095   // 12-bit ESP32 ADC
#define TOUCH_SAMPLES        3      // averaged per axis per read
#define TOUCH_SETTLE_US      200    // after switching pin modes, before the ADC

// Pressure below this counts as "not touched". The plates read near zero when
// open, so this mostly rejects noise; Stage 2's raw phase prints live values
// so it can be set from what this specific panel actually does.
#define TOUCH_Z_THRESHOLD    350

// A saved calibration is only valid for the rotation it was taken in, so the
// stored version embeds it - rotating the display makes the old data fail to
// load rather than silently mapping taps to the wrong place.
#define TOUCH_NVS_NAMESPACE  "loraterm"
#define TOUCH_NVS_KEY        "touchcal"
#define TOUCH_CAL_VERSION    (0x54430200u | (TFT_ROTATION & 0x3))

// ── Display ─────────────────────────────────────────────────────────────────
// Portrait, 240x320 (PLAN.md section 2.1). The inbox gets more rows on
// screen; the cost is that a full 32-character message - the cap the radio
// imposes - does not fit on one 240 px line in the list font, so the list
// shows it truncated and the Detail screen wraps it.
//
// NOTE the numbering is the opposite way round to a real ILI9341, because
// this panel is an ILI9342 whose NATIVE orientation is 320x240 (landscape).
// Rotation 0 is therefore the landscape one here:
//
//   0 = landscape 320x240          2 = landscape 320x240, 180 deg
//   1 = portrait  240x320          3 = portrait  240x320, 180 deg
//
// If the content is the right shape but upside down, this wants 3, not 1.
// Stage 1c cycles all four live on an uncalibrated panel so the correct value
// can be read off the screen rather than guessed.
//
// Touch calibration is stored per orientation and is NOT transferable - the
// raw ADC axes stay fixed to the glass while the screen axes rotate under
// them. Changing this value invalidates the saved calibration, which is why
// TOUCH_CAL_VERSION below includes it.
#define TFT_ROTATION        1

// The shield's backlight is hardwired to its own 3.3V rail with no control
// pin on the Uno headers, so there is no dimming to be had. The idle timeout
// blanks the screen to black instead - which saves nothing on this hardware
// but does stop a lit panel giving away a position at night.
#define SCREEN_BLANK_AFTER_MS  60000

// ── Touch targets ───────────────────────────────────────────────────────────
// Minimum side of anything tappable. A fingertip on a resistive sheet is not
// precise and calibration error adds a few pixels on top, so this is a floor,
// not a suggestion - with no buttons left there is no second way to reach a
// control that turns out to be too small. On a 240 px-wide panel it allows
// one full-width action per row, which is why the presets are a single
// column rather than a 2x4 grid. PLAN.md section 4.1.
#define UI_MIN_TOUCH_PX     40

// ── Preset messages ─────────────────────────────────────────────────────────
// Every one of these must be <= MSG_MAX_LEN characters or it is truncated on
// air. test/test_logic_store/ asserts this on every preset - a runtime check
// rather than a static_assert, because these are const char* rather than
// constexpr and strlen() is not constant-evaluable across them.
// The persistent on-screen SOS control sends this one. It is on every screen
// because dropping the push buttons removed the out-of-band panic path that
// used to be a long press on OK (PLAN.md risk R4e).
#define PRESET_SOS_INDEX    0

static const char* const kPresetMessages[] = {
    "SOS",
    "IM HERE",
    "NEED REINFORCEMENT",
    "MEDIC NEEDED",
    "ALL CLEAR",
    "HOLD POSITION",
    "MOVING OUT",
    "RETURNING TO BASE",
};
static const uint8_t kPresetCount =
    sizeof(kPresetMessages) / sizeof(kPresetMessages[0]);
