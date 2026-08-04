#pragma once
//
// Fixed-capacity history of messages, newest-first.
//
// Contains NO Arduino calls on purpose. Time arrives as a millisecond count
// passed in by the caller, so the whole thing runs under `pio test -e native`
// on a PC in milliseconds. See PLAN.md section 4.
//
// Capacity is MSG_HISTORY_LEN and the buffer never grows: once full, adding
// overwrites the oldest. That is the correct behaviour for a field device -
// running out of RAM mid-emergency because someone chatted for an hour is a
// worse failure than losing hour-old traffic.

#include <stdint.h>

#include "app_config.h"

// One stored message. Fixed-size text rather than a pointer so the whole
// store is one flat allocation with no heap traffic per message.
struct Message {
    char     text[MSG_MAX_LEN + 1];  // always NUL-terminated
    uint32_t rxMillis;               // caller's millisecond clock at arrival
    uint16_t seq;
    int16_t  rssi;
    uint8_t  src;                    // originating node id
    int8_t   snr;
    bool     unread;
    bool     outgoing;               // true = we sent it (a +TX confirmation)
};

class MessageStore {
public:
    // Append a message. Truncates text to MSG_MAX_LEN and always
    // NUL-terminates. Overwrites the oldest entry when full.
    void add(const Message &m);

    // Convenience for the common receive path.
    void addReceived(uint8_t src, uint16_t seq, int16_t rssi, int8_t snr,
                     const char *text, uint32_t nowMs);

    // Convenience for a confirmed transmission (+TX from the nRF).
    void addSent(uint16_t seq, const char *text, uint32_t nowMs);

    // 0 is the NEWEST message; count()-1 the oldest. The UI lists newest
    // first, so making that index 0 keeps the list code free of arithmetic
    // that has to be re-derived (and re-checked) at every call site.
    const Message *at(uint8_t i) const;

    uint8_t count() const { return count_; }
    uint8_t capacity() const { return MSG_HISTORY_LEN; }
    uint8_t unreadCount() const;

    void markRead(uint8_t i);
    void markAllRead();
    void clear();

    // True if the text looks like an emergency, i.e. begins with "SOS".
    // Case-insensitive, because the sender may have typed either. Static so
    // the SOS screen can test an incoming string before it is ever stored.
    static bool isSos(const char *text);

private:
    Message buf_[MSG_HISTORY_LEN]{};
    uint8_t count_ = 0;   // how many slots are live, saturating at capacity
    uint8_t head_  = 0;   // index of the next slot to write
};
