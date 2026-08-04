// ─────────────────────────────────────────────────────────────────────────────
// Stage 1b — TFT display bring-up.   pio run -e t1_display -t upload
//
// Run Stage 1a (t1a_lcdid) FIRST and set the *_DRIVER flag in platformio.ini
// to whatever it reported. A wrong driver here shows as a white screen, a
// shifted image, or wildly wrong colours, and none of that is a wiring fault.
//
// Proves the panel and nothing else: no touch, no buttons, no UART. Each test
// is self-describing on screen so it can be judged by looking; the serial log
// says what a pass looks like and reports what a human cannot eyeball (bus
// throughput, readback, brownout).
//
// The whole sequence repeats forever, so there is time to look twice.
//
// Exit criteria (PLAN.md stage 1b):
//   - colours are named correctly, not BGR-swapped
//   - the white border is fully visible on all four edges, no offset
//   - all four rotations put "TOP-LEFT" in the physical top-left
//   - fonts are legible and unsmeared
//   - no reboot and no ESP_RST_BROWNOUT across a full pass
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_system.h>

#include "pins.h"
#include "app_config.h"

static TFT_eSPI tft;

// The shield's backlight is hardwired to its own 3.3V rail — there is no
// control pin on the Uno headers, so there is nothing to switch or dim.

// Title bar + caption, so a photo of the panel is enough to tell which test
// was running.
static void slate(const char* title, uint16_t bg, uint16_t fg) {
    tft.fillScreen(bg);
    tft.setTextColor(fg, bg);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(title, tft.width() / 2, 8, 4);
    tft.setTextDatum(TL_DATUM);
}

// ── 1. Reset reason ─────────────────────────────────────────────────────────
// A brownout here means the 3.3V rail cannot carry the panel (risk R6), and
// every later symptom would be a red herring.
static void reportResetReason() {
    const char* r;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  r = "power-on";                      break;
        case ESP_RST_SW:       r = "software";                      break;
        case ESP_RST_PANIC:    r = "PANIC (exception)";             break;
        case ESP_RST_INT_WDT:  r = "interrupt watchdog";            break;
        case ESP_RST_TASK_WDT: r = "task watchdog";                 break;
        case ESP_RST_BROWNOUT: r = "BROWNOUT - check the 3V3 rail"; break;
        case ESP_RST_DEEPSLEEP:r = "deep sleep wake";               break;
        default:               r = "other";                         break;
    }
    Serial.printf("  reset reason ....... %s\n", r);
}

// ── 2. Panel identity ───────────────────────────────────────────────────────
// Confirms the driver compiled in actually matches the silicon. Stage 1a
// already answered this over serial; repeating it here means a photo of the
// screen alone is enough to tell which driver produced the image.
static void testIdentity() {
    Serial.println("[2] Driver / geometry cross-check.");
    slate("DRIVER", TFT_BLACK, TFT_CYAN);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    char buf[64];
    int y = 44;
#if   defined(ILI9342_DRIVER)
    const char* drv = "ILI9342";
#elif defined(ILI9341_DRIVER)
    const char* drv = "ILI9341";
#elif defined(ILI9481_DRIVER)
    const char* drv = "ILI9481";
#elif defined(ILI9486_DRIVER)
    const char* drv = "ILI9486";
#elif defined(ILI9488_DRIVER)
    const char* drv = "ILI9488";
#elif defined(ST7789_DRIVER)
    const char* drv = "ST7789";
#elif defined(ST7796_DRIVER)
    const char* drv = "ST7796";
#elif defined(HX8357D_DRIVER)
    const char* drv = "HX8357D";
#elif defined(RM68140_DRIVER)
    const char* drv = "RM68140";
#else
    const char* drv = "unknown";
#endif
    snprintf(buf, sizeof(buf), "driver: %s", drv);
    tft.drawString(buf, 6, y, 2); y += 22;
    snprintf(buf, sizeof(buf), "panel: %d x %d", tft.width(), tft.height());
    tft.drawString(buf, 6, y, 2); y += 22;
    tft.drawString("bus: 8-bit parallel", 6, y, 2); y += 22;
    snprintf(buf, sizeof(buf), "WR=%d RD=%d RS=%d CS=%d",
             PIN_LCD_WR, PIN_LCD_RD, PIN_LCD_RS, PIN_LCD_CS);
    tft.drawString(buf, 6, y, 2);

    Serial.printf("    driver=%s  panel=%dx%d\n", drv, tft.width(), tft.height());
    delay(2500);
}

