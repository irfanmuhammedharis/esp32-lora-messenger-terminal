// ─────────────────────────────────────────────────────────────────────────────
// Stage 4x — UART link diagnostic.   pio run -e t4x_linkdiag -t upload
//
// A self-scoring battery over the ESP32 <-> nRF52840 link, run with the same
// expectations as Stage 4's exit criteria (PLAN.md section 5):
//
//   [1] loopback probes     4 known lines through a TX2->RX2 jumper, each
//                           parsed (or correctly ignored) by the real UART
//   [2] round-trip pings    100 numbered pings through the jumper, answered
//                           in order with zero loss
//   [3] framing             zero parser overruns across the whole battery -
//                           the UART has no parity/CRC, so counters are the
//                           only integrity witness available (risk R11)
//   [4] 32-char boundary    an exact-32-char payload round-trips intact;
//                           a 33-char send() is refused, not truncated
//   [5] live RX             with the jumper off and the nRF wired, a genuine
//                           +RX/log line parses within 30 s
//
// The first four need the jumper (GPIO32 -> GPIO33). The fifth needs it
// removed and the real node attached (WIRING.md section 6). A 5/5 verdict is
// the Stage 4 sign-off; rerun this app whenever the link misbehaves.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "LoraLink.h"
#include "app_config.h"
#include "pins.h"

static TFT_eSPI  tft;
static LoraLink  nrfLink;

// Battery results. `volatile` is unnecessary (single-threaded), but the
// handler runs inside poll() so the state is shared - keep it plain.
static uint32_t rxEvents = 0, txEvents = 0;
static uint16_t lastRxSeq = 0;
static uint8_t  lastRxSrc = 0;
static bool     gotRxSince = false;
static char     lastRxText[MSG_MAX_LEN + 1] = {0};

static bool c1 = false, c2 = false, c3 = false, c4 = false, c5 = false;

static void onLinkEvent(const LinkEvent &ev, void *) {
    if (ev.type == LinkEventType::Rx) {
        rxEvents++;
        gotRxSince = true;
        lastRxSeq = ev.seq;
        lastRxSrc = ev.src;
        strncpy(lastRxText, ev.text, MSG_MAX_LEN);
        lastRxText[MSG_MAX_LEN] = '\0';
        Serial.printf("    RX src=%u seq=%u \"%s\"\n", ev.src, ev.seq, ev.text);
    } else if (ev.type == LinkEventType::TxConfirm) {
        txEvents++;
        Serial.printf("    TX-confirm seq=%u \"%s\"\n", ev.seq, ev.text);
    }
}

// Send one raw line straight down the wire and wait up to `timeoutMs` for the
// handler to observe an Rx event. Returns true if one arrived in time.
static bool sendLineAwaitRx(const char *line, uint32_t timeoutMs) {
    const uint32_t before = rxEvents;
    Serial2.print(line);
    Serial2.flush();

    const uint32_t deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        nrfLink.poll(millis());
        if (rxEvents != before) return true;
        delay(2);
    }
    return false;
}

// ── Battery ─────────────────────────────────────────────────────────────────

