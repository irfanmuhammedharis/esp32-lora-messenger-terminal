// Stage 6 — NMEA parser and the SHARE LOC + VITALS messages, on the host.
//     pio test -e native

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "Gps.h"
#include "MessageStore.h"
#include "Share.h"

static NmeaParser p;

void setUp(void) { p = NmeaParser{}; }
void tearDown(void) {}

// Feed raw bytes. True if any byte completed an applied sentence.
static bool feedN(const char *s, size_t n, uint32_t nowMs) {
    bool any = false;
    for (size_t i = 0; i < n; i++) any |= p.feed(s[i], nowMs);
    return any;
}

static bool feedStr(const char *s, uint32_t nowMs) {
    return feedN(s, strlen(s), nowMs);
}

// Wrap a sentence body as "$<body>*HH\r\n" with a computed checksum, so the
// synthetic sentences below cannot fail on a hand-typed checksum. The two
// reference sentences in the first test pin the checksum algorithm itself.
static bool feedBody(const char *body, uint32_t nowMs) {
    uint8_t sum = 0;
    for (const char *c = body; *c; c++) sum ^= static_cast<uint8_t>(*c);
    char buf[160];
    snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, sum);
    return feedStr(buf, nowMs);
}

// ── Parsing ─────────────────────────────────────────────────────────────────

// The textbook RMC and GGA pair, checksums as published: 48 07.038' N,
// 11 31.000' E. Converting from degrees+minutes is where a parser most often
// goes wrong - reading 4807.038 as 48.07038 degrees lands ~3 km off.
static void test_reference_sentences(void) {
    TEST_ASSERT_TRUE(feedStr(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n",
        1000));
    const GpsFix &f = p.fix();
    TEST_ASSERT_TRUE(f.hasPos);
    TEST_ASSERT_TRUE(f.live);
    TEST_ASSERT_EQUAL_INT32(48117300, f.latE6);   // 48 + 7.038/60
    TEST_ASSERT_EQUAL_INT32(11516667, f.lonE6);   // 11 + 31/60, rounded
    TEST_ASSERT_EQUAL_UINT32(1000, f.fixMs);

    TEST_ASSERT_TRUE(feedStr(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n",
        2000));
    TEST_ASSERT_EQUAL_UINT8(8, p.fix().sats);
    TEST_ASSERT_EQUAL_UINT16(9, p.fix().hdopX10);
    TEST_ASSERT_EQUAL_UINT32(2000, p.fix().fixMs);
    TEST_ASSERT_EQUAL_UINT32(2, p.sentencesOk());
    TEST_ASSERT_EQUAL_UINT32(0, p.checksumErrors());
}

// One digit changed, checksum left alone: exactly what line noise does.
static void test_bad_checksum_rejected(void) {
    TEST_ASSERT_FALSE(feedStr(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B\r\n",
        1000));
    TEST_ASSERT_FALSE(feedStr(
        "$GPRMC,123519,A,4807.039,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n",
        1000));
    TEST_ASSERT_FALSE(p.fix().hasPos);
    TEST_ASSERT_FALSE(p.fix().heard);
    TEST_ASSERT_EQUAL_UINT32(2, p.checksumErrors());
}

// Optional in the standard, mandatory here: without it a floating pin could
// assemble a sentence.
static void test_missing_checksum_rejected(void) {
    TEST_ASSERT_FALSE(feedStr(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W\r\n",
        1000));
    TEST_ASSERT_FALSE(p.fix().hasPos);
    TEST_ASSERT_EQUAL_UINT32(1, p.checksumErrors());
}

static void test_no_fix_rmc(void) {
    TEST_ASSERT_TRUE(feedBody("GPRMC,081836,V,,,,,,,130998,,,N", 500));
    const GpsFix &f = p.fix();
    TEST_ASSERT_TRUE(f.heard);
    TEST_ASSERT_FALSE(f.hasPos);
    TEST_ASSERT_FALSE(f.live);
    TEST_ASSERT_EQUAL_UINT32(500, f.lastSentenceMs);
}

