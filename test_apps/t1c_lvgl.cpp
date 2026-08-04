// ─────────────────────────────────────────────────────────────────────────────
// Stage 1c — LVGL port bring-up.   pio run -e t1c_lvgl -t upload
//
// Proves the LVGL *port layer* and nothing else: draw buffer sizing, the flush
// callback, the tick source, the memory pool, fonts and the theme. No touch,
// no buttons, no UART - LVGL is driven entirely by its own animation timers,
// which is exactly what makes this a clean test of the port.
//
// This slots between Stage 1b (raw panel proven) and Stage 2 (touch), so that
// when the first real input arrives there is no question whether a rendering
// problem is LVGL's or the driver's.
//
// Exit criteria:
//   - a dark themed screen appears with a header, a scrolling list and a bar
//   - the progress bar animates smoothly (proves the tick is running)
//   - colours are correct (proves the RGB565 byte order in flushCb)
//   - text is sharp at all four sizes (proves the fonts are compiled in)
//   - LVGL's memory pool settles and stops growing (proves no leak per frame)
//   - measured refresh rate is comfortably above the 30 fps target
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

#include "Touch.h"
#include "app_config.h"
#include "lvgl_port.h"
#include "pins.h"

static TFT_eSPI tft;
static Touch    touch;

// Frame counter, incremented from a display event so it counts real flushes
// rather than loop() iterations.
static volatile uint32_t s_flushes = 0;
static lv_obj_t *s_bar   = nullptr;
static lv_obj_t *s_clock = nullptr;

static void onRender(lv_event_t *) { s_flushes++; }

// Live rotation, so the right value can be read off the panel instead of
// guessed. Only active while touch is uncalibrated - see cycleRotationOnTap().
static uint8_t s_rotation = TFT_ROTATION;
static bool    s_rotationCycler = false;

// ── Layout constants ────────────────────────────────────────────────────────
// Fixed bands top and bottom; the list takes everything in between, so the
// three together always account for exactly the panel height whatever it is.
static constexpr int32_t kHeaderH = 32;
static constexpr int32_t kFooterH = 34;
static constexpr int32_t kRowH    = 44;

