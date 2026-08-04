#include "Touch.h"

#include <Preferences.h>

#include "app_config.h"
#include "pins.h"

// All four touch corners land on ADC2 channels (GPIO13/14/25/26 are ADC2
// channels 4/6/8/9). ADC2 is unusable while WiFi is active - a real
// constraint, but free here because this device never brings WiFi up. Anyone
// adding WiFi later must move to ADC1 pins, which the parallel bus has
// already spent, so it would mean giving up the touch panel.
static constexpr uint8_t kXP = PIN_TOUCH_XP;   // LCD_D0, GPIO13
static constexpr uint8_t kXM = PIN_TOUCH_XM;   // LCD_RS, GPIO25
static constexpr uint8_t kYP = PIN_TOUCH_YP;   // LCD_CS, GPIO26
static constexpr uint8_t kYM = PIN_TOUCH_YM;   // LCD_D1, GPIO14

void Touch::begin() {
    // 11 dB attenuation gives the ADC the full 0-3.3 V span. On the default
    // 0 dB the input saturates around 1.1 V, so a press near one edge of the
    // panel would read as pegged and the calibration would collapse.
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);
    restoreLcdBus();
}

// Put the four shared pins back the way TFT_eSPI's parallel driver expects:
// data lines as outputs, and both control lines idle HIGH. Skipping this
// leaves CS or RS as an analog input, and the next draw writes commands into
// a display that is not listening - which shows up as a frozen or garbled
// screen some time later, with nothing pointing back at the touch code.
void Touch::restoreLcdBus() const {
    pinMode(PIN_LCD_D0, OUTPUT);
    pinMode(PIN_LCD_D1, OUTPUT);
    pinMode(PIN_LCD_RS, OUTPUT);
    digitalWrite(PIN_LCD_RS, HIGH);
    pinMode(PIN_LCD_CS, OUTPUT);
    digitalWrite(PIN_LCD_CS, HIGH);
}

static uint16_t averaged(uint8_t pin) {
    uint32_t acc = 0;
    for (uint8_t i = 0; i < TOUCH_SAMPLES; i++) acc += analogRead(pin);
    return static_cast<uint16_t>(acc / TOUCH_SAMPLES);
}

bool Touch::readRaw(TouchRaw &out) {
    // ── X: drive a gradient across the X sheet, measure it on the Y sheet ───
    pinMode(kYP, INPUT);
    pinMode(kYM, INPUT);
    pinMode(kXP, OUTPUT); digitalWrite(kXP, HIGH);
    pinMode(kXM, OUTPUT); digitalWrite(kXM, LOW);
    delayMicroseconds(TOUCH_SETTLE_US);
    const uint16_t x = averaged(kYP);

    // ── Y: the same, with the roles of the two sheets exchanged ────────────
    pinMode(kXP, INPUT);
    pinMode(kXM, INPUT);
    pinMode(kYP, OUTPUT); digitalWrite(kYP, HIGH);
    pinMode(kYM, OUTPUT); digitalWrite(kYM, LOW);
    delayMicroseconds(TOUCH_SETTLE_US);
    const uint16_t y = averaged(kXM);

    // ── Z: how hard. Drive XP low and YM high, then measure how far the two
    // sheets have dragged each other's floating corners together. Open, they
    // sit at opposite rails and the difference is the full scale; pressed,
    // the contact resistance pulls them toward each other.
    pinMode(kXP, OUTPUT); digitalWrite(kXP, LOW);
    pinMode(kYM, OUTPUT); digitalWrite(kYM, HIGH);
    pinMode(kXM, INPUT);
    pinMode(kYP, INPUT);
    delayMicroseconds(TOUCH_SETTLE_US);
    const int32_t z1 = analogRead(kXM);
    const int32_t z2 = analogRead(kYP);
    int32_t z = TOUCH_ADC_MAX - (z2 - z1);
    if (z < 0) z = 0;
    if (z > TOUCH_ADC_MAX) z = TOUCH_ADC_MAX;

    restoreLcdBus();

    out.x = x;
    out.y = y;
    out.z = static_cast<uint16_t>(z);

    const uint16_t thr = cal_.zThreshold ? cal_.zThreshold : TOUCH_Z_THRESHOLD;
    return out.z > thr;
}