// Losing the fix keeps the last position - it is still worth sharing, as
// LAST - but stops calling it live, immediately rather than after a timeout.
static void test_fix_lost_keeps_position(void) {
    feedBody("GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,A", 1000);
    TEST_ASSERT_TRUE(gpsFixFresh(p.fix(), 1000));
    const int32_t lat = p.fix().latE6, lon = p.fix().lonE6;

    TEST_ASSERT_TRUE(feedBody("GPRMC,120001,V,,,,,,,280926,,,N", 2000));
    TEST_ASSERT_TRUE(p.fix().hasPos);
    TEST_ASSERT_FALSE(p.fix().live);
    TEST_ASSERT_EQUAL_INT32(lat, p.fix().latE6);
    TEST_ASSERT_EQUAL_INT32(lon, p.fix().lonE6);
    TEST_ASSERT_EQUAL_UINT32(1000, p.fix().fixMs);
    TEST_ASSERT_FALSE(gpsFixFresh(p.fix(), 2000));
}

static void test_south_and_west_are_negative(void) {
    TEST_ASSERT_TRUE(feedBody(
        "GPRMC,120000,A,3345.12345,S,07015.54321,W,0.0,0.0,280926,,,A", 0));
    TEST_ASSERT_EQUAL_INT32(-33752058, p.fix().latE6);   // 33 + 45.12345/60
    TEST_ASSERT_EQUAL_INT32(-70259054, p.fix().lonE6);   // 70 + 15.54321/60
}

// Newer u-blox parts and multi-constellation modules say GN, not GP.
static void test_other_talkers_accepted(void) {
    TEST_ASSERT_TRUE(feedBody(
        "GNRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,A", 0));
    TEST_ASSERT_TRUE(p.fix().hasPos);
}

// Dead-reckoning and "not valid" mode letters veto a status-A RMC.
static void test_rmc_mode_letter_vetoes(void) {
    feedBody("GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,E", 0);
    TEST_ASSERT_FALSE(p.fix().hasPos);
    feedBody("GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,N", 0);
    TEST_ASSERT_FALSE(p.fix().hasPos);
    // NMEA 2.2 has no mode field at all; status A alone must still count.
    feedBody("GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,", 0);
    TEST_ASSERT_TRUE(p.fix().hasPos);
}

// While searching, satellites are the only progress the operator can see.
static void test_gga_no_fix_still_reports_sats(void) {
    TEST_ASSERT_TRUE(feedBody("GPGGA,120000,,,,,0,03,99.99,,,,,,", 0));
    TEST_ASSERT_FALSE(p.fix().hasPos);
    TEST_ASSERT_FALSE(p.fix().live);
    TEST_ASSERT_EQUAL_UINT8(3, p.fix().sats);
    TEST_ASSERT_EQUAL_UINT16(999, p.fix().hdopX10);

    // An empty satellite field means none, not "unchanged".
    feedBody("GPGGA,120001,,,,,0,,,,,,,,", 0);
    TEST_ASSERT_EQUAL_UINT8(0, p.fix().sats);
    TEST_ASSERT_EQUAL_UINT16(0, p.fix().hdopX10);
}

static void test_gga_fix_sets_position(void) {
    TEST_ASSERT_TRUE(feedBody(
        "GPGGA,120000,1031.65852,N,07612.86610,E,1,07,1.2,12.0,M,-90.0,M,,", 700));
    TEST_ASSERT_TRUE(p.fix().hasPos);
    TEST_ASSERT_TRUE(p.fix().live);
    TEST_ASSERT_EQUAL_INT32(10527642, p.fix().latE6);   // 10 + 31.65852/60
    TEST_ASSERT_EQUAL_INT32(76214435, p.fix().lonE6);   // 76 + 12.86610/60
    TEST_ASSERT_EQUAL_UINT8(7, p.fix().sats);
    TEST_ASSERT_EQUAL_UINT16(12, p.fix().hdopX10);

    // Quality 6 is a dead-reckoning estimate, not a fix.
    feedBody("GPGGA,120001,1031.65852,N,07612.86610,E,6,00,,,M,,M,,", 800);
    TEST_ASSERT_FALSE(p.fix().live);
    TEST_ASSERT_EQUAL_UINT32(700, p.fix().fixMs);
}

