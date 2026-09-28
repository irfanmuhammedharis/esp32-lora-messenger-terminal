// Stage 4b - HR/SpO2 algorithm, on the host.   pio test -e native
//
// Exit criteria from PLAN.md section 5:
//   - peak-detected HR within +/-3 bpm of truth on captured waveforms
//     (clean and noisy fixtures)
//   - SpO2 ratio-of-ratios computed
//   - finger-off waveforms classify as no-valid-signal
//
// The fixtures are synthetic but built to the physics of the real part:
// 18-bit IR/red magnitudes, a DC component orders of magnitude above the AC,
// and a pulse whose shape is a rounded bump, not a sine.

#include <math.h>
#include <string.h>
#include <unity.h>

#include "Health.h"

// ── Fixture: a photoplethysmogram at exactly `bpm` ──────────────────────────
// DC levels are typical for a fingertip at 6.4 mA LED current. A Gaussian
// bump per beat is a closer stand-in for a real systolic pulse than a sine,
// and it is what makes the peak detector work for its living.
static void synthPpg(int32_t *ir, int32_t *red, uint16_t n, double bpm,
                     int32_t irDc, int32_t irAc, int32_t redDc,
                     int32_t redAc, double noiseAmp) {
    const double periodSamples = HEALTH_SAMPLE_HZ * 60.0 / bpm;
    const double sigma = periodSamples * 0.08;   // pulse width ~ 8% of period

    uint32_t s = 0x12345678u;   // fixed LCG - the fixture must be deterministic
    for (uint16_t i = 0; i < n; i++) {
        // Distance to the nearest beat centre, in samples.
        const double nearest = fmod((double)i + periodSamples / 2.0,
                                    periodSamples) - periodSamples / 2.0;
        const double pulse = exp(-(nearest * nearest) / (2.0 * sigma * sigma));
        // LCG noise, uniform-ish in [-1, 1).
        s = s * 1664525u + 1013904223u;
        const double noise = ((int32_t)(s >> 8) / 8388608.0) - 1.0;

        ir[i]  = irDc  + (int32_t)(irAc  * pulse + noiseAmp * irAc  * noise);
        red[i] = redDc + (int32_t)(redAc * pulse + noiseAmp * redAc * noise);
    }
}

// Push a fixture through the byte-agnostic interface at the real 100 Hz.
static void feed(HealthCore &c, const int32_t *ir, const int32_t *red,
                 uint16_t n, uint32_t t0Ms) {
    for (uint16_t i = 0; i < n; i++) {
        c.push(t0Ms + (uint32_t)i * 10, red[i], ir[i]);
    }
}

// ── Heart rate ──────────────────────────────────────────────────────────────

static void test_hr_60bpm_clean(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 60.0,
             60000, 4000, 40000, 2500, 0.02);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_TRUE(r.hrValid);
    TEST_ASSERT_INT_WITHIN(3, 60, r.hr);
}

static void test_hr_120bpm_clean(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 120.0,
             60000, 4000, 40000, 2500, 0.02);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_TRUE(r.hrValid);
    TEST_ASSERT_INT_WITHIN(3, 120, r.hr);
}

static void test_hr_180bpm_clean(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 180.0,
             60000, 4000, 40000, 2500, 0.02);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_TRUE(r.hrValid);
    TEST_ASSERT_INT_WITHIN(3, 180, r.hr);
}

// A noisy waveform is the real-world case: a resistive-touch-class sensor
// reading through skin has motion artifacts. 10% noise must not break the
// answer (PLAN.md: graded on noisy fixtures, not just clean ones).
static void test_hr_60bpm_noisy(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 60.0,
             60000, 4000, 40000, 2500, 0.10);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_TRUE(r.hrValid);
    TEST_ASSERT_INT_WITHIN(3, 60, r.hr);
}

// ── SpO2 ────────────────────────────────────────────────────────────────────

// With AC/DC chosen so the ratio-of-ratios R is exactly 1.0, the default
// calibration curve must read 110 - 25*1 = 85 %.
static void test_spo2_known_ratio(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    // AC/DC equal on both channels: (4000/60000) / (2500/40000) = 0.067/0.0625
    // -> tune instead: use redAc so (redAc/redDc) == (irAc/irDc).
    // irAc/irDc = 4000/60000 = 1/15. redAc = redDc/15 = 2667.
    synthPpg(ir, red, HealthCore::kWindow, 60.0,
             60000, 4000, 40000, 2667, 0.01);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_TRUE(r.spo2Valid);
    TEST_ASSERT_INT_WITHIN(2, 85, r.spo2);
}

// Higher perfusion ratio -> lower R -> higher SpO2. Red AC halved relative to
// IR: R = 0.5, SpO2 = 110 - 12.5 = 97.5 -> 98.
static void test_spo2_higher_oxygenation(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 60.0,
             60000, 4000, 40000, 1333, 0.01);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_TRUE(r.spo2Valid);
    TEST_ASSERT_INT_WITHIN(2, 98, r.spo2);
}

