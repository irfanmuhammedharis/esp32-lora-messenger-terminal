//
// Device layer: MAX30102 + MAX30205 over I2C on GPIO21/22 (PLAN.md 2.3).
//
// Everything here is register I/O and timing. No signal processing lives in
// this file - that is HealthCore's job - which is what keeps the algorithm
// testable on a PC.
//
// This file compiles to nothing on the host: the native test environment
// defines no ARDUINO, so `pio test -e native` never sees Wire.h, and any
// future Arduino dependency leaking into the core fails the host build
// instead of silently passing.

#ifdef ARDUINO

#include "Health.h"

// ── MAX30102 ────────────────────────────────────────────────────────────────

bool Max30102::readReg(uint8_t reg, uint8_t *val) {
    wire_->beginTransmission(0x57);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom(0x57, (uint8_t)1) != 1) return false;
    *val = wire_->read();
    return true;
}

bool Max30102::writeReg(uint8_t reg, uint8_t val) {
    wire_->beginTransmission(0x57);
    wire_->write(reg);
    wire_->write(val);
    return wire_->endTransmission(true) == 0;
}

bool Max30102::begin(TwoWire &wire) {
    wire_ = &wire;

    // Identify before configuring, exactly the Stage 1a discipline: a dead
    // sensor and a dead bus both present as NACK, but a part that answers
    // with the wrong ID tells us it is a different chip (PLAN.md risk R8).
    uint8_t id = 0;
    if (!readReg(Max30102Regs::kRevId, &id)) return false;
    if (id != Max30102Regs::kRevIdExpected) return false;

    // Reset, then poll until the part clears the bit itself (~1 ms typ).
    if (!writeReg(Max30102Regs::kModeConfig, 0x40)) return false;
    uint32_t deadline = millis() + 50;
    do {
        uint8_t m;
        if (!readReg(Max30102Regs::kModeConfig, &m)) return false;
        if (!(m & 0x40)) break;
        delay(1);
    } while ((int32_t)(millis() - deadline) < 0);

    // FIFO: rollover, almost-full at 8. Read pointer first so the part's
    // power-on FIFO contents (uninitialised) are marked consumed.
    if (!writeReg(Max30102Regs::kFifoConfig, Max30102Regs::kFifoRolloverA8))
        return false;
    if (!writeReg(Max30102Regs::kFifoRdPtr, 0)) return false;
    if (!writeReg(Max30102Regs::kFifoWrPtr, 0)) return false;

    // LED current, in 0.2 mA steps: HEALTH_LED_CURRENT_TENTH_MA tenths of a
    // milliamp / 2 = register value. 64 tenths -> 32 -> 6.4 mA (Stage 4a).
    const uint8_t led =
        (uint8_t)((HEALTH_LED_CURRENT_TENTH_MA + 1) / 2);   // round up
    if (!writeReg(Max30102Regs::kLedPulseAmp1, led)) return false;
    if (!writeReg(Max30102Regs::kLedPulseAmp2, led)) return false;

    // SpO2 configuration (100 Hz, 18-bit), then start conversion.
    if (!writeReg(Max30102Regs::kSpo2Config,
                  Max30102Regs::kSpo2Cfg100Hz18bit)) return false;
    if (!writeReg(Max30102Regs::kModeConfig, Max30102Regs::kModeSpo2))
        return false;

    ok_ = true;
    return true;
}

