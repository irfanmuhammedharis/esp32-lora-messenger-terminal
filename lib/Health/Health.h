#pragma once
//
// Vitals monitoring: heart rate and SpO2 from a MAX30102, body temperature
// from a MAX30205 (PLAN.md section 2.3).
//
// The library is split along the same line as LoraLink:
//
//   * HealthCore below contains NO Arduino calls and NO I2C. It owns the
//     signal processing - DC removal, adaptive peak detection for HR, the
//     SpO2 ratio-of-ratios, and the validity gates - so `pio test -e native`
//     feeds it captured or synthetic PPG waveforms and grades the answers
//     (Stage 4b, PLAN.md section 5).
//
//   * The device layer at the bottom (Max30102, Max30205, HealthSensor)
//     owns Wire and is guarded by #ifdef ARDUINO, so the native build never
//     sees it.
//
// The boundary matters more here than anywhere else in the project: a
// plausible-looking wrong SpO2 number is worse than none. Every value this
// core emits has passed a gate, or its `valid` flag is false.
//
// ── Why there is no temperature algorithm ───────────────────────────────────
// The MAX30102 has no temperature sensor. Its die-temperature register exists
// only to compensate the LED wavelengths internally; body temperature comes
// from the MAX30205 and is passed into the core verbatim. The core only
// gates it (35.0-42.0 C), because that decision belongs with the other
// validity decisions and therefore belongs in the testable part.

#include <stdint.h>

#include "app_config.h"

// One FIFO sample: red and IR, both 18-bit values sign-extended from the
// part's 3-byte fields.
struct VitalPair {
    int32_t red;
    int32_t ir;
};

struct HealthReading {
    bool     hrValid   = false;
    uint16_t hr        = 0;     // beats per minute
    bool     spo2Valid = false;
    uint8_t  spo2      = 0;     // percent
    bool     tempValid = false;
    // 36500 == 36.5 C. int32, not int16: body temperature in milli-degrees
    // is above int16's 32767, and an int16 here wrapped 36.5 C to -29.0 C.
    int32_t  tempMilliC = 0;
    uint8_t  quality   = 0;     // 0..100, coarse; from peak count vs window
};

class HealthCore {
public:
    // Window: 3 s of samples at HEALTH_SAMPLE_HZ. Sized for the slowest
    // plausibly-real heart rate (30 bpm is a beat every 2 s, so 3 s is
    // guaranteed to hold two peaks) and small enough to keep in a fixed
    // ring buffer - static allocation, no heap (PLAN.md interrupt rules
    // do not apply here, but the memory rules do).
    static constexpr uint16_t kWindow = (HEALTH_SAMPLE_HZ * 3);

    // Peak spacing floor. 220 bpm is a beat every 272 ms; 250 ms admits
    // that while still rejecting double-taps from one systolic bump.
    static constexpr uint16_t kMinPeakSpacingMs = 250;

    // Finger-presence gate on the IR DC component (18-bit ADC path).
    // Below the floor there is no finger; at the ceiling the channel is
    // saturated and the AC component is meaningless either way.
    static constexpr int32_t kIrDcFloor = 10000;
    static constexpr int32_t kIrDcCeil  = 262000;

    static constexpr int32_t kTempMinMilliC = 35000;
    static constexpr int32_t kTempMaxMilliC = 42000;

    // Feed one sample pair. nowMs is the sample's arrival time.
    void push(uint32_t nowMs, int32_t red, int32_t ir);

    // Run the analysis over the buffered window. Deterministic and cheap
    // (one linear pass over kWindow); call it at ~1 Hz from the app.
    HealthReading analyze(uint32_t nowMs) const;

    // Body temperature from the MAX30205, passed in verbatim.
    void setTempMilliC(int32_t t) { tempMilliC_ = t; hasTemp_ = true; }

    void reset();
    uint16_t fill() const { return fill_; }

    // Copy up to `n` most recent IR samples, oldest first, for the UI
    // sparkline. Returns how many were copied.
    uint16_t copyIrWave(int32_t *dst, uint16_t n) const;

    uint32_t lastSampleMs() const { return lastSampleMs_; }

private:
    // Analyze on a materialised copy so the method can stay const while the
    // ring buffer stays lock-free for the caller.
    struct Working {
        int32_t red[kWindow];
        int32_t ir[kWindow];
        uint16_t n;
    };
    Working copyWindow() const;

    int32_t red_[kWindow]   = {0};
    int32_t ir_[kWindow]    = {0};
    uint16_t fill_          = 0;
    uint16_t next_          = 0;
    uint32_t lastSampleMs_  = 0;

    int32_t tempMilliC_ = 0;
    bool    hasTemp_    = false;
};

#ifdef ARDUINO

#include <Arduino.h>
#include <Wire.h>