// ── Finger-off and fault classification (PLAN.md R9) ────────────────────────

static void test_finger_off_is_invalid(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    // Ambient only: tiny IR DC well under the presence floor.
    for (uint16_t i = 0; i < HealthCore::kWindow; i++) {
        ir[i] = 800;
        red[i] = 600;
    }

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_FALSE(r.hrValid);
    TEST_ASSERT_FALSE(r.spo2Valid);
}

static void test_saturated_channel_is_invalid(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    for (uint16_t i = 0; i < HealthCore::kWindow; i++) {
        ir[i] = 262143;   // pegged at full scale
        red[i] = 262143;
    }

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    const HealthReading r = c.analyze(HealthCore::kWindow * 10);

    TEST_ASSERT_FALSE(r.hrValid);
    TEST_ASSERT_FALSE(r.spo2Valid);
}

static void test_stale_data_is_invalid(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 60.0,
             60000, 4000, 40000, 2500, 0.02);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);
    // Analyse 10 s after the last sample: past HEALTH_STALE_MS.
    const HealthReading r = c.analyze(HealthCore::kWindow * 10 + 10000);

    TEST_ASSERT_FALSE(r.hrValid);
    TEST_ASSERT_FALSE(r.spo2Valid);
}

static void test_partial_window_is_invalid(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 60.0,
             60000, 4000, 40000, 2500, 0.02);

    HealthCore c;
    // Half a second of data: too few peaks for a rate.
    feed(c, ir, red, 50, 0);
    const HealthReading r = c.analyze(500);

    TEST_ASSERT_FALSE(r.hrValid);
}

// ── Temperature gate (MAX30205 value carried verbatim) ──────────────────────

static void test_temperature_gate(void) {
    static int32_t ir[HealthCore::kWindow], red[HealthCore::kWindow];
    synthPpg(ir, red, HealthCore::kWindow, 60.0,
             60000, 4000, 40000, 2500, 0.02);

    HealthCore c;
    feed(c, ir, red, HealthCore::kWindow, 0);

    c.setTempMilliC(30000);   // 30 C: a detached probe, not a body
    HealthReading r = c.analyze(HealthCore::kWindow * 10);
    TEST_ASSERT_FALSE(r.tempValid);

    c.setTempMilliC(36500);   // 36.5 C: plausible body temperature
    r = c.analyze(HealthCore::kWindow * 10);
    TEST_ASSERT_TRUE(r.tempValid);
    TEST_ASSERT_EQUAL_INT32(36500, r.tempMilliC);

    // The top of the gate. 42 C is above int16's range in milli-degrees, so
    // this is also the regression check for the type: an int16 field wrapped
    // it negative, and the old EQUAL_INT16 assert above wrapped the expected
    // value the same way and passed anyway.
    c.setTempMilliC(42000);
    r = c.analyze(HealthCore::kWindow * 10);
    TEST_ASSERT_TRUE(r.tempValid);
    TEST_ASSERT_EQUAL_INT32(42000, r.tempMilliC);

    c.setTempMilliC(42100);   // 42.1 C: past what a living body reads
    r = c.analyze(HealthCore::kWindow * 10);
    TEST_ASSERT_FALSE(r.tempValid);
}

// ── Wave access for the UI sparkline ────────────────────────────────────────

static void test_copy_ir_wave_order_and_clamp(void) {
    HealthCore c;
    for (int32_t i = 0; i < 120; i++) c.push((uint32_t)i * 10, i, 1000 + i);

    int32_t dst[100] = {0};
    // Ask for more than is buffered: the count must clamp, order is oldest
    // first and contiguous. 120 pushes < 300 window, so nothing has wrapped
    // and the buffer holds samples 0..119 verbatim.
    const uint16_t got = c.copyIrWave(dst, 100);
    TEST_ASSERT_EQUAL_UINT16(100, got);
    for (uint16_t i = 0; i < got; i++) {
        TEST_ASSERT_EQUAL_INT32(1000 + (int32_t)i, dst[i]);
    }
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_hr_60bpm_clean);
    RUN_TEST(test_hr_120bpm_clean);
    RUN_TEST(test_hr_180bpm_clean);
    RUN_TEST(test_hr_60bpm_noisy);
    RUN_TEST(test_spo2_known_ratio);
    RUN_TEST(test_spo2_higher_oxygenation);
    RUN_TEST(test_finger_off_is_invalid);
    RUN_TEST(test_saturated_channel_is_invalid);
    RUN_TEST(test_stale_data_is_invalid);
    RUN_TEST(test_partial_window_is_invalid);
    RUN_TEST(test_temperature_gate);
    RUN_TEST(test_copy_ir_wave_order_and_clamp);

    return UNITY_END();
}