// A checksum only proves the bytes arrived as sent. The fields must still be
// the fixed NMEA shape, or they are refused rather than half-read.
static void test_malformed_coordinates_rejected(void) {
    // 3 integer digits in a latitude (degrees must be 2 wide)
    feedBody("GPRMC,120000,A,807.038,N,01131.000,E,,,280926,,,A", 0);
    TEST_ASSERT_FALSE(p.fix().hasPos);
    // 61 minutes
    feedBody("GPRMC,120000,A,4861.000,N,01131.000,E,,,280926,,,A", 0);
    TEST_ASSERT_FALSE(p.fix().hasPos);
    // hemisphere letter from the wrong axis
    feedBody("GPRMC,120000,A,4807.038,E,01131.000,N,,,280926,,,A", 0);
    TEST_ASSERT_FALSE(p.fix().hasPos);
    // latitude past the pole
    feedBody("GPRMC,120000,A,9100.000,N,01131.000,E,,,280926,,,A", 0);
    TEST_ASSERT_FALSE(p.fix().hasPos);
    // stray character inside a number
    feedBody("GPRMC,120000,A,4807.0x8,N,01131.000,E,,,280926,,,A", 0);
    TEST_ASSERT_FALSE(p.fix().hasPos);
}

// A floating GPIO34 produces exactly this: junk, control bytes, half
// sentences. The next real sentence must come through intact.
static void test_noise_then_resync(void) {
    const char junk[] = {'\x00', '\xff', 'G', '$', 'G', 'P', '\x07', ',', '1',
                         '$', 'G', 'P', 'R', 'M', 'C', ',', '1', '\x80'};
    TEST_ASSERT_FALSE(feedN(junk, sizeof(junk), 0));
    TEST_ASSERT_EQUAL_UINT32(0, p.checksumErrors());
    TEST_ASSERT_TRUE(feedBody(
        "GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,A", 10));
    TEST_ASSERT_TRUE(p.fix().hasPos);
}

// Two sentences run together with no line ending: the second '$' restarts,
// so the second arrives whole.
static void test_dollar_mid_sentence_restarts(void) {
    feedStr("$GPGSV,3,1,11,03,03,111,00", 0);   // cut off - no CRLF
    TEST_ASSERT_TRUE(feedBody(
        "GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,A", 10));
    TEST_ASSERT_TRUE(p.fix().hasPos);
    TEST_ASSERT_EQUAL_UINT32(0, p.checksumErrors());
}

static void test_overlong_sentence_dropped(void) {
    char longLine[GPS_LINE_MAX + 40];
    longLine[0] = '$';
    memset(longLine + 1, 'A', sizeof(longLine) - 3);
    longLine[sizeof(longLine) - 2] = '\n';
    longLine[sizeof(longLine) - 1] = '\0';
    TEST_ASSERT_FALSE(feedStr(longLine, 0));
    TEST_ASSERT_EQUAL_UINT32(1, p.overruns());
    TEST_ASSERT_EQUAL_UINT32(0, p.checksumErrors());

    TEST_ASSERT_TRUE(feedBody(
        "GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,A", 10));
    TEST_ASSERT_TRUE(p.fix().hasPos);
}

// GSV, GSA, VTG... pass the checksum and prove the module is alive, but
// carry nothing the fix uses.
static void test_other_sentences_counted_not_applied(void) {
    TEST_ASSERT_FALSE(feedBody("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00", 1234));
    TEST_ASSERT_EQUAL_UINT32(1, p.sentencesOk());
    TEST_ASSERT_TRUE(p.fix().heard);
    TEST_ASSERT_EQUAL_UINT32(1234, p.fix().lastSentenceMs);
    TEST_ASSERT_FALSE(p.fix().hasPos);
}

// ── Freshness ───────────────────────────────────────────────────────────────

static void test_fix_freshness_window(void) {
    feedBody("GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,A", 1000);
    const GpsFix &f = p.fix();
    TEST_ASSERT_TRUE(gpsFixFresh(f, 1000));
    TEST_ASSERT_TRUE(gpsFixFresh(f, 1000 + GPS_FIX_STALE_MS - 1));
    TEST_ASSERT_FALSE(gpsFixFresh(f, 1000 + GPS_FIX_STALE_MS));

    // Stamped a moment after the caller read its clock: fresh, age zero.
    TEST_ASSERT_TRUE(gpsFixFresh(f, 990));
    TEST_ASSERT_EQUAL_UINT32(0, gpsFixAgeMs(f, 990));

    // A module unplugged for 25 days is not fresh again - the trap a signed
    // 32-bit difference falls into after 24.8 days.
    TEST_ASSERT_FALSE(gpsFixFresh(f, 1000 + 25u * 24 * 3600 * 1000));
}

