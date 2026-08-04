// ─────────────────────────────────────────────────────────────────────────────
// Stage 1a — identify the LCD controller.   pio run -e t1a_lcdid -t upload
//
// 2.4" Uno-format shields are sold with any of a dozen different controllers
// behind an identical-looking panel, and the silkscreen never says which. The
// driver has to be chosen at compile time, so it has to be identified first.
//
// This talks to the 8-bit bus directly with digitalRead/digitalWrite. That is
// far too slow to draw with, but it depends on no display library at all,
// which is the point: it works before we know what we are talking to.
//
// It reads every ID register the common controllers answer on, and tries both
// register-addressing conventions, because they disagree:
//
//   * ILI9341 / ILI948x / HX8357 / ST7789  — 8-bit command, e.g. 0xD3
//   * ILI932x / SPFD5408 / R61505 / S6D0154 — 16-bit register index, so the
//     index is written as two bytes and the answer comes back as two
//
// Output is the raw hex for every probe (so an unlisted controller can still
// be decoded by hand) followed by a verdict naming the exact build flag.
//
// Exit criterion: a confident verdict, or raw values to look up.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>

#include "pins.h"

static const uint8_t kData[8] = {
    PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
    PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7,
};

// ── Raw bus primitives ──────────────────────────────────────────────────────
// Deliberately unhurried. The read path in particular has a real setup time
// (ILI9341 wants ~340 ns after RD falls before the data is valid) and the
// whole point of this tool is to be trusted, not fast.

static void dataAsOutput() {
    for (uint8_t i = 0; i < 8; i++) pinMode(kData[i], OUTPUT);
}

static void dataAsInput() {
    for (uint8_t i = 0; i < 8; i++) pinMode(kData[i], INPUT);
}

static void write8(uint8_t v) {
    for (uint8_t i = 0; i < 8; i++) digitalWrite(kData[i], (v >> i) & 1);
    digitalWrite(PIN_LCD_WR, LOW);
    delayMicroseconds(1);
    digitalWrite(PIN_LCD_WR, HIGH);
    delayMicroseconds(1);
}

static uint8_t read8() {
    digitalWrite(PIN_LCD_RD, LOW);
    delayMicroseconds(2);            // comfortably past the 340 ns access time
    uint8_t v = 0;
    for (uint8_t i = 0; i < 8; i++) v |= (digitalRead(kData[i]) ? 1 : 0) << i;
    digitalWrite(PIN_LCD_RD, HIGH);
    delayMicroseconds(1);
    return v;
}

static void busBegin() {
    const uint8_t ctrl[] = {PIN_LCD_RD, PIN_LCD_WR, PIN_LCD_RS,
                            PIN_LCD_CS, PIN_LCD_RST};
    for (uint8_t p : ctrl) {
        pinMode(p, OUTPUT);
        digitalWrite(p, HIGH);       // all control lines idle high
    }
    dataAsOutput();
}

static void lcdReset() {
    digitalWrite(PIN_LCD_RST, HIGH); delay(50);
    digitalWrite(PIN_LCD_RST, LOW);  delay(100);
    digitalWrite(PIN_LCD_RST, HIGH); delay(200);
}

// Wake the controller before asking it anything.
//
// This is not optional and it is easy to miss: a controller comes out of
// hardware reset in sleep mode, where it acknowledges nothing and never
// drives the data bus. Probing it there reads back only the ESP32's own pin
// capacitance, which looks exactly like a disconnected shield - the bus-drive
// check below will (correctly) report "nothing driving the bus" and the real,
// working display gets blamed for a wiring fault it does not have.
//
// SLPOUT (0x11) needs 120 ms to settle before the chip will answer.
static void lcdWake() {
    digitalWrite(PIN_LCD_CS, LOW);
    digitalWrite(PIN_LCD_RS, LOW);
    dataAsOutput();
    write8(0x11);                    // SLPOUT
    digitalWrite(PIN_LCD_CS, HIGH);
    delay(120);
}

// ── Probe 1: 8-bit command, n-byte answer (ILI9341 and friends) ─────────────
static void probe8(uint8_t cmd, uint8_t nbytes, uint8_t* out) {
    digitalWrite(PIN_LCD_CS, LOW);
    digitalWrite(PIN_LCD_RS, LOW);          // command phase
    dataAsOutput();
    write8(cmd);
    digitalWrite(PIN_LCD_RS, HIGH);         // data phase
    dataAsInput();
    for (uint8_t i = 0; i < nbytes; i++) out[i] = read8();
    dataAsOutput();
    digitalWrite(PIN_LCD_CS, HIGH);
}

