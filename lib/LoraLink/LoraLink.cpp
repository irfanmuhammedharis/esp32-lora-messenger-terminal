#include "LoraLink.h"

#include <string.h>

// ── Small parsing helpers ───────────────────────────────────────────────────
// Hand-rolled rather than sscanf: sscanf drags in a large, locale-aware
// formatter, and on a partly-corrupted line its failure modes are far harder
// to reason about than an explicit character walk.

static void skipSpaces(const char *&p) {
    while (*p == ' ' || *p == '\t') p++;
}

// Parse a decimal integer, optionally signed. Advances p past it.
// Returns false (leaving p where it was) if there is no digit to read.
static bool parseInt(const char *&p, long &out) {
    skipSpaces(p);
    const char *start = p;

    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') { p++; }

    if (*p < '0' || *p > '9') {
        p = start;
        return false;
    }

    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        if (v > 1000000L) break;   // nothing on this link is ever this big
    }

    out = neg ? -v : v;
    return true;
}

// Expect a literal, and advance past it if present.
static bool eat(const char *&p, const char *lit) {
    const size_t n = strlen(lit);
    if (strncmp(p, lit, n) != 0) return false;
    p += n;
    return true;
}

static void copyCapped(char *dst, const char *src) {
    size_t i = 0;
    for (; i < MSG_MAX_LEN && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// ── Line assembly ───────────────────────────────────────────────────────────

void LoraLinkParser::reset() {
    len_ = 0;
    overrun_ = false;
    line_[0] = '\0';
}

bool LoraLinkParser::feed(char c, LinkEvent &out) {
    // Treat CR and LF both as terminators, and collapse the CRLF pair by
    // ignoring the empty line it would otherwise produce.
    if (c == '\n' || c == '\r') {
        if (len_ == 0 && !overrun_) return false;   // blank line / CRLF tail

        const bool wasOverrun = overrun_;
        line_[len_] = '\0';
        lines_++;
        len_ = 0;
        overrun_ = false;

        // A line that blew the buffer is discarded whole. Parsing a truncated
        // prefix would invent a message that was never sent, which on this
        // device is a worse outcome than dropping one.
        if (wasOverrun) {
            overruns_++;
            return false;
        }

        if (parseLine(out)) {
            events_++;
            return true;
        }
        return false;
    }

    // Silently drop ANSI escape sequences. PLAN.md section 3.2 lists log
    // colour as one of the things that breaks Path A; CONFIG_LOG_BACKEND_
    // SHOW_COLOR=n is the real fix, but surviving it costs three lines.
    if (c == 0x1B) { inEscape_ = true; return false; }
    if (inEscape_) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) inEscape_ = false;
        return false;
    }

    if (overrun_) return false;   // already lost; wait for the newline

    if (len_ >= NRF_LINE_MAX) {
        overrun_ = true;
        return false;
    }

    line_[len_++] = c;
    return false;
}

bool LoraLinkParser::parseLine(LinkEvent &out) {
    const char *p = line_;
    skipSpaces(p);

    // Path B first: it is unambiguous and cheap to reject.
    if (*p == '+') {
        return parsePathB(p + 1, out);
    }
    return parsePathA(p, out);
}

// ── Path B:  +RX,<src>,<seq>,<rssi>,<snr>,<text>   /   +TX,<seq>,<text> ─────
bool LoraLinkParser::parsePathB(const char *p, LinkEvent &out) {
    if (eat(p, "RX,")) {
        long src, seq, rssi, snr;
        if (!parseInt(p, src) || !eat(p, ",")) return false;
        if (!parseInt(p, seq) || !eat(p, ",")) return false;
        if (!parseInt(p, rssi) || !eat(p, ",")) return false;
        if (!parseInt(p, snr) || !eat(p, ",")) return false;

        // Everything remaining is the payload, commas included - the text is
        // the last field precisely so it never needs escaping.
        out = LinkEvent{};
        out.type = LinkEventType::Rx;
        out.src  = static_cast<uint8_t>(src);
        out.seq  = static_cast<uint16_t>(seq);
        out.rssi = static_cast<int16_t>(rssi);
        out.snr  = static_cast<int8_t>(snr);
        copyCapped(out.text, p);
        return true;
    }

    if (eat(p, "TX,")) {
        long seq;
        if (!parseInt(p, seq) || !eat(p, ",")) return false;

        out = LinkEvent{};
        out.type = LinkEventType::TxConfirm;
        out.seq  = static_cast<uint16_t>(seq);
        copyCapped(out.text, p);
        return true;
    }

    return false;
}

