#include "MessageStore.h"

#include <string.h>

// Copy with a hard cap and a guaranteed terminator.
//
// strncpy is deliberately avoided: it does NOT terminate when the source is
// exactly as long as the destination, which is precisely the 32-character
// boundary case this project hits constantly (MSG_MAX_LEN is the radio's cap,
// so full-length messages are normal, not rare).
static void copyText(char *dst, const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i < MSG_MAX_LEN && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void MessageStore::add(const Message &m) {
    Message &slot = buf_[head_];
    slot = m;
    copyText(slot.text, m.text);

    head_ = static_cast<uint8_t>((head_ + 1) % MSG_HISTORY_LEN);
    if (count_ < MSG_HISTORY_LEN) count_++;
}

void MessageStore::addReceived(uint8_t src, uint16_t seq, int16_t rssi,
                               int8_t snr, const char *text, uint32_t nowMs) {
    Message m{};
    copyText(m.text, text);
    m.rxMillis = nowMs;
    m.seq      = seq;
    m.rssi     = rssi;
    m.src      = src;
    m.snr      = snr;
    m.unread   = true;
    m.outgoing = false;
    add(m);
}

void MessageStore::addSent(uint16_t seq, const char *text, uint32_t nowMs) {
    Message m{};
    copyText(m.text, text);
    m.rxMillis = nowMs;
    m.seq      = seq;
    m.unread   = false;   // we sent it; there is nothing to catch up on
    m.outgoing = true;
    add(m);
}

const Message *MessageStore::at(uint8_t i) const {
    if (i >= count_) return nullptr;

    // head_ points at the next slot to WRITE, so head_-1 is the newest.
    // Adding MSG_HISTORY_LEN before the modulo keeps the arithmetic positive
    // on unsigned types, where head_ - 1 - i would otherwise wrap enormous.
    const uint8_t idx =
        static_cast<uint8_t>((head_ + MSG_HISTORY_LEN - 1 - i) % MSG_HISTORY_LEN);
    return &buf_[idx];
}

uint8_t MessageStore::unreadCount() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < count_; i++) {
        if (at(i)->unread) n++;
    }
    return n;
}

void MessageStore::markRead(uint8_t i) {
    if (i >= count_) return;
    const uint8_t idx =
        static_cast<uint8_t>((head_ + MSG_HISTORY_LEN - 1 - i) % MSG_HISTORY_LEN);
    buf_[idx].unread = false;
}

void MessageStore::markAllRead() {
    for (uint8_t i = 0; i < MSG_HISTORY_LEN; i++) buf_[i].unread = false;
}

void MessageStore::clear() {
    count_ = 0;
    head_  = 0;
}

bool MessageStore::isSos(const char *text) {
    if (!text) return false;
    // Compare without <ctype.h>: tolower() is locale-dependent and this has to
    // behave identically on the host test build and on the device.
    for (uint8_t i = 0; i < 3; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != "SOS"[i]) return false;
    }
    return true;
}