static void test_fix_freshness_across_millis_wrap(void) {
    const uint32_t t = 0xFFFFFF00u;   // 256 ms before millis() wraps
    feedBody("GPRMC,120000,A,1031.65852,N,07612.86610,E,0.1,,280926,,,A", t);
    TEST_ASSERT_TRUE(gpsFixFresh(p.fix(), t + 1000));   // wrapped to 744
    TEST_ASSERT_EQUAL_UINT32(1000, gpsFixAgeMs(p.fix(), t + 1000));
}

static void test_module_alive(void) {
    TEST_ASSERT_FALSE(gpsModuleAlive(p.fix(), 0));
    feedBody("GPGSV,1,1,00", 5000);
    TEST_ASSERT_TRUE(gpsModuleAlive(p.fix(), 5000 + GPS_SILENT_MS - 1));
    TEST_ASSERT_FALSE(gpsModuleAlive(p.fix(), 5000 + GPS_SILENT_MS));
}

// ── Formatting ──────────────────────────────────────────────────────────────

static void test_format_coord(void) {
    char b[16];
    formatCoordE6(b, sizeof(b), 48117300);   TEST_ASSERT_EQUAL_STRING("48.11730", b);
    formatCoordE6(b, sizeof(b), -33752058);  TEST_ASSERT_EQUAL_STRING("-33.75206", b);
    formatCoordE6(b, sizeof(b), 76214435);   TEST_ASSERT_EQUAL_STRING("76.21444", b);
    formatCoordE6(b, sizeof(b), 12999996);   TEST_ASSERT_EQUAL_STRING("13.00000", b);
    formatCoordE6(b, sizeof(b), 5);          TEST_ASSERT_EQUAL_STRING("0.00001", b);
    formatCoordE6(b, sizeof(b), -4);         TEST_ASSERT_EQUAL_STRING("0.00000", b);
    formatCoordE6(b, sizeof(b), 0);          TEST_ASSERT_EQUAL_STRING("0.00000", b);
    formatCoordE6(b, sizeof(b), -180000000); TEST_ASSERT_EQUAL_STRING("-180.00000", b);
}

static void test_location_msg_states(void) {
    char m[MSG_MAX_LEN + 1];
    GpsFix f;

    formatLocationMsg(m, sizeof(m), f, 0);
    TEST_ASSERT_EQUAL_STRING("LOC NO GPS FIX", m);

    feedStr("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n",
            10000);
    formatLocationMsg(m, sizeof(m), p.fix(), 10500);
    TEST_ASSERT_EQUAL_STRING("LOC 48.11730,11.51667", m);

    // Aged out by time (module went quiet)...
    formatLocationMsg(m, sizeof(m), p.fix(), 10000 + 125000);
    TEST_ASSERT_EQUAL_STRING("LAST 48.11730,11.51667 2m", m);

    // ...or by the module saying so, however recent.
    feedBody("GPRMC,123520,V,,,,,,,230394,,,N", 11000);
    formatLocationMsg(m, sizeof(m), p.fix(), 12000);
    TEST_ASSERT_EQUAL_STRING("LAST 48.11730,11.51667 2s", m);

    formatLocationMsg(m, sizeof(m), p.fix(), 10000 + 3u * 3600 * 1000);
    TEST_ASSERT_EQUAL_STRING("LAST 48.11730,11.51667 3h", m);
}

// The radio drops everything past MSG_MAX_LEN without a word, so the longest
// line this can ever produce is proved here rather than hoped for.
static void test_location_msg_worst_case_fits(void) {
    char m[64];
    GpsFix f;
    f.hasPos = true;
    f.live   = false;
    f.latE6  = -89999999;
    f.lonE6  = -179999999;
    f.fixMs  = 0;

    formatLocationMsg(m, sizeof(m), f, 0xFFFFFFFFu - 60001u);   // ~1193 h
    TEST_ASSERT_EQUAL_STRING("LAST -90.00000,-180.00000 1193h", m);
    TEST_ASSERT_TRUE_MESSAGE(strlen(m) <= MSG_MAX_LEN, m);

    f.live = true;
    formatLocationMsg(m, sizeof(m), f, 0);
    TEST_ASSERT_EQUAL_STRING("LOC -90.00000,-180.00000", m);
    TEST_ASSERT_TRUE(strlen(m) <= MSG_MAX_LEN);
}