static void runBattery() {
    Serial.println();
    Serial.println("[BATTERY] Fit the jumper: GPIO32 -> GPIO33.");
    Serial.println("          Then anything that fails below is real.");
    delay(1500);

    // ── [1] loopback probes ────────────────────────────────────────────────
    struct Probe { const char *line; const char *expect; };
    static const Probe probes[] = {
        {"+RX,2,5,-80,9,SOS\n",                                "SOS"},
        {"+TX,4,ALL CLEAR\n",                                  nullptr},  // TxConfirm, not Rx
        {"[00:00:12.345,000] <inf> lora_mesh: RX from node 7 "
         "seq 1 ttl 3 (RSSI -55 dBm, SNR 7 dB): IM HERE\n",   "IM HERE"},
        {"this is log noise and must be ignored\n",            nullptr},
    };

    uint8_t ok = 0;
    for (const Probe &p : probes) {
        const uint32_t beforeRx = rxEvents, beforeTx = txEvents;
        Serial2.print(p.line);
        Serial2.flush();
        const uint32_t deadline = millis() + 400;
        while (millis() < deadline) {
            nrfLink.poll(millis());
            if (rxEvents != beforeRx || txEvents != beforeTx) break;
            delay(2);
        }

        if (p.expect) {
            const bool good = (rxEvents != beforeRx) &&
                              strcmp(lastRxText, p.expect) == 0;
            if (good) ok++;
            Serial.printf("  [1] %-38.38s %s\n", p.line, good ? "parsed" : "FAIL");
        } else {
            // +TX line must produce a TxConfirm, the noise line nothing at all.
            const bool txLine = strncmp(p.line, "+TX,", 4) == 0;
            const bool good = txLine ? (txEvents != beforeTx)
                                     : (rxEvents == beforeRx && txEvents == beforeTx);
            if (good) ok++;
            Serial.printf("  [1] %-38.38s %s\n", p.line, good ? "handled" : "FAIL");
        }
    }
    c1 = (ok == 4);
    Serial.printf("  [1] loopback probes ....... %s (%u/4)\n", c1 ? "PASS" : "FAIL", ok);

    // ── [2] 100-ping round trip ────────────────────────────────────────────
    uint16_t lost = 0;
    for (uint16_t n = 0; n < 100; n++) {
        char line[40];
        snprintf(line, sizeof(line), "+RX,9,%u,-50,5,PING\n", n);

        const uint32_t before = rxEvents;
        Serial2.print(line);
        Serial2.flush();

        const uint32_t deadline = millis() + 400;
        while (millis() < deadline) {
            nrfLink.poll(millis());
            if (rxEvents != before) break;
            delay(1);
        }
        if (rxEvents == before || lastRxSeq != n || strcmp(lastRxText, "PING") != 0) {
            if (!lost) Serial.printf("  [2] first loss at ping %u\n", n);
            lost++;
        }
    }
    c2 = (lost == 0);
    Serial.printf("  [2] 100-ping round trip ... %s (%u/100 lost)\n",
                  c2 ? "PASS" : "FAIL", lost);

    // ── [3] framing (parser overruns across the whole battery) ─────────────
    const uint32_t overruns = nrfLink.parser().overruns();
    c3 = (overruns == 0);
    Serial.printf("  [3] framing errors ........ %s (%lu overruns)\n",
                  c3 ? "PASS" : "FAIL", (unsigned long)overruns);

    // ── [4] 32-char boundary ───────────────────────────────────────────────
    // The radio drops characters past 32 silently (reference/nrf.cpp:32);
    // send() refuses them loudly instead. Both halves are checked: a full
    // 32-char payload survives the wire intact, a 33-char one is rejected.
    static const char payload32[MSG_MAX_LEN + 1] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    char line[48];
    snprintf(line, sizeof(line), "+RX,9,0,-50,5,%s\n", payload32);
    gotRxSince = false;
    lastRxText[0] = '\0';
    const bool arrived = sendLineAwaitRx(line, 500);

    const char tooLong[MSG_MAX_LEN + 2] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456";
    const bool refused = !nrfLink.send(tooLong);
    const bool accepted = nrfLink.send(payload32);
    // Drain the accepted one so it cannot pollute later counters.
    const uint32_t drainDeadline = millis() + 3000;
    while (millis() < drainDeadline) {
        nrfLink.poll(millis());
        if (nrfLink.queueDepth() == 0) break;
        delay(2);
    }

    c4 = arrived && strcmp(lastRxText, payload32) == 0 && refused && accepted;
    Serial.printf("  [4] 32-char boundary ...... %s (round-trip %s, 33 refused %s)\n",
                  c4 ? "PASS" : "FAIL", arrived ? "ok" : "FAIL",
                  refused ? "ok" : "FAIL");

    // Battery traffic is done; anything parsed from here on is live.
    gotRxSince = false;

    Serial.println("-----------------------------------------------------");
    Serial.printf("  BATTERY %u/5   [1]%s [2]%s [3]%s [4]%s [5] live below\n",
                  (unsigned)(c1 + c2 + c3 + c4), c1 ? "P" : "F",
                  c2 ? "P" : "F", c3 ? "P" : "F", c4 ? "P" : "F");
    Serial.println();
    Serial.println("[LIVE] Remove the jumper, wire the nRF (WIRING.md 6).");
    Serial.println("       A parsed line from the node within 30 s = [5].");
}

