// Stage 6 — LoraLink parser, on the host.   pio test -e native
//
// Exit criteria from PLAN.md section 5: "Parser handles both Path A and Path
// B, plus truncation, garbage, partial lines, buffer overrun."

#include <string.h>
#include <unity.h>

#include "LoraLink.h"

static LoraLinkParser p;

void setUp(void) { p = LoraLinkParser{}; }
void tearDown(void) {}

// Push a whole string through the byte-at-a-time interface and return the
// last event it completed. Mirrors how the UART actually delivers data.
static bool feedStr(const char *s, LinkEvent &out) {
    bool got = false;
    for (const char *c = s; *c; c++) {
        LinkEvent ev;
        if (p.feed(*c, ev)) {
            out = ev;
            got = true;
        }
    }
    return got;
}

// ── Path B ──────────────────────────────────────────────────────────────────

static void test_pathB_rx(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr("+RX,2,5,-80,9,sos\n", ev));
    TEST_ASSERT_EQUAL(LinkEventType::Rx, ev.type);
    TEST_ASSERT_EQUAL_UINT8(2, ev.src);
    TEST_ASSERT_EQUAL_UINT16(5, ev.seq);
    TEST_ASSERT_EQUAL_INT16(-80, ev.rssi);
    TEST_ASSERT_EQUAL_INT8(9, ev.snr);
    TEST_ASSERT_EQUAL_STRING("sos", ev.text);
}

static void test_pathB_tx(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr("+TX,4,ALL CLEAR\n", ev));
    TEST_ASSERT_EQUAL(LinkEventType::TxConfirm, ev.type);
    TEST_ASSERT_EQUAL_UINT16(4, ev.seq);
    TEST_ASSERT_EQUAL_STRING("ALL CLEAR", ev.text);
}

// The payload is the LAST field precisely so it never needs escaping.
static void test_pathB_text_may_contain_commas(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr("+RX,3,1,-70,8,HOLD,THEN MOVE\n", ev));
    TEST_ASSERT_EQUAL_STRING("HOLD,THEN MOVE", ev.text);
}

static void test_pathB_negative_snr(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr("+RX,7,12,-119,-11,MEDIC NEEDED\n", ev));
    TEST_ASSERT_EQUAL_INT16(-119, ev.rssi);
    TEST_ASSERT_EQUAL_INT8(-11, ev.snr);
}

// ── Path A: the unmodified Zephyr log lines ─────────────────────────────────

static void test_pathA_rx(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr(
        "[00:00:12.345,000] <inf> lora_mesh: RX from node 2 seq 5 ttl 3 "
        "(RSSI -80 dBm, SNR 9 dB): sos\n", ev));
    TEST_ASSERT_EQUAL(LinkEventType::Rx, ev.type);
    TEST_ASSERT_EQUAL_UINT8(2, ev.src);
    TEST_ASSERT_EQUAL_UINT16(5, ev.seq);
    TEST_ASSERT_EQUAL_INT16(-80, ev.rssi);
    TEST_ASSERT_EQUAL_INT8(9, ev.snr);
    TEST_ASSERT_EQUAL_STRING("sos", ev.text);
}

static void test_pathA_tx(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr(
        "[00:00:12.345,000] <inf> lora_mesh: TX own seq 4: hello 4\n", ev));
    TEST_ASSERT_EQUAL(LinkEventType::TxConfirm, ev.type);
    TEST_ASSERT_EQUAL_UINT16(4, ev.seq);
    TEST_ASSERT_EQUAL_STRING("hello 4", ev.text);
}

// PLAN.md section 3.2 lists log colour as a Path A hazard. The real fix is
// CONFIG_LOG_BACKEND_SHOW_COLOR=n, but surviving it anyway is cheap.
static void test_pathA_survives_ansi_colour(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr(
        "\x1b[1;32m[00:00:12.345,000] <inf> lora_mesh: RX from node 9 seq 1 "
        "ttl 3 (RSSI -55 dBm, SNR 7 dB): IM HERE\x1b[0m\n", ev));
    TEST_ASSERT_EQUAL_UINT8(9, ev.src);
    TEST_ASSERT_EQUAL_STRING("IM HERE", ev.text);
}

// ── Robustness ──────────────────────────────────────────────────────────────