// ── Path A: the Zephyr log lines from reference/nrf.cpp ──────────────────────────
//   ... lora_mesh: RX from node 2 seq 5 ttl 3 (RSSI -80 dBm, SNR 9 dB): sos
//   ... lora_mesh: TX own seq 4: hello 4
bool LoraLinkParser::parsePathA(const char *line, LinkEvent &out) {
    const char *p = strstr(line, "RX from node ");
    if (p) {
        p += strlen("RX from node ");

        long src, seq, ttl, rssi, snr;
        if (!parseInt(p, src)) return false;
        if (!eat(p, " seq ")   || !parseInt(p, seq))  return false;
        if (!eat(p, " ttl ")   || !parseInt(p, ttl))  return false;
        if (!eat(p, " (RSSI ") || !parseInt(p, rssi)) return false;
        if (!eat(p, " dBm, SNR ") || !parseInt(p, snr)) return false;
        if (!eat(p, " dB): ")) return false;

        out = LinkEvent{};
        out.type = LinkEventType::Rx;
        out.src  = static_cast<uint8_t>(src);
        out.seq  = static_cast<uint16_t>(seq);
        out.rssi = static_cast<int16_t>(rssi);
        out.snr  = static_cast<int8_t>(snr);
        copyCapped(out.text, p);
        return true;
    }

    p = strstr(line, "TX own seq ");
    if (p) {
        p += strlen("TX own seq ");
        long seq;
        if (!parseInt(p, seq)) return false;
        if (!eat(p, ": ")) return false;

        out = LinkEvent{};
        out.type = LinkEventType::TxConfirm;
        out.seq  = static_cast<uint16_t>(seq);
        copyCapped(out.text, p);
        return true;
    }

    return false;
}

// ── Device-side wrapper ─────────────────────────────────────────────────────
#ifdef ARDUINO

#include "pins.h"

void LoraLink::begin() {
    Serial2.begin(NRF_BAUD, SERIAL_8N1, PIN_NRF_RX, PIN_NRF_TX);
    parser_.reset();
    lastLine_ = millis();
    everSeen_ = false;
}

void LoraLink::poll(uint32_t nowMs) {
    // Bound the work per call. A flood of log lines must not let the UART
    // starve lv_timer_handler(), or the UI freezes exactly when the radio is
    // busiest - which is when the operator most needs to see it.
    int budget = 256;
    while (Serial2.available() > 0 && budget-- > 0) {
        const char c = static_cast<char>(Serial2.read());

        LinkEvent ev;
        if (parser_.feed(c, ev)) {
            if (fn_) fn_(ev, user_);
        }

        // Any complete line - parsed or not - proves the nRF is alive.
        if (c == '\n' || c == '\r') {
            lastLine_ = nowMs;
            everSeen_ = true;
        }
    }

    pumpQueue(nowMs);
}

bool LoraLink::send(const char *text) {
    if (!text || !text[0]) return false;
    if (strlen(text) > MSG_MAX_LEN) return false;
    if (qCount_ >= NRF_TXQ_LEN) return false;

    copyCapped(queue_[qTail_], text);
    qTail_ = static_cast<uint8_t>((qTail_ + 1) % NRF_TXQ_LEN);
    qCount_++;
    return true;
}

void LoraLink::pumpQueue(uint32_t nowMs) {
    if (qCount_ == 0) return;
    if (nowMs - lastTx_ < NRF_TX_GAP_MS) return;

    // The nRF's serial_poll() originates on '\n', so the newline IS the
    // send command - nothing else is needed to frame it.
    Serial2.print(queue_[qHead_]);
    Serial2.print('\n');

    qHead_ = static_cast<uint8_t>((qHead_ + 1) % NRF_TXQ_LEN);
    qCount_--;
    lastTx_ = nowMs;
}

bool LoraLink::linkUp(uint32_t nowMs) const {
    if (!everSeen_) return false;
    return (nowMs - lastLine_) < NRF_LINK_TIMEOUT_MS;
}

#endif  // ARDUINO