// Strip every default a container ships with: the theme gives lv_obj a
// rounded border, an inset pad and a background, all of which leave visible
// gutters between bands. For a full-bleed layout these have to go, and doing
// it in one helper stops one container quietly keeping its padding.
static void makeBand(lv_obj_t *o, int32_t h, uint32_t bg) {
    lv_obj_set_size(o, LV_PCT(100), h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(o, 0, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}

// ── The screen ──────────────────────────────────────────────────────────────
// Built from the widgets the real UI will use - header, scrollable message
// list, status footer - rather than LVGL's stock demo, so that layout problems
// surface now while they still cost nothing to fix.
static void buildScreen() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    // ── Header ──────────────────────────────────────────────────────────────
    lv_obj_t *hdr = lv_obj_create(scr);
    makeBand(hdr, kHeaderH, 0x15803d);
    lv_obj_set_style_pad_hor(hdr, 8, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "INBOX");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    s_clock = lv_label_create(hdr);
    // While the rotation cycler is live this slot shows which rotation is on
    // screen, so the correct value can be read directly off the panel.
    if (s_rotationCycler) {
        char rb[24];
        snprintf(rb, sizeof(rb), "ROT %u  %s", s_rotation,
                 (s_rotation & 1) ? "landscape" : "portrait");
        lv_label_set_text(s_clock, rb);
    } else {
        lv_label_set_text(s_clock, "LINK --");
    }
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(0xd1fae5), LV_PART_MAIN);
    lv_obj_align(s_clock, LV_ALIGN_RIGHT_MID, 0, 0);

    // ── Message list ────────────────────────────────────────────────────────
    // flex_grow takes whatever the header and footer leave. lv_list defaults
    // its height to LV_SIZE_CONTENT, which would size it to all seven rows and
    // overrun the panel, so this is doing real work, not just being tidy.
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 1, LV_PART_MAIN);
    // AUTO rather than OFF now that touch exists: with seven 44 px rows in a
    // 254 px band the list scrolls, and with no scrollbar there is nothing to
    // suggest there is more below.
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    struct Row { const char *who; const char *msg; const char *age; bool alert; };
    static const Row rows[] = {
        {"node 2", "SOS",                "12s", true },
        {"node 4", "NEED REINFORCEMENT", "1m",  false},
        {"node 2", "IM HERE",            "3m",  false},
        {"node 7", "ALL CLEAR",          "8m",  false},
        {"node 3", "HOLD POSITION",      "12m", false},
        {"node 5", "MEDIC NEEDED",       "15m", true },
        {"node 1", "MOVING OUT",         "22m", false},
    };

    for (const Row &r : rows) {
        // The text argument MUST be nullptr, not "".
        //
        // lv_list_add_button only skips creating its label on a NULL pointer -
        // an empty string is still a string, so it builds a label, sets
        // LV_LABEL_LONG_MODE_SCROLL_CIRCULAR on it and gives it flex_grow 1.
        // That invisible label then expands to absorb the row, pushing the
        // real content out of place. Nothing about it looks wrong in the code.
        lv_obj_t *btn = lv_list_add_button(list, nullptr, nullptr);
        lv_obj_set_size(btn, LV_PCT(100), kRowH);
        lv_obj_set_style_bg_color(btn,
            r.alert ? lv_color_hex(0x7f1d1d) : lv_color_hex(0x1a2230),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        // Visible feedback on touch, so a tap that lands is obviously
        // distinguishable from one the panel missed.
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2563eb),
                                  LV_PART_MAIN | LV_STATE_PRESSED);

        // Absolute alignment inside the row rather than flex: two lines at
        // known offsets is easier to read at a glance than a nested layout.
        lv_obj_t *who = lv_label_create(btn);
        lv_label_set_text(who, r.who);
        lv_obj_set_style_text_font(who, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(who,
            r.alert ? lv_color_hex(0xfbbf24) : lv_color_hex(0x7dd3fc),
            LV_PART_MAIN);
        lv_obj_align(who, LV_ALIGN_TOP_LEFT, 0, 3);

        lv_obj_t *age = lv_label_create(btn);
        lv_label_set_text(age, r.age);
        lv_obj_set_style_text_font(age, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(age, lv_color_hex(0x94a3b8), LV_PART_MAIN);
        lv_obj_align(age, LV_ALIGN_TOP_RIGHT, 0, 3);

        lv_obj_t *msg = lv_label_create(btn);
        lv_label_set_text(msg, r.msg);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(msg, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(msg, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    }

    // ── Footer with an animated bar ─────────────────────────────────────────
    // The animation is the actual test: without a working tick callback,
    // everything above still renders perfectly once and then sits frozen -
    // which looks like success until you watch it for a second.
    lv_obj_t *foot = lv_obj_create(scr);
    makeBand(foot, kFooterH, 0x1e293b);
    lv_obj_set_style_pad_hor(foot, 8, LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(foot);
    lv_label_set_text(hint, s_rotationCycler ? "TAP anywhere to rotate"
                                             : "UP/DN   OK open   BACK");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94a3b8), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 2);

    s_bar = lv_bar_create(foot);
    lv_obj_set_size(s_bar, LV_PCT(100), 6);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_bar_set_range(s_bar, 0, 100);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bar);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_bar_set_value(static_cast<lv_obj_t *>(obj), v, LV_ANIM_OFF);
    });
    lv_anim_set_duration(&a, 1500);
    lv_anim_set_playback_duration(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_start(&a);
}