// ── Probe 2: 16-bit register index, 16-bit answer (ILI932x and friends) ─────
static uint16_t probe16(uint16_t reg) {
    digitalWrite(PIN_LCD_CS, LOW);
    digitalWrite(PIN_LCD_RS, LOW);
    dataAsOutput();
    write8(reg >> 8);
    write8(reg & 0xFF);
    digitalWrite(PIN_LCD_RS, HIGH);
    dataAsInput();
    uint8_t hi = read8();
    uint8_t lo = read8();
    dataAsOutput();
    digitalWrite(PIN_LCD_CS, HIGH);
    return ((uint16_t)hi << 8) | lo;
}

// ── Is anything driving the bus during a read? ──────────────────────────────
// An undriven bus is not silent. The ESP32's pin capacitance holds the last
// value we drove for microseconds, so reads come back as a ghost of our own
// writes - easy to mistake for a controller answering badly.
//
// IMPORTANT - what this can and cannot tell you. It writes an arbitrary byte
// as a command and re-reads, so a "floating" result has TWO possible causes
// and this test cannot separate them:
//
//   a) the bus really is broken/unpowered, or
//   b) the controller is fine but does not drive the bus in response to this
//      command - which is the normal, correct behaviour for any command that
//      is not a read command.
//
// So a floating result is a hint, never a verdict. The panel on this build
// reports floating here and yet drives GRAM reads perfectly (Stage 1b test 8
// round-trips pixels exactly) - it simply does not implement the ID
// registers, which is common on clone shields.
static bool busIsFloating() {
    const uint8_t patterns[] = {0xAA, 0x55};
    uint8_t echo[2];

    for (uint8_t i = 0; i < 2; i++) {
        digitalWrite(PIN_LCD_CS, LOW);
        digitalWrite(PIN_LCD_RS, LOW);
        dataAsOutput();
        write8(patterns[i]);
        digitalWrite(PIN_LCD_RS, HIGH);
        dataAsInput();
        echo[i] = read8();
        dataAsOutput();
        digitalWrite(PIN_LCD_CS, HIGH);
    }

    // Count how many bits came back matching what we drove.
    uint8_t agree = 0;
    for (uint8_t b = 0; b < 8; b++) {
        if (((echo[0] >> b) & 1) == ((patterns[0] >> b) & 1)) agree++;
        if (((echo[1] >> b) & 1) == ((patterns[1] >> b) & 1)) agree++;
    }

    Serial.println();
    Serial.println("Bus drive check (writes 0xAA then 0x55 and re-reads):");
    Serial.printf("  wrote 0xAA -> read 0x%02X\n", echo[0]);
    Serial.printf("  wrote 0x55 -> read 0x%02X\n", echo[1]);
    Serial.printf("  %u of 16 bits echoed our own write\n", agree);

    // Two opposite patterns agreeing on 13+ of 16 bits cannot happen if a
    // real device is driving; it is our own charge decaying.
    return agree >= 13;
}

// Which individual lines are unconnected. A floating pin follows whichever
// internal pull we apply; a pin tied to something external resists it.
static void reportFloatingPins() {
    Serial.println();
    Serial.println("Per-line float check (INPUT_PULLUP vs INPUT_PULLDOWN):");
    const char* names[8] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};
    for (uint8_t i = 0; i < 8; i++) {
        pinMode(kData[i], INPUT_PULLUP);   delayMicroseconds(200);
        int up = digitalRead(kData[i]);
        pinMode(kData[i], INPUT_PULLDOWN); delayMicroseconds(200);
        int dn = digitalRead(kData[i]);
        pinMode(kData[i], INPUT);

        const char* verdict;
        if (up == 1 && dn == 0)      verdict = "floating  (nothing holding it)";
        else if (up == 1 && dn == 1) verdict = "held HIGH by something";
        else if (up == 0 && dn == 0) verdict = "held LOW  by something";
        else                         verdict = "inverted?? recheck wiring";
        Serial.printf("  %s (GPIO%2d): pullup=%d pulldown=%d  -> %s\n",
                      names[i], kData[i], up, dn, verdict);
    }
    dataAsOutput();
}

// ── Verdict ─────────────────────────────────────────────────────────────────
struct Known {
    uint16_t id;
    const char* chip;
    const char* flag;      // nullptr => TFT_eSPI cannot drive it
};

