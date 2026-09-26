#pragma once
//
// The ESP32 <-> nRF52840 link: line assembly, parsing, and a TX queue.
//
// LoraLinkParser contains NO Arduino calls, so `pio test -e native` exercises
// every branch of it on a PC. Only the LoraLink wrapper at the bottom - the
// part that owns a HardwareSerial - is device-only, and it is guarded by
// #ifdef ARDUINO so the native build never sees it. See PLAN.md section 4.
//
// ── Two wire formats, by design ─────────────────────────────────────────────
// Path B (preferred, PLAN.md section 3.2) is a machine-readable line the nRF
// emits with printk() alongside its human log:
//
//     +RX,<src>,<seq>,<rssi>,<snr>,<text>
//     +TX,<seq>,<text>
//
// Path A is the existing Zephyr LOG_INF line, parsed as a fallback so the
// project works against unmodified nRF firmware:
//
//     [00:00:12.345,000] <inf> lora_mesh: RX from node 2 seq 5 ttl 3
//         (RSSI -80 dBm, SNR 9 dB): sos          <- one line on the wire
//     [00:00:12.345,000] <inf> lora_mesh: TX own seq 4: hello 4
//
// Path B is tried first. Anything that matches neither is human log noise and
// is silently ignored - which is the whole point of the '+' prefix.
//
// The current nRF firmware emits BOTH for every event, back to back (it runs
// CONFIG_LOG_MODE_IMMEDIATE, so the log line lands first and its Path B twin
// right after). The parser drops an event identical to the one before it, so
// each radio event reaches the application exactly once.
//
// The other direction is one format only, NRF_SEND_PREFIX then the text:
//
//     +SEND,<text>
//
// The nRF drops any line without the prefix (app_config.h has the reason).

#include <stdint.h>

#include "app_config.h"

enum class LinkEventType : uint8_t {
    None = 0,
    Rx,          // a message arrived over the air
    TxConfirm,   // the nRF confirms it put one of ours on air
};

struct LinkEvent {
    LinkEventType type = LinkEventType::None;
    char     text[MSG_MAX_LEN + 1] = {0};
    uint16_t seq  = 0;
    int16_t  rssi = 0;
    uint8_t  src  = 0;
    int8_t   snr  = 0;
};

// True for a node's periodic beacon, "hello <seq>" (reference/nrf.cpp:483) -
// a peer's (Rx) or our own node's (TxConfirm). The number must equal the
// event's own seq, because the nRF stamps both from the same counter; that is
// what keeps a real message such as "hello team" or "hello 5" from being
// swallowed as housekeeping.
bool isBeacon(const LinkEvent &ev);

class LoraLinkParser {
public:
    // Feed one received byte. Returns true exactly when `out` has been filled
    // with a complete event. Bytes that do not complete a line return false.
    bool feed(char c, LinkEvent &out);

    // Drop any partially-assembled line.
    void reset();

    // Diagnostics for the Status screen and Stage 4.
    uint32_t linesSeen() const { return lines_; }      // complete lines
    uint32_t eventsParsed() const { return events_; }  // of which understood
    uint32_t overruns() const { return overruns_; }    // lines too long

private:
    bool parseLine(LinkEvent &out);
    bool parsePathB(const char *s, LinkEvent &out);
    bool parsePathA(const char *s, LinkEvent &out);

    char     line_[NRF_LINE_MAX + 1] = {0};
    uint16_t len_      = 0;
    bool     overrun_  = false;   // this line already blew the buffer
    bool     inEscape_ = false;   // mid ANSI escape sequence, dropping bytes
    LinkEvent last_;              // previous event, to drop its Path A/B twin
    uint32_t lines_    = 0;
    uint32_t events_   = 0;
    uint32_t overruns_ = 0;
};

#ifdef ARDUINO

#include <Arduino.h>

// Device-side wrapper: owns UART2, drains it, and holds a bounded TX queue.
//
// The queue is what makes backpressure visible instead of silent. A LoRa node
// at SF10 needs a noticeable fraction of a second per packet, so a user
// mashing presets can outrun the radio; send() returning false is the UI's
// cue to say so rather than pretend the message went.
class LoraLink {
public:
    using EventFn = void (*)(const LinkEvent &ev, void *user);

    void begin();
    void setEventHandler(EventFn fn, void *user) { fn_ = fn; user_ = user; }

    // Drain the UART and dispatch any parsed events. Call from loop().
    void poll(uint32_t nowMs);

    // Queue one line for transmission. False = queue full, nothing queued.
    // Text longer than MSG_MAX_LEN is rejected rather than truncated: the
    // radio would silently drop the tail, and a message that says something
    // other than what the sender typed is worse than a refused send.
    bool send(const char *text);

    // True while lines are still arriving. The nRF beacons every 10 s, so
    // silence past NRF_LINK_TIMEOUT_MS (~3 missed beacons) is a real fault.
    bool linkUp(uint32_t nowMs) const;
    uint32_t lastLineMillis() const { return lastLine_; }

    uint8_t queueDepth() const { return qCount_; }
    uint8_t queueCapacity() const { return NRF_TXQ_LEN; }

    const LoraLinkParser &parser() const { return parser_; }

private:
    void pumpQueue(uint32_t nowMs);

    LoraLinkParser parser_;
    EventFn  fn_   = nullptr;
    void    *user_ = nullptr;

    char    queue_[NRF_TXQ_LEN][MSG_MAX_LEN + 1] = {};
    uint8_t qHead_ = 0, qTail_ = 0, qCount_ = 0;

    uint32_t lastLine_ = 0;
    uint32_t lastTx_   = 0;
    bool     everSeen_ = false;
};

#endif  // ARDUINO
