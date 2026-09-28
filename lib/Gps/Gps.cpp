#include "Gps.h"

#include <stdio.h>
#include <string.h>

// RMC carries 13 fields, GGA 15. Anything past this is not read, so the
// splitter stops there and leaves the tail unsplit.
static constexpr uint8_t kMaxFields = 20;

// ── Freshness ───────────────────────────────────────────────────────────────

// Elapsed time between a stamp and the caller's clock. The fix is stamped
// during poll(), so a caller's `now` read a moment earlier would make the
// unsigned difference wrap to ~4e9 and read as ancient; a "future" stamp
// within a minute is therefore taken as zero. Not a plain signed difference
// (main.cpp's link timing uses one): that wraps after 24.8 days, and a module
// unplugged that long would then read as fresh again.
static uint32_t elapsed(uint32_t stampMs, uint32_t nowMs) {
    const uint32_t d = nowMs - stampMs;
    return d > 0xFFFFFFFFu - 60000u ? 0 : d;
}

uint32_t gpsFixAgeMs(const GpsFix &fix, uint32_t nowMs) {
    return elapsed(fix.fixMs, nowMs);
}

bool gpsFixFresh(const GpsFix &fix, uint32_t nowMs) {
    return fix.hasPos && fix.live &&
           gpsFixAgeMs(fix, nowMs) < GPS_FIX_STALE_MS;
}

bool gpsModuleAlive(const GpsFix &fix, uint32_t nowMs) {
    return fix.heard && elapsed(fix.lastSentenceMs, nowMs) < GPS_SILENT_MS;
}

void formatCoordE6(char *out, size_t n, int32_t e6) {
    // int64 so that negating INT32_MIN cannot overflow.
    int64_t a = e6;
    if (a < 0) a = -a;
    const int64_t u = (a + 5) / 10;   // units of 1e-5 degree, rounded half-up
    // Rounding can carry into the degrees (12.999996 -> 13.00000); dividing
    // the rounded total rather than rounding the fraction alone handles that.
    // A value that rounds to zero prints without a sign: "-0.00000" would
    // read as a real hemisphere.
    snprintf(out, n, "%s%ld.%05ld", (e6 < 0 && u != 0) ? "-" : "",
             (long)(u / 100000), (long)(u % 100000));
}

// ── Field parsers ───────────────────────────────────────────────────────────
// None of these use strtol/atof: an NMEA field is fixed-format, and a parser
// that accepts only that format rejects a corrupted field outright instead of
// reading a plausible number out of the front of it.

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Unsigned integer field. False on an empty field or any non-digit.
static bool parseUint(const char *s, uint32_t &out) {
    if (!s || !*s) return false;
    uint32_t v = 0;
    for (; *s; s++) {
        if (!isDigit(*s)) return false;
        v = v * 10 + static_cast<uint32_t>(*s - '0');
        if (v > 100000) return false;   // no field used here is anywhere near
    }
    out = v;
    return true;
}

// "0.9" / "12.35" -> tenths (9 / 123). Digits past the first decimal are
// dropped; the Status screen shows one.
static bool parseTenths(const char *s, uint16_t &out) {
    if (!s || !*s) return false;
    uint32_t whole = 0, tenth = 0;
    bool any = false;
    while (isDigit(*s)) {
        whole = whole * 10 + static_cast<uint32_t>(*s++ - '0');
        any = true;
        if (whole > 6000) return false;
    }
    if (*s == '.') {
        s++;
        if (isDigit(*s)) {
            tenth = static_cast<uint32_t>(*s++ - '0');
            any = true;
        }
        while (isDigit(*s)) s++;
    }
    if (*s || !any) return false;
    out = static_cast<uint16_t>(whole * 10 + tenth);
    return true;
}