// TFT_eSPI's parallel path covers the ILI934x/948x, ST77xx and HX8357
// families. The older ILI932x-class parts are a different command set
// entirely, and no build flag will make TFT_eSPI speak it.
static const Known kKnown[] = {
    {0x9341, "ILI9341",  "-DILI9341_DRIVER=1"},
    // ILI9342 must NOT be mapped onto the ILI9341 flag. They share an init
    // sequence and a command set, but their native geometry differs -
    // 240x320 against 320x240 - and the wrong one draws into the left 240
    // columns of a 320-wide panel while the bottom 80 rows fall off, with no
    // other symptom.
    {0x9342, "ILI9342",  "-DILI9342_DRIVER=1"},
    {0x9481, "ILI9481",  "-DILI9481_DRIVER=1"},
    {0x9486, "ILI9486",  "-DILI9486_DRIVER=1"},
    {0x9488, "ILI9488",  "-DILI9488_DRIVER=1"},
    {0x7789, "ST7789",   "-DST7789_DRIVER=1"},
    {0x7796, "ST7796",   "-DST7796_DRIVER=1"},
    {0x8357, "HX8357",   "-DHX8357D_DRIVER=1"},
    {0x6814, "RM68140",  "-DRM68140_DRIVER=1"},
    {0x9325, "ILI9325",  nullptr},
    {0x9328, "ILI9328",  nullptr},
    {0x9320, "ILI9320",  nullptr},
    {0x5408, "SPFD5408", nullptr},
    {0x1505, "R61505",   nullptr},
    {0x0154, "S6D0154",  nullptr},
    {0x7783, "ST7781",   nullptr},
};

static const Known* lookup(uint16_t id) {
    for (const auto& k : kKnown) if (k.id == id) return &k;
    return nullptr;
}

static void report(uint16_t id, const char* via) {
    const Known* k = lookup(id);
    Serial.println();
    Serial.println("=====================================================");
    if (!k) {
        Serial.printf(" Controller ID 0x%04X (via %s) is not in the table.\n", id, via);
        Serial.println(" Search that number - the shields are well documented.");
    } else if (k->flag) {
        Serial.printf(" Controller: %s   (ID 0x%04X via %s)\n", k->chip, id, via);
        Serial.println();
        Serial.println(" In platformio.ini, replace the provisional");
        Serial.println("     -DILI9341_DRIVER=1");
        Serial.printf( " with %s\n", k->flag);
        Serial.println(" then run:  pio run -e t1_display -t upload");
    } else {
        Serial.printf(" Controller: %s   (ID 0x%04X via %s)\n", k->chip, id, via);
        Serial.println();
        Serial.println(" TFT_eSPI CANNOT drive this one - it is the older");
        Serial.println(" ILI932x-style command set, not a register-compatible");
        Serial.println(" part. The project must switch to MCUFRIEND_kbv for");
        Serial.println(" the display layer. Everything above the driver (UI,");
        Serial.println(" buttons, protocol) is unaffected.");
    }
    Serial.println("=====================================================");
}