//
// MAX30102 register map (datasheet, table 1). Only the registers this driver
// touches are declared; a register that does not appear here is not written.
//
namespace Max30102Regs {
    constexpr uint8_t kIntStatus1    = 0x00;
    constexpr uint8_t kFifoWrPtr     = 0x04;
    constexpr uint8_t kOverflowCtr   = 0x05;
    constexpr uint8_t kFifoRdPtr     = 0x06;
    constexpr uint8_t kFifoData      = 0x07;
    constexpr uint8_t kFifoConfig    = 0x08;
    constexpr uint8_t kModeConfig    = 0x09;
    constexpr uint8_t kSpo2Config    = 0x0A;
    constexpr uint8_t kLedPulseAmp1  = 0x0C;   // red
    constexpr uint8_t kLedPulseAmp2  = 0x0D;   // IR
    constexpr uint8_t kRevId         = 0xFF;

    constexpr uint8_t kRevIdExpected = 0x15;

    // Mode: SpO2 (red + IR), both LEDs cycling. Reset/SHDN cleared.
    constexpr uint8_t kModeSpo2 = 0x03;

    // SpO2 config 0x27: ADC range 4096 nA (bits 6:5 = 01), sample rate
    // 100 Hz (bits 4:2 = 001), pulse width 411 us / 18-bit (bits 1:0 = 11).
    // The 18-bit resolution is what the FIFO returns regardless, and the
    // 411 us pulse gives the best signal for a fingertip.
    constexpr uint8_t kSpo2Cfg100Hz18bit = 0x27;

    // FIFO: rollover enabled (bit 4), almost-full at 8 unread samples.
    // With no averaging, a sample is 3 bytes red + 3 bytes IR.
    constexpr uint8_t kFifoRolloverA8 = 0x18;

    constexpr uint8_t kFifoSampleBytes = 6;
    constexpr uint8_t kFifoDepth        = 32;
    constexpr uint8_t kFifoMask         = 0x1F;
}

//
// MAX30205 register map. Two registers and done: the temperature register and
// the configuration register (write 0x00 = continuous conversion, thermostat
// comparator mode off).
//
namespace Max30205Regs {
    constexpr uint8_t kTemperature = 0x00;
    constexpr uint8_t kConfig      = 0x01;
    constexpr uint8_t kConfigCont  = 0x00;

    // 16-bit two's complement, 0.00390625 C per LSB.
    constexpr double kLsbCelsius = 0.00390625;
}

// Thin I2C transport for one MAX30102. Everything here is register I/O; all
// the signal processing lives in HealthCore.
class Max30102 {
public:
    bool begin(TwoWire &wire);
    bool present() const { return ok_; }

    // Drain up to `maxSamples` from the FIFO, oldest first. Returns how many
    // were read; -1 means the part stopped answering (I2C failure).
    int16_t readSamples(VitalPair *out, uint8_t maxSamples);
    uint8_t overflowCounter();

private:
    bool readReg(uint8_t reg, uint8_t *val);
    bool writeReg(uint8_t reg, uint8_t val);

    TwoWire *wire_ = nullptr;
    bool     ok_   = false;
};

// Thin I2C transport for one MAX30205. Temperature only.
class Max30205 {
public:
    bool begin(TwoWire &wire);
    bool present() const { return ok_; }
    bool readMilliC(int32_t *out);

private:
    TwoWire *wire_ = nullptr;
    bool     ok_   = false;
};

// The two sensors plus the core, as one pollable unit.
class HealthSensor {
public:
    // Wire up the bus on GPIO21/22 and bring both parts up. Returns false
    // if neither part answers - the app treats health as optional and keeps
    // running (radio is the core function; a missing sensor must not brick
    // the terminal). Individual presence is available from the getters.
    bool begin(TwoWire &wire, int sda, int scl);
    bool max30102Present() const { return ppg_.present(); }
    bool max30205Present() const { return temp_.present(); }

    // Drain the FIFO and fold new samples into the core. Call every loop();
    // it self-throttles to HEALTH_FIFO_DRAIN_MS.
    void poll(uint32_t nowMs);

    HealthReading latest(uint32_t nowMs) {
        HealthReading r = core_.analyze(nowMs);
        if (max30205Present() && nowMs - lastTempMs_ >= HEALTH_TEMP_INTERVAL_MS) {
            int32_t t;
            if (temp_.readMilliC(&t)) {
                lastTempMs_ = nowMs;
                core_.setTempMilliC(t);
            }
        }
        return core_.analyze(nowMs);
    }

    const HealthCore &core() const { return core_; }

    // Diagnostics for Stage 4a: samples drained, drains, FIFO overflows.
    uint32_t samplesDrained() const { return samples_; }
    uint32_t drains() const        { return drains_; }
    uint8_t  fifoOverflows() const { return overflows_; }

private:
    HealthCore core_;
    Max30102   ppg_;
    Max30205   temp_;

    uint32_t lastDrainMs_ = 0;
    uint32_t lastTempMs_  = 0;
    uint32_t samples_ = 0;
    uint32_t drains_  = 0;
    uint8_t  overflows_ = 0;
};

#endif // ARDUINO
