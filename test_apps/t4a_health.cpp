// ─────────────────────────────────────────────────────────────────────────────
// Stage 4a — health bring-up: MAX30102 + MAX30205 on the I2C bus (GPIO21/22).
//             pio run -e t4a_health -t upload
//
// Exit criteria (PLAN.md section 5):
//   [1] I2C scan at 100/400 kHz finds 0x57 and 0x48
//   [2] MAX30102 REV_ID reads 0x15
//   [3] FIFO streams red+IR at 100 Hz with no corruption
//   [4] MAX30205 reads 35-42 C, stable within +/-0.1 C over a minute
//   [5] (interactive) with a finger on the glass, HR/SpO2 become valid
//
// The first four are graded automatically over a 20 s window. Criterion 5 is
// graded live: it cannot pass without a finger, and that is by design — the
// whole point of the gate is that it refuses to invent a number.
//
// Wiring: WIRING.md section 5. SDA->GPIO21, SCL->GPIO22, 3V3 from the ESP32's
// own rail, common ground. No INT line - the first cut polls the FIFO.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>

#include "Health.h"
#include "app_config.h"
#include "pins.h"

static TFT_eSPI      tft;
static HealthSensor  health;

// ── I2C scan ────────────────────────────────────────────────────────────────

static uint8_t scanBus(TwoWire &wire, char *out, size_t outLen) {
    uint8_t found = 0;
    out[0] = '\0';
    for (uint8_t addr = 1; addr < 127; addr++) {
        wire.beginTransmission(addr);
        if (wire.endTransmission() == 0) {
            found++;
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X ", addr);
            if (strlen(out) + strlen(buf) < outLen) strcat(out, buf);
        }
    }
    return found;
}

// ── Display ─────────────────────────────────────────────────────────────────

static void drawLine(int line, const char *s, uint32_t colour) {
    tft.setTextColor(colour, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(s, 4, 4 + line * 26, 2);
}

static constexpr uint32_t kGreen = 0x2f6f2f;
static constexpr uint32_t kRed   = 0x8f2f2f;
static constexpr uint32_t kGrey  = 0x7b8fa3;

// ── Grading ─────────────────────────────────────────────────────────────────

static bool criterion1 = false;   // scan finds both
static bool criterion2 = false;   // REV_ID == 0x15
static bool criterion3 = false;   // sample rate within 10% of 100 Hz
static bool criterion4 = false;   // temperature in range
static bool criterion5 = false;   // HR/SpO2 valid (finger)

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 4a - health bring-up: MAX30102 + MAX30205");
    Serial.println("=====================================================");
    Serial.printf("  bus ............... Wire on GPIO%d (SDA) / GPIO%d (SCL)\n",
                  PIN_I2C_SDA, PIN_I2C_SCL);

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);
    drawLine(0, "STAGE 4a - HEALTH", TFT_CYAN);

    // ── [1] Scan at 100 kHz first: a marginal bus usually still answers at
    // 100 kHz, and failing here but passing nowhere is a pull-up hint.
    char found[160];
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);
    const uint8_t n100 = scanBus(Wire, found, sizeof(found));
    Serial.printf("  scan @ 100 kHz .... %u device(s): %s\n", n100, found);

    Wire.setClock(400000);
    const uint8_t n400 = scanBus(Wire, found, sizeof(found));
    Serial.printf("  scan @ 400 kHz .... %u device(s): %s\n", n400, found);

    criterion1 = (n100 >= 2 || n400 >= 2);

    // ── [2] Part IDs, inside begin(). A part that answers with the wrong
    // ID is a different chip, and the app must say so, not guess (risk R8).
    const bool any = health.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL);
    Serial.printf("  MAX30102 .......... %s\n",
                  health.max30102Present() ? "present, REV_ID = 0x15"
                                           : "NOT FOUND");
    Serial.printf("  MAX30205 .......... %s\n",
                  health.max30205Present() ? "present" : "NOT FOUND");
    criterion2 = health.max30102Present();
    criterion1 = criterion1 && health.max30102Present() &&
                 health.max30205Present();
    if (!any) {
        Serial.println("  No sensor answers. Check wiring and pull-ups");
        Serial.println("  (WIRING.md section 5), then reset.");
    }

    Serial.println("-----------------------------------------------------");
    Serial.println("  streaming... keep still; criterion 5 needs a finger");
    Serial.println("-----------------------------------------------------");
}

