#include "Share.h"

#include <stdio.h>

// "12s", "4m", "3h" - the same relative age the inbox shows. There is no RTC,
// so an absolute time would be a guess; the receiver reasons about "how old"
// anyway.
static void formatAge(char *buf, size_t n, uint32_t ageMs) {
    const uint32_t sec = ageMs / 1000;
    if (sec < 60)        snprintf(buf, n, "%lus", (unsigned long)sec);
    else if (sec < 3600) snprintf(buf, n, "%lum", (unsigned long)(sec / 60));
    else                 snprintf(buf, n, "%luh", (unsigned long)(sec / 3600));
}

static int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void formatLocationMsg(char *out, size_t n, const GpsFix &fix, uint32_t nowMs) {
    if (!fix.hasPos) {
        snprintf(out, n, "LOC NO GPS FIX");
        return;
    }

    char lat[16], lon[16];
    formatCoordE6(lat, sizeof(lat), fix.latE6);
    formatCoordE6(lon, sizeof(lon), fix.lonE6);

    if (gpsFixFresh(fix, nowMs)) {
        snprintf(out, n, "LOC %s,%s", lat, lon);
        return;
    }

    // A stale position is still worth sending - it is where the sender was,
    // which beats nothing when they need finding - but it must never read as
    // "here". Hence a different keyword, not just an age suffix someone might
    // skim past.
    char ageBuf[12];
    formatAge(ageBuf, sizeof(ageBuf), gpsFixAgeMs(fix, nowMs));
    snprintf(out, n, "LAST %s,%s %s", lat, lon, ageBuf);
}

void formatVitalsMsg(char *out, size_t n,
                     bool hrOk, int hr,
                     bool spo2Ok, int spo2,
                     bool tempOk, int32_t tempMilliC) {
    char h[8], s[8], t[12];

    if (hrOk)   snprintf(h, sizeof(h), "%d", clampInt(hr, 0, 999));
    else        snprintf(h, sizeof(h), "--");
    if (spo2Ok) snprintf(s, sizeof(s), "%d", clampInt(spo2, 0, 100));
    else        snprintf(s, sizeof(s), "--");

    if (tempOk) {
        // Tenths, rounded half away from zero, in integers so both builds
        // print the same digit.
        int32_t t10 = (tempMilliC >= 0 ? tempMilliC + 50 : tempMilliC - 50) / 100;
        if (t10 < -999) t10 = -999;
        if (t10 > 9999) t10 = 9999;
        const int32_t a = t10 < 0 ? -t10 : t10;
        snprintf(t, sizeof(t), "%s%ld.%ld", t10 < 0 ? "-" : "",
                 (long)(a / 10), (long)(a % 10));
    } else {
        snprintf(t, sizeof(t), "--.-");
    }

    snprintf(out, n, "HR %s SPO2 %s%% T %sC", h, s, t);
}