bool Touch::read(int32_t w, int32_t h, int32_t &sx, int32_t &sy) {
    if (!calibrated()) return false;

    TouchRaw r;
    if (!readRaw(r)) return false;

    const int32_t a = cal_.swapAxes ? r.y : r.x;
    const int32_t b = cal_.swapAxes ? r.x : r.y;

    // map() handles an inverted range (min > max) correctly, which is how a
    // panel mounted "backwards" relative to the screen is absorbed without a
    // separate inversion flag.
    sx = map(a, cal_.xRawMin, cal_.xRawMax, 0, w - 1);
    sy = map(b, cal_.yRawMin, cal_.yRawMax, 0, h - 1);
    sx = constrain(sx, 0, w - 1);
    sy = constrain(sy, 0, h - 1);
    return true;
}

TouchCal Touch::solve(const TouchRaw &tl, const TouchRaw &tr,
                      const TouchRaw &br, const TouchRaw &bl,
                      int32_t inset, int32_t w, int32_t h) {
    TouchCal c{};
    c.version    = TOUCH_CAL_VERSION;
    c.zThreshold = TOUCH_Z_THRESHOLD;

    // Which raw axis tracks the screen's X? Compare how much each raw axis
    // moved along an edge where only screen X changed (top-left -> top-right).
    // Whichever moved more is the one carrying X. This is what makes the
    // routine rotation-agnostic: no assumption about how the glass is mounted.
    const int32_t dxAlongTop = abs((int32_t)tr.x - (int32_t)tl.x);
    const int32_t dyAlongTop = abs((int32_t)tr.y - (int32_t)tl.y);
    c.swapAxes = dyAlongTop > dxAlongTop;

    // Average the two samples on each edge to halve the effect of a sloppy tap.
    auto axisA = [&](const TouchRaw &r) { return (int32_t)(c.swapAxes ? r.y : r.x); };
    auto axisB = [&](const TouchRaw &r) { return (int32_t)(c.swapAxes ? r.x : r.y); };

    const int32_t aLeft   = (axisA(tl) + axisA(bl)) / 2;
    const int32_t aRight  = (axisA(tr) + axisA(br)) / 2;
    const int32_t bTop    = (axisB(tl) + axisB(tr)) / 2;
    const int32_t bBottom = (axisB(bl) + axisB(br)) / 2;

    // The targets sit `inset` pixels in from each edge, not at the corners -
    // you cannot reliably tap a corner, and a resistive panel is least linear
    // right at its edge. Extrapolate from the targets out to 0 and w-1/h-1.
    const int32_t spanX = w - 1 - 2 * inset;
    const int32_t spanY = h - 1 - 2 * inset;
    if (spanX > 0) {
        const int32_t perPx = ((aRight - aLeft) * 1000) / spanX;
        c.xRawMin = aLeft  - (perPx * inset) / 1000;
        c.xRawMax = aRight + (perPx * inset) / 1000;
    } else {
        c.xRawMin = aLeft;
        c.xRawMax = aRight;
    }
    if (spanY > 0) {
        const int32_t perPx = ((bBottom - bTop) * 1000) / spanY;
        c.yRawMin = bTop    - (perPx * inset) / 1000;
        c.yRawMax = bBottom + (perPx * inset) / 1000;
    } else {
        c.yRawMin = bTop;
        c.yRawMax = bBottom;
    }
    return c;
}

bool Touch::loadCal() {
    Preferences p;
    // Opened read-write even though this only reads. Opening read-only on a
    // namespace that does not exist yet - i.e. every first boot - makes the
    // NVS layer log an ERROR, and a red line during bring-up is exactly the
    // sort of thing that gets chased for an hour. Read-write creates the
    // namespace silently; nothing is written unless saveCal() is called.
    if (!p.begin(TOUCH_NVS_NAMESPACE, /*readOnly=*/false)) return false;

    TouchCal c{};
    const size_t n = p.getBytes(TOUCH_NVS_KEY, &c, sizeof(c));
    p.end();

    // A version mismatch is the normal outcome after a rotation change, not
    // an error - it just means the stored mapping describes a different
    // screen orientation and has to be redone.
    if (n != sizeof(c) || c.version != TOUCH_CAL_VERSION) return false;

    cal_ = c;
    return true;
}

void Touch::saveCal() const {
    Preferences p;
    if (!p.begin(TOUCH_NVS_NAMESPACE, /*readOnly=*/false)) return;
    p.putBytes(TOUCH_NVS_KEY, &cal_, sizeof(cal_));
    p.end();
}

void Touch::clearCal() {
    cal_ = TouchCal{};
    Preferences p;
    if (!p.begin(TOUCH_NVS_NAMESPACE, /*readOnly=*/false)) return;
    p.remove(TOUCH_NVS_KEY);
    p.end();
}