// ── 3. Colour order ─────────────────────────────────────────────────────────
// The single most common ILI9341 fault: a panel wired BGR shows this test's
// RED as blue. The fix is a build flag, not a code change, so the log says so.
static void testColours() {
    struct { const char* name; uint16_t c; uint16_t fg; } steps[] = {
        {"RED",   TFT_RED,   TFT_WHITE},
        {"GREEN", TFT_GREEN, TFT_BLACK},
        {"BLUE",  TFT_BLUE,  TFT_WHITE},
        {"WHITE", TFT_WHITE, TFT_BLACK},
        {"BLACK", TFT_BLACK, TFT_WHITE},
    };
    Serial.println("[3] Colour order - each screen must match its own label.");
    Serial.println("    If RED shows blue, add -DTFT_RGB_ORDER=TFT_BGR to");
    Serial.println("    build_flags. If everything is inverted, -DTFT_INVERSION_ON.");

    for (auto& s : steps) {
        tft.fillScreen(s.c);
        tft.setTextColor(s.fg, s.c);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(s.name, tft.width() / 2, tft.height() / 2, 6);
        tft.setTextDatum(TL_DATUM);
        delay(900);
    }
}

// ── 4. Geometry ─────────────────────────────────────────────────────────────
// A 1-pixel border plus corner crosses. A missing edge means the driver's
// width/height or column/row offset is wrong for this particular panel.
static void testGeometry() {
    const int w = tft.width(), h = tft.height();
    Serial.printf("[4] Geometry - %dx%d. All four edges of the white border\n", w, h);
    Serial.println("    must be visible, and all four corner marks complete.");

    tft.fillScreen(TFT_BLACK);
    tft.drawRect(0, 0, w, h, TFT_WHITE);

    const int n = 20;
    tft.drawFastHLine(0, 0, n, TFT_RED);      tft.drawFastVLine(0, 0, n, TFT_RED);
    tft.drawFastHLine(w - n, 0, n, TFT_GREEN); tft.drawFastVLine(w - 1, 0, n, TFT_GREEN);
    tft.drawFastHLine(0, h - 1, n, TFT_BLUE);  tft.drawFastVLine(0, h - n, n, TFT_BLUE);
    tft.drawFastHLine(w - n, h - 1, n, TFT_YELLOW);
    tft.drawFastVLine(w - 1, h - n, n, TFT_YELLOW);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("TL", 4, 24, 2);
    tft.setTextDatum(TR_DATUM); tft.drawString("TR", w - 4, 24, 2);
    tft.setTextDatum(BL_DATUM); tft.drawString("BL", 4, h - 24, 2);
    tft.setTextDatum(BR_DATUM); tft.drawString("BR", w - 4, h - 24, 2);
    tft.setTextDatum(TL_DATUM);
    delay(2500);
}