static void test_garbage_is_ignored(void) {
    LinkEvent ev;
    TEST_ASSERT_FALSE(feedStr("*** boot banner ***\n", ev));
    TEST_ASSERT_FALSE(feedStr("<wrn> lora_mesh: something else\n", ev));
    TEST_ASSERT_FALSE(feedStr("+NOPE,1,2\n", ev));
    TEST_ASSERT_FALSE(feedStr("+RX,notanumber,5,-80,9,x\n", ev));
    TEST_ASSERT_FALSE(feedStr("+RX,2,5,-80\n", ev));   // too few fields
    TEST_ASSERT_EQUAL_UINT32(0, p.eventsParsed());
}

// A line only completes on the newline, never before.
static void test_partial_line_yields_nothing_until_newline(void) {
    LinkEvent ev;
    TEST_ASSERT_FALSE(feedStr("+RX,2,5,-80,9,par", ev));
    TEST_ASSERT_EQUAL_UINT32(0, p.linesSeen());
    TEST_ASSERT_TRUE(feedStr("tial\n", ev));
    TEST_ASSERT_EQUAL_STRING("partial", ev.text);
}

static void test_crlf_does_not_emit_a_blank_line(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr("+TX,1,HI\r\n", ev));
    TEST_ASSERT_EQUAL_STRING("HI", ev.text);
    TEST_ASSERT_EQUAL_UINT32(1, p.linesSeen());   // not 2
}

// Text past the radio's 32-byte cap is truncated, never overflowed.
static void test_text_truncated_to_MSG_MAX_LEN(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr(
        "+RX,1,1,-70,8,0123456789012345678901234567890123456789\n", ev));
    TEST_ASSERT_EQUAL_UINT32(MSG_MAX_LEN, strlen(ev.text));
    TEST_ASSERT_EQUAL_STRING("01234567890123456789012345678901", ev.text);
}

// Exactly MSG_MAX_LEN must survive intact - this is the boundary the compose
// screen lets the user reach, so it is the common case, not an edge case.
static void test_text_at_exactly_MSG_MAX_LEN(void) {
    LinkEvent ev;
    TEST_ASSERT_TRUE(feedStr(
        "+RX,1,1,-70,8,01234567890123456789012345678901\n", ev));
    TEST_ASSERT_EQUAL_UINT32(MSG_MAX_LEN, strlen(ev.text));
}

// An over-long line is discarded whole rather than parsed as a prefix:
// inventing a message nobody sent is worse than dropping one.
static void test_buffer_overrun_discards_the_line(void) {
    LinkEvent ev;
    char huge[NRF_LINE_MAX + 64];
    memset(huge, 'A', sizeof(huge));
    huge[0] = '+';
    huge[sizeof(huge) - 2] = '\n';
    huge[sizeof(huge) - 1] = '\0';

    TEST_ASSERT_FALSE(feedStr(huge, ev));
    TEST_ASSERT_EQUAL_UINT32(1, p.overruns());
    TEST_ASSERT_EQUAL_UINT32(0, p.eventsParsed());
}

// ...and the parser must recover cleanly on the very next line.
static void test_parser_recovers_after_overrun(void) {
    LinkEvent ev;
    char huge[NRF_LINE_MAX + 64];
    memset(huge, 'A', sizeof(huge));
    huge[sizeof(huge) - 2] = '\n';
    huge[sizeof(huge) - 1] = '\0';
    feedStr(huge, ev);

    TEST_ASSERT_TRUE(feedStr("+RX,2,5,-80,9,recovered\n", ev));
    TEST_ASSERT_EQUAL_STRING("recovered", ev.text);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_pathB_rx);
    RUN_TEST(test_pathB_tx);
    RUN_TEST(test_pathB_text_may_contain_commas);
    RUN_TEST(test_pathB_negative_snr);
    RUN_TEST(test_pathA_rx);
    RUN_TEST(test_pathA_tx);
    RUN_TEST(test_pathA_survives_ansi_colour);
    RUN_TEST(test_garbage_is_ignored);
    RUN_TEST(test_partial_line_yields_nothing_until_newline);
    RUN_TEST(test_crlf_does_not_emit_a_blank_line);
    RUN_TEST(test_text_truncated_to_MSG_MAX_LEN);
    RUN_TEST(test_text_at_exactly_MSG_MAX_LEN);
    RUN_TEST(test_buffer_overrun_discards_the_line);
    RUN_TEST(test_parser_recovers_after_overrun);
    return UNITY_END();
}
