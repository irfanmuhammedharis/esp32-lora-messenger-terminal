// ─────────────────────────────────────────────────────────────────────────────
// Stage 4 — UART link to the nRF52840.   pio run -e t4_uart -t upload
//
// Two phases:
//
//   A  LOOPBACK   jumper GPIO32 -> GPIO33 (TX2 to RX2) and this proves the
//                 UART, the pin mapping and the whole parser end to end with
//                 no nRF attached at all. If this fails, nothing about the
//                 radio node is worth investigating yet.
//   B  LIVE       remove the jumper, wire the nRF (WIRING.md section 5), and
//                 every line it sends is parsed and reported.
//
// Exit criteria (PLAN.md stage 4):
//   - loopback passes
//   - a typed line goes on air
//   - +RX / log lines parse
//   - no framing errors at 115200 over 10 minutes
//
// ⚠ The single biggest integration risk (R1): the nRF's Zephyr console must
// be on a HARDWARE UART, not USB CDC. uart_dev is DT_CHOSEN(zephyr_console)
// (reference/nrf.cpp:75) and the ESP32 cannot act as a USB host. See PLAN.md 3.3.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "LoraLink.h"
#include "MessageStore.h"
#include "app_config.h"
#include "pins.h"

static TFT_eSPI      tft;
static LoraLink      nrfLink;
static MessageStore  store;

static uint32_t rxEvents = 0, txEvents = 0;

static void onLinkEvent(const LinkEvent &ev, void *) {
    const uint32_t now = millis();
    if (ev.type == LinkEventType::Rx) {
        rxEvents++;
        store.addReceived(ev.src, ev.seq, ev.rssi, ev.snr, ev.text, now);
        Serial.printf("  RX  node %u seq %u  %d dBm / %d dB  \"%s\"\n",
                      ev.src, ev.seq, ev.rssi, ev.snr, ev.text);
    } else if (ev.type == LinkEventType::TxConfirm) {
        txEvents++;
        store.addSent(ev.seq, ev.text, now);
        Serial.printf("  TX  confirmed seq %u  \"%s\"\n", ev.seq, ev.text);
    }
}

// ── Phase A: loopback ───────────────────────────────────────────────────────
// Feeds known-good lines of both wire formats out of TX2 and expects them
// back on RX2. Because it exercises the real UART rather than calling the
// parser directly, a wrong pin mapping or baud rate fails here too - which is
// exactly what the native tests cannot catch.
static bool phaseLoopback() {
    Serial.println();
    Serial.println("[A] Loopback - jumper GPIO32 to GPIO33.");

    struct Probe { const char *line; const char *expectText; };
    static const Probe probes[] = {
        {"+RX,2,5,-80,9,SOS\n",                                  "SOS"},
        {"+TX,4,ALL CLEAR\n",                                    "ALL CLEAR"},
        {"[00:00:12.345,000] <inf> lora_mesh: RX from node 7 seq 1 "
         "ttl 3 (RSSI -55 dBm, SNR 7 dB): IM HERE\n",            "IM HERE"},
        {"this is ordinary log noise and must be ignored\n",     nullptr},
    };

    uint8_t passed = 0, expected = 0;
    for (const Probe &p : probes) {
        if (p.expectText) expected++;

        const uint32_t beforeRx = rxEvents, beforeTx = txEvents;
        const uint8_t  beforeCount = store.count();

        Serial2.print(p.line);
        Serial2.flush();

        // Give the bytes time to come back around and be parsed.
        const uint32_t deadline = millis() + 400;
        while (millis() < deadline) {
            nrfLink.poll(millis());
            if (rxEvents != beforeRx || txEvents != beforeTx) break;
            delay(2);
        }

        const bool gotOne = (store.count() != beforeCount);
        if (p.expectText) {
            const bool ok = gotOne && strcmp(store.at(0)->text, p.expectText) == 0;
            Serial.printf("    %-28.28s -> %s\n", p.line,
                          ok ? "parsed ok" : "FAILED");
            if (ok) passed++;
        } else {
            Serial.printf("    %-28.28s -> %s\n", p.line,
                          gotOne ? "FAILED (should be ignored)" : "ignored ok");
            if (!gotOne) passed++;
            expected++;
        }
    }

    const bool ok = (passed == expected);
    Serial.printf("    %u/%u probes behaved correctly.\n", passed, expected);
    if (!ok) {
        Serial.println("    -> No jumper fitted? TX2=GPIO32 to RX2=GPIO33.");
        Serial.println("       If the jumper IS fitted, recheck those two pins.");
    }
    return ok;
}