// ── 5. Gradient ─────────────────────────────────────────────────────────────
// On a parallel bus, speckle in a smooth ramp means one data line is not
// making contact — a stuck bit shows up as a repeating stripe at whichever
// power of two that line carries. Vertical banding at 16-pixel spacing is D4,
// at 2-pixel spacing is D1, and so on.
static void testGradient() {
    Serial.println("[5] Gradient - smooth ramps, no banding, no speckle.");
    Serial.println("    Regular stripes => one data line is not connected;");
    Serial.println("    the stripe spacing tells you which bit.");
    const int w = tft.width(), h = tft.height();
    const int band = h / 4;

    for (int x = 0; x < w; x++) {
        uint8_t v = (uint8_t)((x * 255) / (w - 1));
        tft.drawFastVLine(x, 0,        band, tft.color565(v, 0, 0));
        tft.drawFastVLine(x, band,     band, tft.color565(0, v, 0));
        tft.drawFastVLine(x, band * 2, band, tft.color565(0, 0, v));
        tft.drawFastVLine(x, band * 3, h - band * 3, tft.color565(v, v, v));
    }
    delay(2500);
}

// ── 6. Fonts ────────────────────────────────────────────────────────────────
// Every font this project actually uses, at the sizes the UI will use them.
static void testFonts() {
    Serial.println("[6] Fonts - all lines legible, no smearing or clipping.");
    slate("FONTS", TFT_BLACK, TFT_CYAN);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    int y = 40;
    tft.drawString("F1 the quick brown fox", 4, y, 1);  y += 14;
    tft.drawString("F2 NEED REINFORCEMENT", 4, y, 2);   y += 22;
    tft.drawString("F4 node 2  -80dBm", 4, y, 4);       y += 32;
    tft.drawString("SOS", 4, y, 6);                     y += 52;
    tft.drawString("12:34", 4, y, 7);                   y += 52;

    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("32 chars: ................", 4, y, 2);
    delay(3000);
}

// ── 7. Rotation ─────────────────────────────────────────────────────────────
static void testRotations() {
    Serial.println("[7] Rotation - in each of the 4, the red block and the");
    Serial.println("    words TOP-LEFT must sit in the physical top-left.");
    for (uint8_t r = 0; r < 4; r++) {
        tft.setRotation(r);
        tft.fillScreen(TFT_NAVY);
        tft.fillRect(0, 0, 40, 40, TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_NAVY);
        tft.drawString("TOP-LEFT", 44, 12, 2);
        char buf[48];
        snprintf(buf, sizeof(buf), "rotation %u  %dx%d", r, tft.width(), tft.height());
        tft.drawString(buf, 4, 60, 2);
        delay(1400);
    }
    tft.setRotation(TFT_ROTATION);
}

// ── 8. Bus readback ─────────────────────────────────────────────────────────
// The parallel bus is bidirectional and RD is wired, so unlike an SPI module
// this really can be read back. A mismatch on exactly one bit is a loose data
// line, which is worth knowing before it is blamed on the driver.
static void testReadback() {
    Serial.println("[8] Bus readback (writes a colour, reads the pixel back):");
    tft.fillScreen(TFT_BLACK);
    const uint16_t probes[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        tft.fillRect(0, 0, 32, 32, probes[i]);
        uint16_t got = tft.readPixel(4, 4);
        // The read path is 18-bit truncated to 16, so the low bits of each
        // channel are lost. Compare only the top 4 bits per channel.
        bool match = (got & 0xF79E) == (probes[i] & 0xF79E);
        Serial.printf("    wrote 0x%04X  read 0x%04X  %s\n",
                      probes[i], got, match ? "ok" : "MISMATCH");
        ok += match;
    }
    if (ok == 4) {
        Serial.println("    -> bus reads back correctly, RD is wired.");
    } else {
        Serial.println("    -> MISMATCH. XOR the wrote/read values above: a");
        Serial.println("       single differing bit names the loose data line.");
    }
}