void loop() {
    const uint32_t now = millis();
    static uint32_t t0 = now;
    static uint32_t lastReport = 0;
    static uint32_t lastSamples = 0;
    static bool     gradedOnce = false;
    static int32_t  tempMin = 0, tempMax = 0;
    static bool     haveTemp = false;

    health.poll(now);

    if (now - lastReport >= 1000) {
        const uint32_t dt = now - lastReport;
        lastReport = now;

        const HealthReading r = health.latest(now);

        // ── [3] instantaneous rate, for the serial trace only; the verdict
        // below grades the whole 20 s window.
        const uint32_t s = health.samplesDrained();
        const uint32_t rate = (s - lastSamples) * 1000 / dt;
        lastSamples = s;

        // ── [4] temperature tracking over the window: min/max spread is the
        // stability measure, evaluated once at the end.
        if (r.tempValid) {
            if (!haveTemp) { tempMin = tempMax = r.tempMilliC; haveTemp = true; }
            if (r.tempMilliC < tempMin) tempMin = r.tempMilliC;
            if (r.tempMilliC > tempMax) tempMax = r.tempMilliC;
        }

        // ── [5] HR/SpO2 validity needs a finger; latch it once seen.
        if (r.hrValid && r.spo2Valid) criterion5 = true;

        Serial.printf("  t=%3lus rate=%3lu/s samples=%lu oflw=%u | "
                      "HR %s%u bpm | SpO2 %s%u%% | T %s%.1f C\n",
                      (unsigned long)((now - t0) / 1000), (unsigned long)rate,
                      (unsigned long)s, health.fifoOverflows(),
                      r.hrValid ? "" : "-", r.hr,
                      r.spo2Valid ? "" : "-", r.spo2,
                      r.tempValid ? "" : "-", r.tempMilliC / 1000.0);

        // ── Grade criteria 3 and 4 exactly once, at the 20 s mark, from
        // cumulative statistics - a burst of correct samples followed by
        // silence must not pass.
        if (!gradedOnce && now - t0 >= 20000) {
            gradedOnce = true;
            const uint32_t overall =
                health.samplesDrained() * 1000 / (now - t0);
            criterion3 = (overall >= 90 && overall <= 110);
            criterion4 = haveTemp && (tempMax - tempMin) <= 100 &&
                         tempMin >= 35000 && tempMax <= 42000;
            Serial.println("-----------------------------------------------------");
            Serial.printf("  VERDICT %u/5  [1]%s [2]%s [3]%s [4]%s [5]%s\n",
                          (unsigned)(criterion1 + criterion2 + criterion3 +
                                     criterion4 + criterion5),
                          criterion1 ? "P" : "F", criterion2 ? "P" : "F",
                          criterion3 ? "P" : "F", criterion4 ? "P" : "F",
                          criterion5 ? "P" : "needs finger");
        }

        // TFT status block
        char buf[48];
        const bool waiting = (now - t0 < 20000);
        drawLine(1, criterion1 ? "[1] scan 57+48      PASS" : "[1] scan 57+48      FAIL",
                 criterion1 ? kGreen : kRed);
        drawLine(2, criterion2 ? "[2] REV_ID 0x15      PASS" : "[2] REV_ID 0x15      FAIL",
                 criterion2 ? kGreen : kRed);
        snprintf(buf, sizeof(buf), "[3] 100 Hz stream   %s",
                 gradedOnce ? (criterion3 ? "PASS" : "FAIL") : "wait");
        drawLine(3, buf, !gradedOnce ? kGrey : (criterion3 ? kGreen : kRed));
        snprintf(buf, sizeof(buf), "[4] temp 35-42 C    %s",
                 gradedOnce ? (criterion4 ? "PASS" : "FAIL") : "wait");
        drawLine(4, buf, !gradedOnce ? kGrey : (criterion4 ? kGreen : kRed));
        snprintf(buf, sizeof(buf), "[5] HR/SpO2 valid   %s",
                 criterion5 ? "PASS" : "needs finger");
        drawLine(5, buf, criterion5 ? kGreen : kGrey);

        snprintf(buf, sizeof(buf), "HR %u bpm  SpO2 %u%%  %.1f C",
                 r.hr, r.spo2, r.tempMilliC / 1000.0);
        drawLine(7, buf, TFT_WHITE);

        if (gradedOnce && !criterion3) {
            drawLine(9, "No stream: check 3V3 rail + SDA/SCL", kRed);
        }
    }

    delay(5);
}