// A read that comes back all-0x00 or all-0xFF is a wiring fault, not an ID:
// 0xFF means nothing is driving the bus, 0x00 means it is stuck low.
static bool plausible(uint16_t v) { return v != 0x0000 && v != 0xFFFF; }

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 1a - LCD controller identification");
    Serial.println("=====================================================");
    Serial.printf("  data bus ... D0=%d D1=%d D2=%d D3=%d D4=%d D5=%d D6=%d D7=%d\n",
                  PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
                  PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);
    Serial.printf("  control .... RD=%d WR=%d RS=%d CS=%d RST=%d\n",
                  PIN_LCD_RD, PIN_LCD_WR, PIN_LCD_RS, PIN_LCD_CS, PIN_LCD_RST);
    Serial.println("-----------------------------------------------------");

    busBegin();
    lcdReset();
    lcdWake();

    // ── Is there anything on the other end at all? ──────────────────────────
    // Done first: if the bus is floating, every ID probe below is noise and
    // saying so up front stops a wiring fault being chased as a driver bug.
    bool floating = busIsFloating();
    reportFloatingPins();

    if (floating) {
        Serial.println();
        Serial.println("=====================================================");
        Serial.println(" The controller did not drive the bus on a read.");
        Serial.println();
        Serial.println(" This is NOT proof of a fault. The likeliest cause by");
        Serial.println(" far is a panel that implements the ILI9341 command");
        Serial.println(" set but not its ID registers - very common on clone");
        Serial.println(" shields, and harmless: nothing in this project ever");
        Serial.println(" reads an ID at run time.");
        Serial.println();
        Serial.println(" SETTLE IT BY LOOKING AT THE PANEL:");
        Serial.println("     pio run -e t1_display -t upload");
        Serial.println(" If the test patterns appear, the bus and the driver");
        Serial.println(" are both correct and this whole test is moot.");
        Serial.println();
        Serial.println(" Only if the panel stays blank is it worth checking:");
        Serial.println("  1. Is the shield POWERED? Its 5V pin must go to");
        Serial.println("     ESP32 VIN - its own regulator makes the panel's");
        Serial.println("     3.3V, so feeding it 3.3V gives ~2.2V out and the");
        Serial.println("     controller never starts. The shield's 3V3 pin");
        Serial.println("     should measure ~3.3V.");
        Serial.println("  2. Is GND shared with the ESP32?");
        Serial.println("  3. Are D8/D9 wired to GPIO13/14? They carry LCD_D0");
        Serial.println("     and LCD_D1, not what their labels suggest.");
        Serial.println();
        Serial.println(" Probing anyway, for the record:");
        Serial.println("=====================================================");
    }

    // ── 8-bit command probes ────────────────────────────────────────────────
    Serial.println("8-bit command probes (ILI9341 / ILI948x / ST77xx / HX8357):");
    struct { uint8_t cmd; uint8_t n; const char* what; } probes[] = {
        {0x04, 4, "RDDID   "},
        {0xD3, 4, "RDDID4  "},   // ILI9341: xx 00 93 41
        {0xBF, 6, "RDID_EXT"},   // ILI9481/9486/9488
        {0xEF, 4, "RDID_ALT"},
        {0xD0, 4, "RDID_D0 "},
        {0x09, 5, "RDDST   "},
    };

    uint16_t id8 = 0;
    const char* id8_from = "";
    for (auto& p : probes) {
        uint8_t buf[8] = {0};
        probe8(p.cmd, p.n, buf);
        Serial.printf("  0x%02X %s ->", p.cmd, p.what);
        for (uint8_t i = 0; i < p.n; i++) Serial.printf(" %02X", buf[i]);

        // The ID is the last two meaningful bytes; the leading ones are a
        // dummy byte and a manufacturer code that varies between batches.
        uint16_t cand = ((uint16_t)buf[p.n - 2] << 8) | buf[p.n - 1];
        if (lookup(cand) && !id8) { id8 = cand; id8_from = p.what; }
        Serial.printf("    (id candidate 0x%04X%s)\n",
                      cand, lookup(cand) ? " <= MATCH" : "");
    }

    // ── 16-bit register probes ──────────────────────────────────────────────
    Serial.println();
    Serial.println("16-bit register probes (ILI932x / SPFD5408 / R61505):");
    uint16_t id16 = 0;
    const uint16_t regs[] = {0x0000, 0x0067, 0x00BF};
    for (uint16_t r : regs) {
        uint16_t v = probe16(r);
        Serial.printf("  reg 0x%04X -> 0x%04X%s\n",
                      r, v, lookup(v) ? "   <= MATCH" : "");
        if (lookup(v) && !id16) id16 = v;
    }

    // ── Verdict ─────────────────────────────────────────────────────────────
    if (floating) {
        Serial.println();
        Serial.println("=====================================================");
        Serial.println(" NO ID AVAILABLE - this panel does not answer ID");
        Serial.println(" register reads. Fall back to trying a driver and");
        Serial.println(" looking at the result, starting with ILI9341 (which");
        Serial.println(" is what the vast majority of 240x320 2.4\" shields");
        Serial.println(" are, and what this build is already set to):");
        Serial.println("     pio run -e t1_display -t upload");
        Serial.println("=====================================================");
    } else if (id8 && plausible(id8)) {
        report(id8, id8_from);
    } else if (id16 && plausible(id16)) {
        report(id16, "16-bit register 0x00");
    } else {
        Serial.println();
        Serial.println("=====================================================");
        Serial.println(" No controller identified.");
        Serial.println();
        Serial.println(" All-FF everywhere => nothing is driving the bus:");
        Serial.println("   - is the shield powered? 5V must reach its 5V pin,");
        Serial.println("     because its onboard regulator makes the panel's");
        Serial.println("     3.3V. Feeding 3.3V in gives ~2.2V out and the");
        Serial.println("     controller never starts.");
        Serial.println("   - is RD (shield A0) actually wired? Reading is the");
        Serial.println("     only thing this test does.");
        Serial.println(" All-00 everywhere => a data line is shorted to GND,");
        Serial.println("   or CS/RS are swapped.");
        Serial.println(" Garbage that changes each run => check D0/D1. They are");
        Serial.println("   on shield pins D8/D9, NOT D0/D1. See WIRING.md.");
        Serial.println("=====================================================");
    }
}

void loop() {
    delay(5000);
}