// NMEA position: "ddmm.mmmmm" for latitude, "dddmm.mmmmm" for longitude -
// degrees, then whole minutes as exactly two digits, then decimal minutes -
// plus a one-letter hemisphere field. Result in micro-degrees.
static bool parseCoord(const char *s, const char *hemi, bool isLat,
                       int32_t &out) {
    if (!s || !hemi || hemi[0] == '\0' || hemi[1] != '\0') return false;
    const char pos = isLat ? 'N' : 'E';
    const char neg = isLat ? 'S' : 'W';
    if (hemi[0] != pos && hemi[0] != neg) return false;

    // The degree width is fixed by the format (zero-padded), so a field of
    // any other width is corruption, not a different notation.
    const uint8_t degDigits = isLat ? 2 : 3;
    uint8_t intDigits = 0;
    while (isDigit(s[intDigits])) intDigits++;
    if (intDigits != degDigits + 2) return false;

    int32_t deg = 0;
    for (uint8_t i = 0; i < degDigits; i++) deg = deg * 10 + (s[i] - '0');
    const int32_t minWhole = (s[degDigits] - '0') * 10 + (s[degDigits + 1] - '0');
    if (minWhole >= 60) return false;

    // Decimal minutes to 1e-6 of a minute. The NEO-6M sends five places;
    // anything past six is below what the format can mean and is dropped.
    const char *p = s + intDigits;
    int32_t minFrac = 0;
    uint8_t fd = 0;
    if (*p == '.') {
        p++;
        for (; isDigit(*p); p++) {
            if (fd < 6) {
                minFrac = minFrac * 10 + (*p - '0');
                fd++;
            }
        }
    }
    if (*p) return false;
    for (; fd < 6; fd++) minFrac *= 10;

    const int64_t minutesE6 = static_cast<int64_t>(minWhole) * 1000000 + minFrac;
    int64_t e6 = static_cast<int64_t>(deg) * 1000000 + (minutesE6 + 30) / 60;
    if (e6 > (isLat ? 90000000 : 180000000)) return false;
    if (hemi[0] == neg) e6 = -e6;
    out = static_cast<int32_t>(e6);
    return true;
}

// ── Sentence assembly ───────────────────────────────────────────────────────

bool NmeaParser::feed(char c, uint32_t nowMs) {
    // '$' always starts a sentence, even mid-sentence. That is the resync:
    // after a burst of noise or a dropped byte, the next real sentence is
    // picked up whole instead of being glued to the wreckage of the last.
    if (c == '$') {
        inLine_  = true;
        overrun_ = false;
        len_     = 0;
        return false;
    }
    if (!inLine_) return false;

    if (c == '\r' || c == '\n') {
        inLine_ = false;
        if (overrun_) return false;   // already counted when it overran
        return finishSentence(nowMs);
    }

    // NMEA is printable ASCII. Anything else is line noise - typically a
    // floating GPIO34 with no module fitted - and the sentence is abandoned
    // quietly rather than counted as a checksum failure the module never made.
    if (c < 0x20 || c > 0x7e) {
        inLine_ = false;
        return false;
    }

    if (overrun_) return false;
    if (len_ >= GPS_LINE_MAX) {
        overrun_ = true;
        overruns_++;
        return false;
    }
    line_[len_++] = c;
    return false;
}

void NmeaParser::reset() {
    len_     = 0;
    inLine_  = false;
    overrun_ = false;
    fix_     = GpsFix{};
}

bool NmeaParser::finishSentence(uint32_t nowMs) {
    line_[len_] = '\0';

    // "<body>*HH", where HH is the XOR of every byte between '$' and '*'.
    // The checksum is optional in the NMEA standard but a u-blox always sends
    // it, and without it a floating pin could assemble a "sentence" - so a
    // sentence without one is rejected like a wrong one.
    char *star = strchr(line_, '*');
    if (!star || star == line_ || strlen(star) != 3) {
        bad_++;
        return false;
    }
    const int hi = hexVal(star[1]);
    const int lo = hexVal(star[2]);
    uint8_t sum = 0;
    for (const char *p = line_; p < star; p++) sum ^= static_cast<uint8_t>(*p);
    if (hi < 0 || lo < 0 || sum != static_cast<uint8_t>((hi << 4) | lo)) {
        bad_++;
        return false;
    }
    *star = '\0';

    ok_++;
    fix_.heard          = true;
    fix_.lastSentenceMs = nowMs;

    // Split in place. Empty fields are meaningful - a no-fix RMC is mostly
    // commas - so this walks the commas rather than using strtok, which would
    // merge adjacent ones and shift every field after the gap.
    char   *f[kMaxFields];
    uint8_t nf = 0;
    f[nf++] = line_;
    for (char *p = line_; *p; p++) {
        if (*p != ',') continue;
        if (nf >= kMaxFields) break;
        *p = '\0';
        f[nf++] = p + 1;
    }

    // Two-letter talker (GP, GN, GL, ...) and a three-letter type.
    if (strlen(f[0]) != 5) return false;
    const char *type = f[0] + 2;
    if (strcmp(type, "RMC") == 0) return applyRmc(f, nf, nowMs);
    if (strcmp(type, "GGA") == 0) return applyGga(f, nf, nowMs);
    return false;
}

