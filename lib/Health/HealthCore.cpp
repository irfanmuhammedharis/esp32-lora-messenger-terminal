//
// HealthCore - the pure, Arduino-free vitals algorithm (PLAN.md Stage 4b).
//
// Pipeline, in order, each stage inside analyze():
//
//   1. staleness     - no samples recently? nothing to say.
//   2. DC removal    - mean over the window; AC is the residual.
//   3. finger gate   - IR DC must sit between floor and ceiling.
//   4. peak detection- adaptive threshold on the IR AC, minimum spacing.
//   5. HR            - 60 / mean peak spacing; validated 30..220 bpm.
//   6. SpO2          - ratio-of-ratios R, SpO2 = 110 - 25*R, clamped.
//   7. temperature   - MAX30205 value gated to 35.0..42.0 C.
//
// Every one of those decisions is deterministic, so the native tests can
// assert exact numbers on synthetic waveforms.

#include "Health.h"

#include <math.h>

void HealthCore::push(uint32_t nowMs, int32_t red, int32_t ir) {
    red_[next_] = red;
    ir_[next_]  = ir;
    next_ = (next_ + 1) % kWindow;
    if (fill_ < kWindow) fill_++;
    lastSampleMs_ = nowMs;
}

void HealthCore::reset() {
    fill_ = 0;
    next_ = 0;
    lastSampleMs_ = 0;
    tempMilliC_ = 0;
    hasTemp_ = false;
}

uint16_t HealthCore::copyIrWave(int32_t *dst, uint16_t n) const {
    if (!dst || !fill_) return 0;
    const uint16_t out = n < fill_ ? n : fill_;
    // Oldest is at `next_ % kWindow` (the first overwritten slot); walk
    // forward so the caller gets chronological order.
    uint16_t idx = (next_ + kWindow - fill_) % kWindow;
    for (uint16_t i = 0; i < out; i++) {
        dst[i] = ir_[idx];
        idx = (idx + 1) % kWindow;
    }
    return out;
}

HealthCore::Working HealthCore::copyWindow() const {
    Working w;
    w.n = fill_;
    const uint16_t start = (next_ + kWindow - fill_) % kWindow;
    for (uint16_t i = 0; i < fill_; i++) {
        const uint16_t idx = (start + i) % kWindow;
        w.red[i] = red_[idx];
        w.ir[i]  = ir_[idx];
    }
    return w;
}

HealthReading HealthCore::analyze(uint32_t nowMs) const {
    HealthReading r;

    // ── 1. Staleness ───────────────────────────────────────────────────────
    if (!fill_ || nowMs - lastSampleMs_ > HEALTH_STALE_MS) {
        r.tempValid = hasTemp_ && tempMilliC_ >= kTempMinMilliC &&
                      tempMilliC_ <= kTempMaxMilliC;
        if (r.tempValid) r.tempMilliC = tempMilliC_;
        return r;
    }

    const Working w = copyWindow();

    // ── 2/3. DC removal + finger gate ───────────────────────────────────────
    int64_t redSum = 0, irSum = 0;
    for (uint16_t i = 0; i < w.n; i++) {
        redSum += w.red[i];
        irSum  += w.ir[i];
    }
    const int32_t redDc = (int32_t)(redSum / w.n);
    const int32_t irDc  = (int32_t)(irSum / w.n);

    if (irDc < kIrDcFloor || irDc > kIrDcCeil) {
        // No finger, or a saturated channel. AC analysis is meaningless.
        r.tempValid = hasTemp_ && tempMilliC_ >= kTempMinMilliC &&
                      tempMilliC_ <= kTempMaxMilliC;
        if (r.tempValid) r.tempMilliC = tempMilliC_;
        return r;
    }

    // ── 4. Peak detection on the IR AC ──────────────────────────────────────
    int32_t hpIr[kWindow];
    int32_t maxV = 0;
    int64_t sqSumRed = 0, sqSumIr = 0;
    for (uint16_t i = 0; i < w.n; i++) {
        const int32_t h = w.ir[i] - irDc;
        hpIr[i] = h;
        if (h > maxV) maxV = h;
        const int32_t hRed = w.red[i] - redDc;
        sqSumRed += (int64_t)hRed * hRed;
        sqSumIr  += (int64_t)h * h;
    }
    // A waveform with no swing (max AC <= 0) has no peaks to find.
    if (maxV <= 0) {
        r.tempValid = hasTemp_ && tempMilliC_ >= kTempMinMilliC &&
                      tempMilliC_ <= kTempMaxMilliC;
        if (r.tempValid) r.tempMilliC = tempMilliC_;
        return r;
    }

    const int32_t thr = maxV / 2;   // adaptive: half the window's own swing
    const uint16_t spacingSamples =
        (uint16_t)((uint32_t)kMinPeakSpacingMs * HEALTH_SAMPLE_HZ / 1000);

    uint16_t peakCount = 0;
    int32_t  firstPeak = -1, lastPeak = -1;
    int32_t  lastPeakIdx = -(int32_t)spacingSamples;
    for (uint16_t i = 1; i + 1 < w.n; i++) {
        if (hpIr[i] < thr) continue;
        if (hpIr[i] < hpIr[i - 1] || hpIr[i] < hpIr[i + 1]) continue;
        if ((int32_t)i - lastPeakIdx < (int32_t)spacingSamples) continue;
        if (peakCount == 0) firstPeak = i;
        lastPeak = i;
        lastPeakIdx = i;
        peakCount++;
    }

    // ── 5. Heart rate ───────────────────────────────────────────────────────
    if (peakCount >= 2) {
        const int32_t spanSamples = lastPeak - firstPeak;
        if (spanSamples > 0) {
            const uint32_t spanMs =
                (uint32_t)spanSamples * 1000 / HEALTH_SAMPLE_HZ;
            const uint32_t meanSpacingMs = spanMs / (peakCount - 1);
            if (meanSpacingMs > 0) {
                const uint32_t hr = 60000 / meanSpacingMs;
                if (hr >= 30 && hr <= 220) {
                    r.hr = (uint16_t)hr;
                    r.hrValid = true;
                }
            }
        }
        // Coarse quality: how many beats the 3 s window actually held, out
        // of the ~6 a healthy 120 bpm would. Purely cosmetic.
        uint32_t q = peakCount * 100 / 6;
        r.quality = (uint8_t)(q > 100 ? 100 : q);
    }

    // ── 6. SpO2, ratio-of-ratios ────────────────────────────────────────────
    if (irDc > 0 && redDc > 0 && w.n > 0) {
        const double acRed = sqSumRed > 0
            ? (double)sqrt((double)sqSumRed / w.n) : 0.0;
        const double acIr  = sqSumIr > 0
            ? (double)sqrt((double)sqSumIr / w.n) : 0.0;
        if (acIr > 0) {
            const double ratio = (acRed / redDc) / (acIr / irDc);
            // Maxim's default calibration curve. An empirical device-specific
            // re-calibration is a Stage 7+ item; this is the documented
            // standard slope/intercept pair.
            const double spo2d = 110.0 - 25.0 * ratio;
            if (spo2d >= 70.0 && spo2d <= 100.0) {
                r.spo2 = (uint8_t)(spo2d + 0.5);
                r.spo2Valid = true;
            }
        }
    }

    // ── 7. Temperature gate ─────────────────────────────────────────────────
    r.tempValid = hasTemp_ && tempMilliC_ >= kTempMinMilliC &&
                  tempMilliC_ <= kTempMaxMilliC;
    if (r.tempValid) r.tempMilliC = tempMilliC_;

    return r;
}