static void drawStatus() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("STAGE 4x - LINK DIAG", tft.width() / 2, 6, 2);
    tft.setTextDatum(TL_DATUM);

    char buf[40];
    snprintf(buf, sizeof(buf), "battery %u/5", (unsigned)(c1 + c2 + c3 + c4 + c5));
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(buf, 6, 36, 2);

    tft.setTextColor(c1 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(c1 ? "[1] probes    PASS" : "[1] probes    FAIL", 6, 62, 2);
    tft.setTextColor(c2 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(c2 ? "[2] pings     PASS" : "[2] pings     FAIL", 6, 82, 2);
    tft.setTextColor(c3 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(c3 ? "[3] framing   PASS" : "[3] framing   FAIL", 6, 102, 2);
    tft.setTextColor(c4 ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(c4 ? "[4] boundary  PASS" : "[4] boundary  FAIL", 6, 122, 2);
    tft.setTextColor(c5 ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    tft.drawString(c5 ? "[5] live RX   PASS" : "[5] live RX   waiting", 6, 142, 2);

    snprintf(buf, sizeof(buf), "rx %lu  tx %lu  ovr %lu",
             (unsigned long)rxEvents, (unsigned long)txEvents,
             (unsigned long)nrfLink.parser().overruns());
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(buf, 6, 180, 2);
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 4x - UART link diagnostic (self-scoring)");
    Serial.println("=====================================================");
    Serial.printf("  TX2 = GPIO%d -> nRF RX     RX2 = GPIO%d <- nRF TX\n",
                  PIN_NRF_TX, PIN_NRF_RX);

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);

    nrfLink.begin();
    nrfLink.setEventHandler(onLinkEvent, nullptr);

    runBattery();
    drawStatus();
}

void loop() {
    static uint32_t liveStart = 0;
    static uint32_t lastReport = 0;
    static bool     announced = false;

    const uint32_t now = millis();
    nrfLink.poll(now);

    // ── [5] live RX: a genuine line from the node. Our own battery traffic
    // used src 9, so only a line from another node counts.
    if (!c5 && gotRxSince && lastRxSrc != 9 && liveStart) {
        c5 = true;
        Serial.printf("  [5] live RX .............. PASS (\"%s\", seq %u)\n",
                      lastRxText, lastRxSeq);
        Serial.println("-----------------------------------------------------");
        Serial.printf("  FINAL VERDICT %u/5\n",
                      (unsigned)(c1 + c2 + c3 + c4 + c5));
        drawStatus();
    }

    if (now - lastReport >= 5000) {
        lastReport = now;
        if (!announced) {
            announced = true;
            liveStart = now;
        }
        const LoraLinkParser &p = nrfLink.parser();
        Serial.printf("  link %s | lines %lu events %lu overruns %lu | rx %lu tx %lu\n",
                      nrfLink.linkUp(now) ? "UP  " : "DOWN",
                      (unsigned long)p.linesSeen(),
                      (unsigned long)p.eventsParsed(),
                      (unsigned long)p.overruns(),
                      (unsigned long)rxEvents, (unsigned long)txEvents);
        drawStatus();
    }
}