// ── Does the layout actually cover the panel? ───────────────────────────────
// Prints where LVGL placed each band and whether they tile the full height
// with no gap and no overrun. Cheaper and far more definitive than judging
// a few pixels of dark background by eye.
static void dumpGeometry() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_update_layout(scr);

    const int32_t sw = lv_obj_get_width(scr);
    const int32_t sh = lv_obj_get_height(scr);
    Serial.printf("  screen ............. %ldx%ld\n", (long)sw, (long)sh);

    int32_t covered = 0;
    const uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        const int32_t x = lv_obj_get_x(c), y = lv_obj_get_y(c);
        const int32_t w = lv_obj_get_width(c), h = lv_obj_get_height(c);
        covered += h;
        Serial.printf("  band %lu ............ x=%3ld y=%3ld  %3ldx%-3ld%s\n",
                      (unsigned long)i, (long)x, (long)y, (long)w, (long)h,
                      (w == sw) ? "" : "  <- NOT full width");
    }
    Serial.printf("  bands total height . %ld of %ld  %s\n",
                  (long)covered, (long)sh,
                  covered == sh ? "OK - tiles the panel exactly"
                                : "<- GAP or OVERRUN");
}

// ── Live rotation ───────────────────────────────────────────────────────────
// Retarget the panel and LVGL together, then rebuild. Note this drives
// TFT_eSPI's own rotation and only tells LVGL the new resolution — it does NOT
// use lv_display_set_rotation(), which would rotate in software on top and
// leave the two turning against each other.
static void applyRotation(uint8_t r) {
    s_rotation = r & 0x3;
    tft.setRotation(s_rotation);

    lv_display_t *d = lv_display_get_default();
    lv_display_set_resolution(d, tft.width(), tft.height());

    lv_obj_clean(lv_screen_active());   // also cancels animations on the deleted objects
    buildScreen();
    lv_obj_invalidate(lv_screen_active());

    Serial.println();
    Serial.printf("  rotation -> %u  (%dx%d, %s)\n", s_rotation,
                  tft.width(), tft.height(),
                  (s_rotation & 1) ? "landscape" : "portrait");
    dumpGeometry();
    Serial.printf("  >> when this looks right, set TFT_ROTATION %u "
                  "in include/app_config.h\n", s_rotation);
    lvglPortResetCoverage();
}

// Advance on a press edge. Uses readRaw(), which needs no calibration — the
// whole point is to settle the orientation *before* calibrating, since a
// calibration is only valid for the rotation it was taken in.
static void cycleRotationOnTap() {
    static bool     wasPressed = false;
    static uint32_t lastPoll = 0;

    // A read costs about a millisecond and briefly commandeers the LCD bus,
    // so it is polled rather than run every loop iteration.
    if (millis() - lastPoll < 100) return;
    lastPoll = millis();

    TouchRaw r;
    const bool pressed = touch.readRaw(r);
    if (pressed && !wasPressed) applyRotation(s_rotation + 1);
    wasPressed = pressed;
}

static void reportMemory(const char *when) {
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    Serial.printf("  [%s] LVGL pool %u/%u B used (%u%%), frag %u%%, "
                  "largest free %u B | ESP heap %lu B\n",
                  when, (unsigned)(m.total_size - m.free_size),
                  (unsigned)m.total_size, (unsigned)m.used_pct,
                  (unsigned)m.frag_pct, (unsigned)m.free_biggest_size,
                  (unsigned long)ESP.getFreeHeap());
}