static void drawStatus(bool loopbackOk) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("STAGE 4 - UART", tft.width() / 2, 6, 4);
    tft.setTextDatum(TL_DATUM);

    tft.setTextColor(loopbackOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(loopbackOk ? "loopback PASS" : "loopback FAIL", 6, 40, 2);

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char buf[48];
    snprintf(buf, sizeof(buf), "TX2=GPIO%d  RX2=GPIO%d", PIN_NRF_TX, PIN_NRF_RX);
    tft.drawString(buf, 6, 60, 2);
    snprintf(buf, sizeof(buf), "%d baud 8N1", NRF_BAUD);
    tft.drawString(buf, 6, 78, 2);
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 4 - UART link to the nRF52840");
    Serial.println("=====================================================");
    Serial.printf("  TX2 = GPIO%d -> nRF RX     RX2 = GPIO%d <- nRF TX\n",
                  PIN_NRF_TX, PIN_NRF_RX);
    Serial.printf("  %d baud, 8N1, common ground required\n", NRF_BAUD);
    Serial.println("-----------------------------------------------------");

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);

    nrfLink.begin();
    nrfLink.setEventHandler(onLinkEvent, nullptr);

    const bool ok = phaseLoopback();
    drawStatus(ok);

    Serial.println();
    Serial.println("[B] Live - remove the jumper and wire the nRF.");
    Serial.println("    Anything you type here is sent to the node as a line.");
    Serial.println("    Its replies are parsed and printed below.");
    Serial.println("-----------------------------------------------------");
}

void loop() {
    static uint32_t lastReport = 0;
    static char     typed[MSG_MAX_LEN + 1];
    static uint8_t  typedLen = 0;

    const uint32_t now = millis();
    nrfLink.poll(now);

    // Bridge the USB console to the radio, so a message can be sent by typing
    // without any UI in the way.
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (typedLen) {
                typed[typedLen] = '\0';
                Serial.printf("  -> sending \"%s\"\n", typed);
                if (!nrfLink.send(typed)) Serial.println("  !! TX queue full");
                typedLen = 0;
            }
        } else if (typedLen < MSG_MAX_LEN) {
            typed[typedLen++] = c;
        }
        // Characters past MSG_MAX_LEN are dropped: the radio would drop them
        // anyway (reference/nrf.cpp:32), and doing it here makes that visible.
    }

    if (now - lastReport >= 5000) {
        lastReport = now;
        const LoraLinkParser &p = nrfLink.parser();
        Serial.printf("  link %s | lines %lu, events %lu, overruns %lu | "
                      "rx %lu tx %lu | queue %u/%u\n",
                      nrfLink.linkUp(now) ? "UP  " : "DOWN",
                      (unsigned long)p.linesSeen(),
                      (unsigned long)p.eventsParsed(),
                      (unsigned long)p.overruns(),
                      (unsigned long)rxEvents, (unsigned long)txEvents,
                      nrfLink.queueDepth(), nrfLink.queueCapacity());

        if (!nrfLink.linkUp(now) && p.linesSeen() == 0) {
            Serial.println("    Nothing received yet. If the nRF is wired:");
            Serial.println("    is its Zephyr console on a hardware UART?");
            Serial.println("    USB CDC cannot be reached from here (PLAN 3.3).");
        }
    }
}