// $--RMC,time,status,lat,N/S,lon,E/W,speed,course,date,magvar,E/W[,mode]
bool NmeaParser::applyRmc(char **f, uint8_t nf, uint32_t nowMs) {
    if (nf < 7) return false;

    // Status V is the receiver saying "do not use this position". The NMEA
    // 2.3 mode letter, when present, can also veto one: E (dead-reckoning
    // estimate) and N (not valid) are not somewhere anyone should walk to.
    const bool modeOk = nf < 13 || f[12][0] == '\0' ||
                        f[12][0] == 'A' || f[12][0] == 'D';
    if (f[2][0] != 'A' || f[2][1] != '\0' || !modeOk) {
        fix_.live = false;
        return true;
    }

    int32_t lat, lon;
    if (!parseCoord(f[3], f[4], true, lat) ||
        !parseCoord(f[5], f[6], false, lon)) {
        // Claims a fix, but the position will not parse. Trust neither.
        return false;
    }
    fix_.hasPos = true;
    fix_.live   = true;
    fix_.latE6  = lat;
    fix_.lonE6  = lon;
    fix_.fixMs  = nowMs;
    return true;
}

// $--GGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,geoid,M,age,station
bool NmeaParser::applyGga(char **f, uint8_t nf, uint32_t nowMs) {
    if (nf < 9) return false;

    uint32_t q;
    if (!parseUint(f[6], q)) return false;

    // Satellites and HDOP are reported with or without a fix, and while
    // searching they are the only sign of progress the operator gets.
    uint32_t sats;
    fix_.sats = parseUint(f[7], sats)
                    ? static_cast<uint8_t>(sats > 99 ? 99 : sats) : 0;
    uint16_t hdop;
    fix_.hdopX10 = parseTenths(f[8], hdop) ? hdop : 0;

    // Quality 1-5 are real fixes (GPS, DGPS, PPS, RTK, float RTK). 0 is none
    // and 6 is a dead-reckoning estimate; 7/8 are manual and simulated input.
    if (q < 1 || q > 5) {
        fix_.live = false;
        return true;
    }

    int32_t lat, lon;
    if (parseCoord(f[2], f[3], true, lat) &&
        parseCoord(f[4], f[5], false, lon)) {
        fix_.hasPos = true;
        fix_.live   = true;
        fix_.latE6  = lat;
        fix_.lonE6  = lon;
        fix_.fixMs  = nowMs;
    }
    return true;
}

// ── Device-side wrapper ─────────────────────────────────────────────────────
#ifdef ARDUINO

#include "pins.h"

void GpsReceiver::begin() {
    // UART1's own default pins are GPIO9/10 - the SPI flash. RX is passed
    // explicitly and TX as -1: HardwareSerial only falls back to those
    // defaults when BOTH pins are negative, so this attaches GPIO34 alone and
    // leaves UART1's TX unrouted (pins.h has no output pin to give it).
    //
    // The buffer must be set before begin(). The 256-byte default is about a
    // quarter of a second of NMEA at 9600 baud; GPS_RX_BUFFER rides out a
    // slow screen rebuild without dropping a sentence.
    Serial1.setRxBufferSize(GPS_RX_BUFFER);
    Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, -1);
    parser_.reset();
}

bool GpsReceiver::poll(uint32_t nowMs) {
    // Bounded, like LoraLink::poll(): 9600 baud is under a byte per
    // millisecond, so this never has a backlog worth starving LVGL for.
    bool updated = false;
    int  budget  = 256;
    while (Serial1.available() > 0 && budget-- > 0) {
        if (parser_.feed(static_cast<char>(Serial1.read()), nowMs)) {
            updated = true;
        }
    }
    return updated;
}

#endif  // ARDUINO