static void test_vitals_msg(void) {
    char m[MSG_MAX_LEN + 1];

    formatVitalsMsg(m, sizeof(m), true, 72, true, 98, true, 36600);
    TEST_ASSERT_EQUAL_STRING("HR 72 SPO2 98% T 36.6C", m);

    // A value the health gates rejected is a dash, never a number.
    formatVitalsMsg(m, sizeof(m), false, 72, false, 98, false, 36600);
    TEST_ASSERT_EQUAL_STRING("HR -- SPO2 --% T --.-C", m);

    formatVitalsMsg(m, sizeof(m), true, 120, false, 0, true, 38000);
    TEST_ASSERT_EQUAL_STRING("HR 120 SPO2 --% T 38.0C", m);

    // Rounded to the nearest tenth, not truncated.
    formatVitalsMsg(m, sizeof(m), false, 0, false, 0, true, 36650);
    TEST_ASSERT_EQUAL_STRING("HR -- SPO2 --% T 36.7C", m);
    formatVitalsMsg(m, sizeof(m), false, 0, false, 0, true, 36549);
    TEST_ASSERT_EQUAL_STRING("HR -- SPO2 --% T 36.5C", m);
}

static void test_vitals_msg_worst_case_fits(void) {
    char m[64];
    formatVitalsMsg(m, sizeof(m), true, 30000, true, -5, true, 2000000);
    TEST_ASSERT_EQUAL_STRING("HR 999 SPO2 0% T 999.9C", m);
    TEST_ASSERT_TRUE(strlen(m) <= MSG_MAX_LEN);

    formatVitalsMsg(m, sizeof(m), true, -1, true, 250, true, -123456);
    TEST_ASSERT_EQUAL_STRING("HR 0 SPO2 100% T -99.9C", m);
    TEST_ASSERT_TRUE(strlen(m) <= MSG_MAX_LEN);
}

// A shared position must never trip the far end's SOS takeover, which fires
// on any message starting with "SOS".
static void test_share_msgs_are_not_sos(void) {
    char m[MSG_MAX_LEN + 1];
    GpsFix f;
    formatLocationMsg(m, sizeof(m), f, 0);
    TEST_ASSERT_FALSE(MessageStore::isSos(m));
    f.hasPos = f.live = true;
    formatLocationMsg(m, sizeof(m), f, 0);
    TEST_ASSERT_FALSE(MessageStore::isSos(m));
    f.live = false;
    formatLocationMsg(m, sizeof(m), f, 0);
    TEST_ASSERT_FALSE(MessageStore::isSos(m));
    formatVitalsMsg(m, sizeof(m), true, 72, true, 98, true, 36600);
    TEST_ASSERT_FALSE(MessageStore::isSos(m));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_reference_sentences);
    RUN_TEST(test_bad_checksum_rejected);
    RUN_TEST(test_missing_checksum_rejected);
    RUN_TEST(test_no_fix_rmc);
    RUN_TEST(test_fix_lost_keeps_position);
    RUN_TEST(test_south_and_west_are_negative);
    RUN_TEST(test_other_talkers_accepted);
    RUN_TEST(test_rmc_mode_letter_vetoes);
    RUN_TEST(test_gga_no_fix_still_reports_sats);
    RUN_TEST(test_gga_fix_sets_position);
    RUN_TEST(test_malformed_coordinates_rejected);
    RUN_TEST(test_noise_then_resync);
    RUN_TEST(test_dollar_mid_sentence_restarts);
    RUN_TEST(test_overlong_sentence_dropped);
    RUN_TEST(test_other_sentences_counted_not_applied);
    RUN_TEST(test_fix_freshness_window);
    RUN_TEST(test_fix_freshness_across_millis_wrap);
    RUN_TEST(test_module_alive);
    RUN_TEST(test_format_coord);
    RUN_TEST(test_location_msg_states);
    RUN_TEST(test_location_msg_worst_case_fits);
    RUN_TEST(test_vitals_msg);
    RUN_TEST(test_vitals_msg_worst_case_fits);
    RUN_TEST(test_share_msgs_are_not_sos);
    return UNITY_END();
}