int16_t Max30102::readSamples(VitalPair *out, uint8_t maxSamples) {
    if (!ok_ || !out || !maxSamples) return 0;

    uint8_t rd = 0, wr = 0;
    if (!readReg(Max30102Regs::kFifoRdPtr, &rd)) return -1;
    if (!readReg(Max30102Regs::kFifoWrPtr, &wr)) return -1;

    int16_t avail = (wr - rd) & Max30102Regs::kFifoMask;
    if (avail <= 0) return 0;
    if (avail > maxSamples) avail = maxSamples;

    // One burst read of avail*6 bytes. The FIFO is a shift register: the
    // part pops exactly the bytes read, and the burst keeps the sample
    // boundaries in lockstep even at 400 kHz.
    const uint8_t bytes = (uint8_t)(avail * Max30102Regs::kFifoSampleBytes);
    wire_->beginTransmission(0x57);
    wire_->write(Max30102Regs::kFifoData);
    if (wire_->endTransmission(false) != 0) return -1;
    if (wire_->requestFrom(0x57, bytes) != bytes) return -1;

    for (int16_t i = 0; i < avail; i++) {
        const uint32_t rh = wire_->read();
        const uint32_t rm = wire_->read();
        const uint32_t rl = wire_->read();
        const uint32_t ih = wire_->read();
        const uint32_t im = wire_->read();
        const uint32_t il = wire_->read();

        // 18-bit, MSB first. The parts emit 18-bit values with the top two
        // bits masked by the ADC range; sign-extending is harmless because
        // these are magnitudes.
        out[i].red = (int32_t)((rh << 16) | (rm << 8) | rl) & 0x3FFFF;
        out[i].ir  = (int32_t)((ih << 16) | (im << 8) | il) & 0x3FFFF;
    }
    return avail;
}

uint8_t Max30102::overflowCounter() {
    uint8_t v = 0;
    readReg(Max30102Regs::kOverflowCtr, &v);
    return v;
}

// ── MAX30205 ────────────────────────────────────────────────────────────────

bool Max30205::begin(TwoWire &wire) {
    wire_ = &wire;

    // Continuous conversion, comparator mode off. A NACK here on a scanned
    // address means the part is not what we think it is.
    wire_->beginTransmission(0x48);
    wire_->write(Max30205Regs::kConfig);
    wire_->write(Max30205Regs::kConfigCont);
    if (wire_->endTransmission(true) != 0) return false;

    int32_t t;
    if (!readMilliC(&t)) return false;

    ok_ = true;
    return true;
}

bool Max30205::readMilliC(int32_t *out) {
    wire_->beginTransmission(0x48);
    wire_->write(Max30205Regs::kTemperature);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom(0x48, (uint8_t)2) != 2) return false;

    const uint8_t hi = wire_->read();
    const uint8_t lo = wire_->read();
    const int16_t raw = (int16_t)((uint16_t)(hi << 8) | lo);
    const double celsius = raw * Max30205Regs::kLsbCelsius;
    *out = (int32_t)(celsius * 1000.0);   // milli-degrees
    return true;
}

// ── HealthSensor ────────────────────────────────────────────────────────────

bool HealthSensor::begin(TwoWire &wire, int sda, int scl) {
    // GPIO21/22 - Wire's own defaults, usable now that LCD_D2/D3 moved to
    // GPIO16/17 - the bus is still constructed explicitly (PLAN.md 2.3).
    wire.begin(sda, scl);
    wire.setClock(400000);   // both parts are 400 kHz parts; the scan in
                             // Stage 4a proved the pull-ups can take it

    ppg_.begin(wire);
    temp_.begin(wire);
    return max30102Present() || max30205Present();
}

void HealthSensor::poll(uint32_t nowMs) {
    if (nowMs - lastDrainMs_ < HEALTH_FIFO_DRAIN_MS) return;
    lastDrainMs_ = nowMs;

    if (!max30102Present()) return;

    VitalPair batch[5];
    const int16_t n = ppg_.readSamples(batch, 5);
    if (n < 0) return;   // I2C fault; next poll tries again

    if (max30102Present()) overflows_ = ppg_.overflowCounter();

    // Fold the batch in with per-sample timestamps: 10 ms apart at 100 Hz,
    // walking backwards from now, so staleness logic still sees fresh data.
    for (int16_t i = 0; i < n; i++) {
        const uint32_t t =
            nowMs - (uint32_t)(n - 1 - i) * (1000 / HEALTH_SAMPLE_HZ);
        core_.push(t, batch[i].red, batch[i].ir);
    }
    samples_ += (uint32_t)n;
    drains_++;
}

#endif // ARDUINO
