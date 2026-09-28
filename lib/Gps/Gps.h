#pragma once
//
// NEO-6M GPS: NMEA sentence assembly, checksum, and the position fix.
//
// Split along the same line as LoraLink: NmeaParser contains NO Arduino
// calls, so `pio test -e native` feeds it recorded sentences and grades the
// fix on a PC. Only the GpsReceiver wrapper at the bottom - the part that
// owns UART1 - is device-only, guarded by #ifdef ARDUINO.
//
// ── One wire, receive only ──────────────────────────────────────────────────
// The module streams NMEA at 9600 8N1 from power-up with no configuration, so
// its TX line into GPIO34 is the whole interface (pins.h, WIRING.md §7).
// GPIO34 has no pull-up and floats while no module is fitted; every NMEA
// sentence carries an XOR checksum, and a sentence that fails it is dropped
// whole, so that noise can never become a position.
//
// Only two sentence types are used, whatever the talker (GP, GN, GL, ...):
//
//     $--RMC  fix status (A/V) and position
//     $--GGA  fix quality, satellites used, HDOP, and position
//
// Everything else the module sends (GSV, GSA, VTG, GLL) passes the checksum
// and is counted, then ignored.

#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

struct GpsFix {
    // Position. Meaningful once hasPos is set; it is then the LAST known
    // position, kept after the fix is lost so it can still be shared as such.
    bool     hasPos  = false;
    int32_t  latE6   = 0;      // micro-degrees, positive north
    int32_t  lonE6   = 0;      // micro-degrees, positive east
    uint32_t fixMs   = 0;      // caller's clock when the position last updated

    // True while the module's most recent RMC/GGA reported a valid fix. A
    // sentence saying "no fix" clears it at once, rather than waiting for the
    // position to age past GPS_FIX_STALE_MS.
    bool     live    = false;

    uint8_t  sats    = 0;      // satellites used in the fix (GGA)
    uint16_t hdopX10 = 0;      // horizontal dilution x10 (GGA), 0 = unknown

    // Module presence: any checksum-valid sentence at all.
    bool     heard          = false;
    uint32_t lastSentenceMs = 0;
};

// How old the position is. A fix stamped a moment AFTER `now` - poll() runs
// after the caller read its clock - counts as zero rather than wrapping to
// 49 days; any real age up to millis()'s full 49.7-day range is exact.
uint32_t gpsFixAgeMs(const GpsFix &fix, uint32_t nowMs);

// A fix that is current enough to call "here": the module says it has one and
// the position is younger than GPS_FIX_STALE_MS.
bool gpsFixFresh(const GpsFix &fix, uint32_t nowMs);

// The module has sent a valid sentence within GPS_SILENT_MS. A NEO-6M talks
// every second whether or not it has a fix, so silence means no module.
bool gpsModuleAlive(const GpsFix &fix, uint32_t nowMs);

// "12.34567" / "-123.45678": signed decimal degrees, 5 places (~1.1 m, finer
// than a NEO-6M can resolve). Integer arithmetic, so the host test build and
// the device print the same digits.
void formatCoordE6(char *out, size_t n, int32_t e6);

class NmeaParser {
public:
    // Feed one received byte with the caller's clock. Returns true exactly
    // when a complete, checksum-valid RMC or GGA sentence has been applied to
    // the fix.
    bool feed(char c, uint32_t nowMs);

    // Drop any partially-assembled sentence and forget the fix.
    void reset();

    const GpsFix &fix() const { return fix_; }

    // Diagnostics for the Status screen and the serial report.
    uint32_t sentencesOk() const { return ok_; }      // checksum passed
    uint32_t checksumErrors() const { return bad_; }  // framed, failed checksum
    uint32_t overruns() const { return overruns_; }   // longer than GPS_LINE_MAX

private:
    bool finishSentence(uint32_t nowMs);
    bool applyRmc(char **f, uint8_t nf, uint32_t nowMs);
    bool applyGga(char **f, uint8_t nf, uint32_t nowMs);

    char     line_[GPS_LINE_MAX + 1] = {0};
    uint16_t len_      = 0;
    bool     inLine_   = false;   // a '$' has been seen and not yet ended
    bool     overrun_  = false;   // this sentence already blew the buffer
    GpsFix   fix_;
    uint32_t ok_       = 0;
    uint32_t bad_      = 0;
    uint32_t overruns_ = 0;
};

#ifdef ARDUINO

#include <Arduino.h>

// Device-side wrapper: owns UART1 in receive-only mode on PIN_GPS_RX.
class GpsReceiver {
public:
    void begin();

    // Drain the UART into the parser. Call from loop(). Returns true when the
    // fix was updated by at least one sentence during this call.
    bool poll(uint32_t nowMs);

    const GpsFix &fix() const { return parser_.fix(); }
    const NmeaParser &parser() const { return parser_; }

private:
    NmeaParser parser_;
};

#endif  // ARDUINO