void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println(" Stage 1c - LVGL port bring-up");
    Serial.println("=====================================================");
    Serial.printf("  LVGL ............... v%d.%d.%d\n",
                  LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    Serial.printf("  heap before ........ %lu B\n",
                  (unsigned long)ESP.getFreeHeap());

    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);

    if (!lvglPortInit(tft)) {
        Serial.println("  FATAL: draw buffer allocation failed.");
        Serial.println("  Lower LVGL_BUF_LINES or LV_MEM_SIZE and rebuild.");
        while (true) delay(1000);
    }

    touch.begin();
    if (!touch.loadCal()) {
        // Uncalibrated: taps cannot address widgets, so they are put to work
        // choosing the orientation instead. This is the right order anyway -
        // a calibration is only valid for the rotation it was taken in, so
        // settling rotation first avoids calibrating twice.
        s_rotationCycler = true;
        Serial.println("  touch .............. uncalibrated -> ROTATION CYCLER ON");
        Serial.println("                       tap the panel to step 0->1->2->3");
        Serial.println("                       then run: pio run -e t2_touch -t upload");
    } else if (lvglPortInitTouch(touch)) {
        Serial.println("  touch .............. active (LV_INDEV_TYPE_POINTER)");
    } else {
        Serial.println("  touch .............. failed to register");
    }

    Serial.printf("  panel .............. %dx%d (rotation %d)\n",
                  tft.width(), tft.height(), TFT_ROTATION);
    Serial.printf("  draw buffer ........ %u B (%d lines, RGB565)\n",
                  (unsigned)lvglPortDrawBufBytes(), LVGL_BUF_LINES);
    Serial.printf("  LV_MEM_SIZE ........ %u B\n", (unsigned)LV_MEM_SIZE);
    reportMemory("post-init");

    lv_display_add_event_cb(lv_display_get_default(), onRender,
                            LV_EVENT_RENDER_READY, nullptr);
    buildScreen();
    reportMemory("post-build");

    Serial.println("-----------------------------------------------------");
    dumpGeometry();
    Serial.println("-----------------------------------------------------");
    Serial.println(" Watch the panel: the bar must sweep continuously.");
    Serial.println(" A frozen bar with everything else drawn correctly");
    Serial.println(" means the tick callback is not running.");
    Serial.println("-----------------------------------------------------");

    lvglPortResetCoverage();
}

void loop() {
    static uint32_t lastReport = 0;
    static uint32_t lastFlushes = 0;
    static uint32_t seconds = 0;

    lvglPortTask();
    if (s_rotationCycler) cycleRotationOnTap();

    const uint32_t now = millis();
    if (now - lastReport >= 1000) {
        lastReport = now;
        seconds++;

        const uint32_t f = s_flushes;
        const uint32_t fps = f - lastFlushes;
        lastFlushes = f;

        char buf[24];
        snprintf(buf, sizeof(buf), "LINK %lus", (unsigned long)seconds);
        lv_label_set_text(s_clock, buf);

        Serial.printf("  t=%3lus  %2lu refresh/s", (unsigned long)seconds,
                      (unsigned long)fps);
        lv_mem_monitor_t m;
        lv_mem_monitor(&m);
        Serial.printf("  pool %2u%% used, frag %2u%%, ESP heap %lu B\n",
                      (unsigned)m.used_pct, (unsigned)m.frag_pct,
                      (unsigned long)ESP.getFreeHeap());

        // After the first full paint, report which pixels were actually sent
        // to the panel. Anything short of the full panel resolution (taken
        // from the display, so it is rotation-correct) means the port is
        // not addressing the whole display - a different fault entirely from
        // a layout that merely leaves background showing.
        if (seconds == 3) {
            int32_t x1, y1, x2, y2;
            uint32_t flushes;
            lvglPortGetCoverage(x1, y1, x2, y2, flushes);
            const int32_t w = lv_display_get_horizontal_resolution(nullptr);
            const int32_t h = lv_display_get_vertical_resolution(nullptr);
            const bool full = (x1 == 0 && y1 == 0 && x2 == w - 1 && y2 == h - 1);
            Serial.println("-----------------------------------------------------");
            Serial.printf("  flush coverage ..... (%ld,%ld)-(%ld,%ld) over %lu flushes\n",
                          (long)x1, (long)y1, (long)x2, (long)y2,
                          (unsigned long)flushes);
            Serial.printf("  panel .............. (0,0)-(%ld,%ld)  %s\n",
                          (long)(w - 1), (long)(h - 1),
                          full ? "OK - complete display used"
                               : "<- PORT IS NOT PAINTING THE WHOLE PANEL");
            Serial.println("-----------------------------------------------------");
        }
    }
}
