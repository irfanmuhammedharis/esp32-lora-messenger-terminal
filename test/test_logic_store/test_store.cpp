// Stage 6 — MessageStore, on the host.   pio test -e native

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "MessageStore.h"

static MessageStore s;

void setUp(void) { s = MessageStore{}; }
void tearDown(void) {}

static void test_empty(void) {
    TEST_ASSERT_EQUAL_UINT8(0, s.count());
    TEST_ASSERT_EQUAL_UINT8(0, s.unreadCount());
    TEST_ASSERT_NULL(s.at(0));
}

static void test_add_and_read_back(void) {
    s.addReceived(2, 5, -80, 9, "SOS", 1000);
    TEST_ASSERT_EQUAL_UINT8(1, s.count());

    const Message *m = s.at(0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING("SOS", m->text);
    TEST_ASSERT_EQUAL_UINT8(2, m->src);
    TEST_ASSERT_EQUAL_UINT16(5, m->seq);
    TEST_ASSERT_EQUAL_INT16(-80, m->rssi);
    TEST_ASSERT_EQUAL_INT8(9, m->snr);
    TEST_ASSERT_EQUAL_UINT32(1000, m->rxMillis);
    TEST_ASSERT_TRUE(m->unread);
    TEST_ASSERT_FALSE(m->outgoing);
}

// index 0 is the NEWEST - the inbox lists newest first, and putting that at
// index 0 keeps the list code free of arithmetic.
static void test_index_zero_is_newest(void) {
    s.addReceived(1, 1, -70, 8, "first",  100);
    s.addReceived(2, 2, -70, 8, "second", 200);
    s.addReceived(3, 3, -70, 8, "third",  300);

    TEST_ASSERT_EQUAL_UINT8(3, s.count());
    TEST_ASSERT_EQUAL_STRING("third",  s.at(0)->text);
    TEST_ASSERT_EQUAL_STRING("second", s.at(1)->text);
    TEST_ASSERT_EQUAL_STRING("first",  s.at(2)->text);
    TEST_ASSERT_NULL(s.at(3));
}

static void test_ring_overwrites_oldest_when_full(void) {
    char buf[16];
    for (int i = 0; i < MSG_HISTORY_LEN + 5; i++) {
        snprintf(buf, sizeof(buf), "m%d", i);
        s.addReceived(1, (uint16_t)i, -70, 8, buf, (uint32_t)i);
    }

    TEST_ASSERT_EQUAL_UINT8(MSG_HISTORY_LEN, s.count());

    // Newest is the last one added...
    snprintf(buf, sizeof(buf), "m%d", MSG_HISTORY_LEN + 4);
    TEST_ASSERT_EQUAL_STRING(buf, s.at(0)->text);

    // ...and the oldest survivor is exactly MSG_HISTORY_LEN-1 behind it.
    snprintf(buf, sizeof(buf), "m%d", 5);
    TEST_ASSERT_EQUAL_STRING(buf, s.at(MSG_HISTORY_LEN - 1)->text);
    TEST_ASSERT_NULL(s.at(MSG_HISTORY_LEN));
}

static void test_text_truncated_at_MSG_MAX_LEN(void) {
    s.addReceived(1, 1, -70, 8,
                  "0123456789012345678901234567890123456789", 0);
    TEST_ASSERT_EQUAL_UINT32(MSG_MAX_LEN, strlen(s.at(0)->text));
}

// strncpy would leave this one unterminated; copyText must not.
static void test_text_at_exactly_MSG_MAX_LEN_is_terminated(void) {
    s.addReceived(1, 1, -70, 8, "01234567890123456789012345678901", 0);
    const Message *m = s.at(0);
    TEST_ASSERT_EQUAL_UINT32(MSG_MAX_LEN, strlen(m->text));
    TEST_ASSERT_EQUAL_CHAR('\0', m->text[MSG_MAX_LEN]);
}

static void test_unread_tracking(void) {
    s.addReceived(1, 1, -70, 8, "a", 0);
    s.addReceived(1, 2, -70, 8, "b", 0);
    s.addReceived(1, 3, -70, 8, "c", 0);
    TEST_ASSERT_EQUAL_UINT8(3, s.unreadCount());

    s.markRead(0);
    TEST_ASSERT_EQUAL_UINT8(2, s.unreadCount());
    TEST_ASSERT_FALSE(s.at(0)->unread);
    TEST_ASSERT_TRUE(s.at(1)->unread);

    s.markAllRead();
    TEST_ASSERT_EQUAL_UINT8(0, s.unreadCount());
}

// Our own confirmed transmissions are not "unread" - there is nothing to
// catch up on for a message we sent ourselves.
static void test_sent_messages_are_not_unread(void) {
    s.addSent(7, "ALL CLEAR", 500);
    const Message *m = s.at(0);
    TEST_ASSERT_TRUE(m->outgoing);
    TEST_ASSERT_FALSE(m->unread);
    TEST_ASSERT_EQUAL_UINT16(7, m->seq);
    TEST_ASSERT_EQUAL_UINT8(0, s.unreadCount());
}

static void test_clear(void) {
    s.addReceived(1, 1, -70, 8, "a", 0);
    s.clear();
    TEST_ASSERT_EQUAL_UINT8(0, s.count());
    TEST_ASSERT_NULL(s.at(0));
}

// An incoming SOS takes over the screen, so this predicate gates a
// full-screen interrupt and has to be exactly right.
static void test_isSos(void) {
    TEST_ASSERT_TRUE(MessageStore::isSos("SOS"));
    TEST_ASSERT_TRUE(MessageStore::isSos("sos"));
    TEST_ASSERT_TRUE(MessageStore::isSos("SoS"));
    TEST_ASSERT_TRUE(MessageStore::isSos("SOS NEED MEDIC"));

    TEST_ASSERT_FALSE(MessageStore::isSos("NO SOS HERE"));
    TEST_ASSERT_FALSE(MessageStore::isSos("SO"));
    TEST_ASSERT_FALSE(MessageStore::isSos("S"));
    TEST_ASSERT_FALSE(MessageStore::isSos(""));
    TEST_ASSERT_FALSE(MessageStore::isSos(nullptr));
}

// Every preset must fit the radio's cap, or it is silently cut on air.
// PLAN.md risk R2.
static void test_all_presets_fit(void) {
    for (uint8_t i = 0; i < kPresetCount; i++) {
        TEST_ASSERT_TRUE_MESSAGE(strlen(kPresetMessages[i]) <= MSG_MAX_LEN,
                                 kPresetMessages[i]);
    }
    TEST_ASSERT_TRUE(MessageStore::isSos(kPresetMessages[PRESET_SOS_INDEX]));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_add_and_read_back);
    RUN_TEST(test_index_zero_is_newest);
    RUN_TEST(test_ring_overwrites_oldest_when_full);
    RUN_TEST(test_text_truncated_at_MSG_MAX_LEN);
    RUN_TEST(test_text_at_exactly_MSG_MAX_LEN_is_terminated);
    RUN_TEST(test_unread_tracking);
    RUN_TEST(test_sent_messages_are_not_unread);
    RUN_TEST(test_clear);
    RUN_TEST(test_isSos);
    RUN_TEST(test_all_presets_fit);
    return UNITY_END();
}