// ── 9. Throughput ───────────────────────────────────────────────────────────
// The 8-bit parallel bus on an ESP32 should clear 30+ full-screen fills per
// second. Much below that and the UI will feel sluggish once it is redrawing
// message lists, so it is worth knowing the ceiling before designing to it.
static void testThroughput() {
    const int frames = 20;
    uint32_t t0 = millis();
    for (int i = 0; i < frames; i++) tft.fillScreen(i & 1 ? TFT_BLACK : TFT_DARKGREY);
    uint32_t dt = millis() - t0;

    float fps = (frames * 1000.0f) / (float)dt;
    uint32_t px = (uint32_t)tft.width() * tft.height() * frames;
    Serial.printf("[9] Throughput: %d full-screen fills in %lu ms\n",
                  frames, (unsigned long)dt);
    Serial.printf("    %.1f fps, %.2f Mpixel/s on the 8-bit parallel bus\n",
                  fps, (px / 1000000.0f) / (dt / 1000.0f));
    if (fps < 20.0f) {
        Serial.println("    LOW - long jumper wires, or the driver is falling");
        Serial.println("    back to a per-pin path. Keep the leads short.");
    }
}

// ── 10. UI preview ──────────────────────────────────────────────────────────
// A static mock of the real inbox screen. Not a test of the panel so much as
// a check that the layout the UI layer will use is actually readable on this
// physical display, at this size, before any of it is written.
static void previewInbox() {
    Serial.println("[10] Inbox layout preview - is this readable at arm's length?");
    const int w = tft.width();
    tft.fillScreen(TFT_BLACK);

    tft.fillRect(0, 0, w, 26, TFT_DARKGREEN);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("INBOX", 6, 5, 2);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("LINK OK", w - 6, 5, 2);
    tft.setTextDatum(TL_DATUM);

    struct { const char* who; const char* msg; const char* age; bool alert; } rows[] = {
        {"node 2", "SOS",                "12s", true },
        {"node 4", "NEED REINFORCEMENT", "1m",  false},
        {"node 2", "IM HERE",            "3m",  false},
        {"node 7", "ALL CLEAR",          "8m",  false},
    };

    int y = 32;
    for (auto& r : rows) {
        uint16_t bg = r.alert ? TFT_MAROON : TFT_BLACK;
        tft.fillRect(0, y, w, 44, bg);
        tft.drawFastHLine(0, y + 43, w, TFT_DARKGREY);
        tft.setTextColor(r.alert ? TFT_YELLOW : TFT_CYAN, bg);
        tft.drawString(r.who, 6, y + 4, 2);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(TFT_LIGHTGREY, bg);
        tft.drawString(r.age, w - 6, y + 4, 2);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, bg);
        tft.drawString(r.msg, 6, y + 22, 2);
        y += 44;
    }

    tft.fillRect(0, tft.height() - 24, w, 24, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("UP/DN  OK=open  BACK", 6, tft.height() - 19, 2);
    delay(4000);
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 1b - TFT display bring-up (8-bit parallel shield)");
    Serial.println("=====================================================");
    Serial.printf("  data bus ........... D0=%d D1=%d D2=%d D3=%d\n",
                  PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    Serial.printf("                       D4=%d D5=%d D6=%d D7=%d\n",
                  PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);
    Serial.printf("  control ............ RD=%d WR=%d RS=%d CS=%d RST=%d\n",
                  PIN_LCD_RD, PIN_LCD_WR, PIN_LCD_RS,
                  PIN_LCD_CS, PIN_LCD_RST);
    Serial.printf("  backlight .......... fixed (shield's own 3.3V rail)\n");
    Serial.printf("  free heap .......... %lu bytes\n",
                  (unsigned long)ESP.getFreeHeap());
    reportResetReason();
    Serial.println("-----------------------------------------------------");

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);
    Serial.printf("[1] init ok - reported panel size %dx%d\n",
                  tft.width(), tft.height());
}

void loop() {
    testIdentity();
    testColours();
    testGeometry();
    testGradient();
    testFonts();
    testRotations();
    testReadback();
    testThroughput();
    previewInbox();

    Serial.println("-----------------------------------------------------");
    Serial.printf("  free heap after a full pass: %lu bytes\n",
                  (unsigned long)ESP.getFreeHeap());
    reportResetReason();   // a brownout mid-pass shows up as a reset here
    Serial.println("  Pass complete. Repeating in 3s.");
    Serial.println("=====================================================");
    delay(3000);
}
