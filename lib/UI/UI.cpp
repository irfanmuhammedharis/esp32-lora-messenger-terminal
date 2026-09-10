#include "UI.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "lvgl_port.h"

// ── Layout, in pixels, for the panel's native 240x320 portrait ──────────────
// Header and footer are fixed; the body takes the remainder via flex_grow, so
// the three bands always tile the panel exactly whatever its height is. Stage
// 1c's dumpGeometry() is what proves that at run time.
static constexpr int32_t kHeaderH = 32;
// The footer is an action bar carrying the SOS and BACK controls, so it has
// to be tall enough for a UI_MIN_TOUCH_PX target plus a little breathing
// room. That is why it is 48 and not the 34 a caption strip would need.
static constexpr int32_t kFooterH = 48;
static constexpr int32_t kRowH    = 46;   // -> 5 message rows in the 240 px body
static constexpr int32_t kPresetRowH = 44;

static_assert(kFooterH >= UI_MIN_TOUCH_PX,
              "footer cannot hold a minimum-size touch target");
static_assert(kRowH >= UI_MIN_TOUCH_PX,
              "inbox rows are below the minimum touch target");
static_assert(kPresetRowH >= UI_MIN_TOUCH_PX,
              "preset rows are below the minimum touch target");

// Palette. Dark, because this is a field device and a bright panel at night
// is a liability (matches LV_THEME_DEFAULT_DARK in lv_conf.h).
static constexpr uint32_t kBg        = 0x0d1117;
static constexpr uint32_t kHeaderBg  = 0x15803d;
static constexpr uint32_t kFooterBg  = 0x1e293b;
static constexpr uint32_t kRowBg     = 0x1a2230;
static constexpr uint32_t kRowAlert  = 0x7f1d1d;
static constexpr uint32_t kRowAction = 0x1d4ed8;
static constexpr uint32_t kPressed   = 0x2563eb;
static constexpr uint32_t kTextDim   = 0x94a3b8;
static constexpr uint32_t kTextBlue  = 0x7dd3fc;
static constexpr uint32_t kTextWarn  = 0xfbbf24;
static constexpr uint32_t kSosBg     = 0xb91c1c;

// The inbox renders at most this many rows. The store holds 32, but building
// all of them would blow LV_MEM_SIZE - Stage 1c measured seven rows at 27% of
// the 48 KB pool. 16 is two and a half screenfuls of scroll, which is far
// more history than anyone reads on a 2.4" panel in the field; the rest stays
// in the store and is still counted in the unread badge.
static constexpr uint8_t kInboxMaxRows = 16;

// Rows pinned above the messages, so the send path is reachable from the home
// screen without any navigation: one tap opens presets, a second sends.
enum ActionId : uint32_t {
    kActSend = 1, kActCompose = 2, kActStatus = 3, kActVitals = 4
};

// ── Small helpers ───────────────────────────────────────────────────────────

// Strip every default a container ships with. The theme gives lv_obj a
// rounded border, an inset pad and a background, which leave visible gutters
// between bands; for a full-bleed layout these have to go.
static void stripStyle(lv_obj_t *o) {
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(o, 0, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                       uint32_t colour) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), LV_PART_MAIN);
    return l;
}

// "12s", "4m", "3h" - a relative age is what the operator actually reasons
// about, and it fits where an absolute clock would not (there is no RTC here
// anyway, so an absolute time would be a lie).
static void formatAge(char *buf, size_t n, uint32_t rxMillis, uint32_t nowMs) {
    const uint32_t sec = (nowMs - rxMillis) / 1000;
    if (sec < 60)        snprintf(buf, n, "%lus", (unsigned long)sec);
    else if (sec < 3600) snprintf(buf, n, "%lum", (unsigned long)(sec / 60));
    else                 snprintf(buf, n, "%luh", (unsigned long)(sec / 3600));
}

// Link state wording and colours (PLAN.md 4.1a). The header gets the short
// form, the Status screen the full sentence. SEARCHING is deliberately amber,
// never red: it is a deployment condition, not a fault.
static const char *linkStateShort(LinkState s) {
    switch (s) {
        case LinkState::Down:      return "--";
        case LinkState::Searching: return "SCAN";
        case LinkState::Up:        return "UP";
    }
    return "?";
}

static const char *linkStateWord(LinkState s) {
    switch (s) {
        case LinkState::Down:      return "LINK DOWN";
        case LinkState::Searching: return "SEARCHING FOR NETWORK";
        case LinkState::Up:        return "LINK UP";
    }
    return "LINK ?";
}

static uint32_t linkStateColour(LinkState s) {
    switch (s) {
        case LinkState::Down:      return 0xfca5a5;
        case LinkState::Searching: return kTextWarn;
        case LinkState::Up:        return 0x86efac;
    }
    return kTextDim;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void UI::begin(MessageStore &store, SendFn sendFn, void *user) {
    store_  = &store;
    sendFn_ = sendFn;
    user_   = user;
    show(Screen::Inbox);
}

lv_obj_t *UI::makeScreen() {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    stripStyle(scr);
    lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    return scr;
}

lv_obj_t *UI::makeHeader(lv_obj_t *scr, const char *title) {
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), kHeaderH);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(kHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);
    stripStyle(hdr);
    lv_obj_set_style_pad_hor(hdr, 6, LV_PART_MAIN);

    lv_obj_t *t = label(hdr, title, &lv_font_montserrat_20, 0xffffff);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    hdrRight_ = label(hdr, "", &lv_font_montserrat_14, 0xd1fae5);
    lv_obj_align(hdrRight_, LV_ALIGN_RIGHT_MID, 0, 0);
    refreshHeader();
    return hdr;
}

lv_obj_t *UI::makeBody(lv_obj_t *scr) {
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_color(body, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    stripStyle(body);

    // stripStyle() turns scrolling off for the fixed chrome, but a body must
    // scroll: a long wrapped message on the Detail screen overflows its band,
    // and an unscrollable container simply draws the overflow on top of the
    // footer - where it covers BACK and SOS and eats the taps meant for them.
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    return body;
}

// The action bar. Built by the shared chrome, so SOS and BACK exist on every
// screen by construction - no screen can forget them, which matters because
// with the buttons gone there is no out-of-band way to reach either.
lv_obj_t *UI::makeFooter(lv_obj_t *scr) {
    lv_obj_t *foot = lv_obj_create(scr);
    lv_obj_set_size(foot, LV_PCT(100), kFooterH);
    lv_obj_set_style_bg_color(foot, lv_color_hex(kFooterBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, LV_PART_MAIN);
    stripStyle(foot);
    lv_obj_set_style_pad_all(foot, 4, LV_PART_MAIN);

    // ── SOS, always present, always the same place ──────────────────────────
    // One tap sends immediately, with no confirmation step. That is the point
    // of a panic control: the old long-press-OK sent immediately too, and a
    // confirm dialog is exactly what someone under stress cannot deal with.
    // The trade is that a stray tap here transmits - which is why it sits in
    // the corner, is only as large as it needs to be, and announces itself
    // loudly in the footer afterwards.
    lv_obj_t *sos = lv_button_create(foot);
    lv_obj_set_size(sos, 66, UI_MIN_TOUCH_PX);
    lv_obj_align(sos, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(sos, lv_color_hex(kSosBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sos, lv_color_hex(0xef4444),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(sos, 4, LV_PART_MAIN);
    lv_obj_t *sl = label(sos, "SOS", &lv_font_montserrat_20, 0xffffff);
    lv_obj_center(sl);
    lv_obj_add_event_cb(sos, onSosPanic, LV_EVENT_CLICKED, this);

    // ── Back, everywhere except the screen it would return to ───────────────
    if (cur_ != Screen::Inbox) {
        lv_obj_t *back = lv_button_create(foot);
        lv_obj_set_size(back, 74, UI_MIN_TOUCH_PX);
        lv_obj_align(back, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(back, lv_color_hex(kRowAction), LV_PART_MAIN);
        lv_obj_set_style_bg_color(back, lv_color_hex(kPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(back, 4, LV_PART_MAIN);
        lv_obj_t *bl = label(back, "BACK", &lv_font_montserrat_16, 0xffffff);
        lv_obj_center(bl);
        lv_obj_add_event_cb(back, onBackTapped, LV_EVENT_CLICKED, this);
    }

    // Status text between the two controls. Narrow on purpose: it is where
    // the send confirmation lands, and it must never grow into either target.
    footHint_ = label(foot, "", &lv_font_montserrat_14, kTextDim);
    lv_obj_set_width(footHint_, 84);
    lv_label_set_long_mode(footHint_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(footHint_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(footHint_, LV_ALIGN_CENTER, 0, 0);
    if (toastUntil_) lv_label_set_text(footHint_, toastMsg_);

    return foot;
}

void UI::refreshHeader() {
    if (!hdrRight_) return;
    char buf[24];
    const uint8_t unread = store_ ? store_->unreadCount() : 0;
    if (unread) {
        snprintf(buf, sizeof(buf), "%u NEW %s", unread,
                 linkStateShort(linkState_));
    } else {
        snprintf(buf, sizeof(buf), "LINK %s", linkStateShort(linkState_));
    }
    lv_label_set_text(hdrRight_, buf);
}

void UI::show(Screen s) {
    lv_obj_t *old = scr_;

    cur_ = s;
    hdrRight_ = footHint_ = composeTa_ = composeCnt_ = nullptr;
    hdrVitals_ = vitalsHrL_ = vitalsSpo2L_ = vitalsTempL_ = nullptr;
    vitalsStateL_ = vitalsChart_ = nullptr;
    vitalsSer_ = nullptr;

    switch (s) {
        case Screen::Inbox:   buildInbox();   break;
        case Screen::Detail:  buildDetail();  break;
        case Screen::Presets: buildPresets(); break;
        case Screen::Compose: buildCompose(); break;
        case Screen::Vitals:  buildVitals();  break;
        case Screen::Status:  buildStatus();  break;
        case Screen::Sos:     buildSos();     break;
    }

    lv_screen_load(scr_);

    // ASYNC delete, not lv_obj_delete().
    //
    // Every caller of show() is an LVGL event handler on a button that lives
    // inside `old` - tapping BACK is literally "delete the tree containing the
    // widget whose click is still being dispatched". A direct delete frees it
    // mid-dispatch and LVGL keeps walking the freed event list, so navigation
    // silently fails or corrupts the heap. lv_obj_delete_async() defers the
    // free until the event has finished unwinding, which is exactly what
    // LVGL's own docs prescribe for this case.
    if (old && old != scr_) lv_obj_delete_async(old);
}

// ── Inbox ───────────────────────────────────────────────────────────────────

void UI::buildInbox() {
    scr_ = makeScreen();
    makeHeader(scr_, "INBOX");

    // Vitals strip, pinned directly under the header title: compact live
    // HR / SpO2 / temperature, always visible on the home screen without a
    // navigation step (PLAN.md 4.1). Informational only - no touch target,
    // and it scrolls with nothing, by design.
    lv_obj_t *strip = lv_obj_create(scr_);
    lv_obj_set_size(strip, LV_PCT(100), 18);
    lv_obj_set_style_bg_color(strip, lv_color_hex(kFooterBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
    stripStyle(strip);
    hdrVitals_ = label(strip, "", &lv_font_montserrat_14, kTextDim);
    lv_obj_center(hdrVitals_);
    refreshVitalsStrip();

    lv_obj_t *list = lv_list_create(scr_);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_color(list, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 1, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    struct Action { const char *text; uint32_t id; };
    static const Action actions[] = {
        {"SEND PRESET",  kActSend},
        {"COMPOSE TEXT", kActCompose},
        {"VITALS",       kActVitals},
        {"LINK STATUS",  kActStatus},
    };

    for (const Action &a : actions) {
        // The text argument MUST be nullptr, not "": lv_list_add_button only
        // skips creating its label on a NULL pointer. An empty string still
        // builds a label, sets LV_LABEL_LONG_MODE_SCROLL_CIRCULAR on it and
        // gives it flex_grow 1 - an invisible label that absorbs the row and
        // shoves the real content out of place.
        lv_obj_t *btn = lv_list_add_button(list, nullptr, nullptr);
        lv_obj_set_size(btn, LV_PCT(100), UI_MIN_TOUCH_PX);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kRowAction), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        stripStyle(btn);
        lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);

        lv_obj_t *l = label(btn, a.text, &lv_font_montserrat_16, 0xffffff);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_add_event_cb(btn, onActionClicked, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(a.id));
    }

    const uint32_t now = lv_tick_get();
    const uint8_t n = store_ ? store_->count() : 0;
    const uint8_t shown = n < kInboxMaxRows ? n : kInboxMaxRows;

    for (uint8_t i = 0; i < shown; i++) {
        const Message *m = store_->at(i);
        if (!m) break;
        const bool alert = MessageStore::isSos(m->text);

        lv_obj_t *btn = lv_list_add_button(list, nullptr, nullptr);
        lv_obj_set_size(btn, LV_PCT(100), kRowH);
        lv_obj_set_style_bg_color(btn,
            lv_color_hex(alert ? kRowAlert : kRowBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        stripStyle(btn);
        lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);

        char who[24];
        if (m->outgoing) snprintf(who, sizeof(who), "SENT");
        else             snprintf(who, sizeof(who), "node %u%s", m->src,
                                  m->unread ? " *" : "");
        lv_obj_t *w = label(btn, who, &lv_font_montserrat_14,
                            alert ? kTextWarn : kTextBlue);
        lv_obj_align(w, LV_ALIGN_TOP_LEFT, 0, 3);

        char age[12];
        formatAge(age, sizeof(age), m->rxMillis, now);
        lv_obj_t *a = label(btn, age, &lv_font_montserrat_14, kTextDim);
        lv_obj_align(a, LV_ALIGN_TOP_RIGHT, 0, 3);

        // A full 32-character message does not fit one 240 px line, so the
        // list truncates with an ellipsis and the Detail screen wraps it.
        // This is the cost of portrait, taken deliberately (PLAN.md 2.1).
        lv_obj_t *t = label(btn, m->text, &lv_font_montserrat_16, 0xffffff);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 0, -4);

        lv_obj_add_event_cb(btn, onRowClicked, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(
                                      static_cast<uintptr_t>(i)));
    }

    if (n == 0) {
        lv_obj_t *empty = label(list, "no messages yet",
                                &lv_font_montserrat_14, kTextDim);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 10, LV_PART_MAIN);
    }

    makeFooter(scr_);
}

// ── Detail ──────────────────────────────────────────────────────────────────

void UI::buildDetail() {
    scr_ = makeScreen();
    makeHeader(scr_, "MESSAGE");
    lv_obj_t *body = makeBody(scr_);
    lv_obj_set_style_pad_all(body, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, LV_PART_MAIN);

    const Message *m = store_ ? store_->at(detailIdx_) : nullptr;
    if (!m) {
        label(body, "message is gone", &lv_font_montserrat_16, kTextDim);
        makeFooter(scr_);
        return;
    }

    char meta[64];
    if (m->outgoing) {
        snprintf(meta, sizeof(meta), "SENT  seq %u", m->seq);
    } else {
        snprintf(meta, sizeof(meta), "node %u  seq %u", m->src, m->seq);
    }
    label(body, meta, &lv_font_montserrat_14, kTextBlue);

    // Full text, wrapped. This is the screen that makes portrait's narrower
    // line acceptable: nothing is ever truncated here.
    lv_obj_t *t = label(body, m->text, &lv_font_montserrat_20, 0xffffff);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);

    if (!m->outgoing) {
        char radio[64];
        char age[12];
        formatAge(age, sizeof(age), m->rxMillis, lv_tick_get());
        snprintf(radio, sizeof(radio), "RSSI %d dBm   SNR %d dB   %s ago",
                 m->rssi, m->snr, age);
        label(body, radio, &lv_font_montserrat_14, kTextDim);
    }

    lv_obj_t *reply = lv_button_create(body);
    lv_obj_set_size(reply, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(reply, lv_color_hex(kRowAction), LV_PART_MAIN);
    lv_obj_set_style_radius(reply, 4, LV_PART_MAIN);
    lv_obj_t *rl = label(reply, "QUICK REPLY", &lv_font_montserrat_16, 0xffffff);
    lv_obj_center(rl);
    lv_obj_add_event_cb(reply, onActionClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(reply, reinterpret_cast<void *>(kActSend));

    makeFooter(scr_);
}

// ── Presets ─────────────────────────────────────────────────────────────────

void UI::buildPresets() {
    scr_ = makeScreen();
    makeHeader(scr_, "PRESETS");

    // A scrollable list, not an lv_buttonmatrix.
    //
    // The matrix was the obvious choice and was wrong: eight presets sharing
    // a 240 px body gives 30 px per row, under the UI_MIN_TOUCH_PX floor, and
    // a matrix cannot scroll to buy itself more room. A list of full-height
    // buttons scrolls, so every preset gets its full target size no matter how
    // many there are - which also means adding a ninth preset later cannot
    // silently shrink the other eight below the floor.
    lv_obj_t *list = lv_list_create(scr_);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_color(list, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 2, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (uint8_t i = 0; i < kPresetCount; i++) {
        lv_obj_t *btn = lv_list_add_button(list, nullptr, nullptr);
        lv_obj_set_size(btn, LV_PCT(100), kPresetRowH);
        stripStyle(btn);
        lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);

        // The SOS preset is visually distinct: reaching for it under stress
        // must not require reading the label.
        const bool isSos = (i == PRESET_SOS_INDEX);
        lv_obj_set_style_bg_color(btn,
            lv_color_hex(isSos ? kSosBg : kRowBg), LV_PART_MAIN);

        lv_obj_t *l = label(btn, kPresetMessages[i],
                            &lv_font_montserrat_16, 0xffffff);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_add_event_cb(btn, onPresetClicked, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(
                                      static_cast<uintptr_t>(i)));
    }

    makeFooter(scr_);
}

// ── Compose ─────────────────────────────────────────────────────────────────

void UI::buildCompose() {
    scr_ = makeScreen();
    makeHeader(scr_, "COMPOSE");
    lv_obj_t *body = makeBody(scr_);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 2, LV_PART_MAIN);

    composeTa_ = lv_textarea_create(body);
    lv_obj_set_width(composeTa_, LV_PCT(100));
    lv_obj_set_height(composeTa_, 52);
    lv_textarea_set_one_line(composeTa_, false);
    lv_textarea_set_placeholder_text(composeTa_, "message");
    // The radio drops everything past MSG_MAX_LEN without complaint, so the
    // limit is enforced at the point of entry rather than discovered on air.
    // PLAN.md risk R2.
    lv_textarea_set_max_length(composeTa_, MSG_MAX_LEN);
    lv_obj_set_style_text_font(composeTa_, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_add_event_cb(composeTa_, onComposeChanged, LV_EVENT_VALUE_CHANGED, this);

    composeCnt_ = label(body, "0/32", &lv_font_montserrat_14, kTextDim);

    lv_obj_t *kb = lv_keyboard_create(body);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_keyboard_set_textarea(kb, composeTa_);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);

    // The one deliberate exception to UI_MIN_TOUCH_PX.
    //
    // A 10-column QWERTY across 240 px gives 24 px keys and no arrangement
    // fixes that while it is still a keyboard. Popovers are the mitigation:
    // the pressed key is drawn enlarged above the fingertip, so what is about
    // to be typed is visible even though the finger covers the key itself.
    // This is also precisely why the presets are the primary send path and
    // free text is the fallback, not the other way round.
    lv_keyboard_set_popovers(kb, true);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, onComposeSend, LV_EVENT_READY, this);

    makeFooter(scr_);
}

// ── Vitals ──────────────────────────────────────────────────────────────────

// One value tile: a big number the refresh loop rewrites, a unit caption.
static lv_obj_t *vitalTile(lv_obj_t *parent, const char *unit) {
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_size(t, LV_PCT(33), LV_PCT(100));
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_style_bg_color(t, lv_color_hex(kRowBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(t, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *big = label(t, "--", &lv_font_montserrat_28, 0xffffff);
    label(t, unit, &lv_font_montserrat_14, kTextDim);
    return big;
}

void UI::buildVitals() {
    scr_ = makeScreen();
    makeHeader(scr_, "VITALS");
    lv_obj_t *body = makeBody(scr_);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 6, LV_PART_MAIN);

    // Three equal value tiles: HR, SpO2, temperature.
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_size(row, LV_PCT(100), 68);
    stripStyle(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);

    vitalsHrL_   = vitalTile(row, "bpm");
    vitalsSpo2L_ = vitalTile(row, "% SpO2");
    vitalsTempL_ = vitalTile(row, "deg C");

    // Signal state. This is where the quality gate speaks: a value is shown
    // only when HealthCore's gate passed, and this line says why otherwise.
    vitalsStateL_ = label(body, "", &lv_font_montserrat_14, kTextWarn);
    lv_obj_set_width(vitalsStateL_, LV_PCT(100));
    lv_label_set_long_mode(vitalsStateL_, LV_LABEL_LONG_MODE_WRAP);

    // PPG sparkline: the most recent IR waveform, 0..100 normalised.
    vitalsChart_ = lv_chart_create(body);
    lv_obj_set_size(vitalsChart_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(vitalsChart_, 1);
    lv_obj_set_style_bg_color(vitalsChart_, lv_color_hex(kRowBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vitalsChart_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(vitalsChart_, 6, LV_PART_MAIN);
    lv_chart_set_type(vitalsChart_, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(vitalsChart_, kVitalsWaveMax);
    lv_chart_set_range(vitalsChart_, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(vitalsChart_, 0, 2);
    vitalsSer_ = lv_chart_add_series(vitalsChart_, lv_color_hex(0x22d3ee),
                                     LV_CHART_AXIS_PRIMARY_Y);

    refreshVitalsScreen();
    makeFooter(scr_);
}

// ── Status ──────────────────────────────────────────────────────────────────

void UI::buildStatus() {
    scr_ = makeScreen();
    makeHeader(scr_, "STATUS");
    lv_obj_t *body = makeBody(scr_);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 5, LV_PART_MAIN);

    char buf[64];

    snprintf(buf, sizeof(buf), "link ......... %s", linkStateWord(linkState_));
    label(body, buf, &lv_font_montserrat_16, linkStateColour(linkState_));

    snprintf(buf, sizeof(buf), "messages ..... %u  (%u unread)",
             store_ ? store_->count() : 0, store_ ? store_->unreadCount() : 0);
    label(body, buf, &lv_font_montserrat_14, kTextDim);

    if (hasTx_) snprintf(buf, sizeof(buf), "last TX seq .. %u", lastTxSeq_);
    else        snprintf(buf, sizeof(buf), "last TX seq .. none yet");
    label(body, buf, &lv_font_montserrat_14, kTextDim);

    snprintf(buf, sizeof(buf), "lines/events . %lu / %lu",
             (unsigned long)pLines_, (unsigned long)pEvents_);
    label(body, buf, &lv_font_montserrat_14, kTextDim);

    // A non-zero overrun count means the nRF is emitting lines longer than
    // NRF_LINE_MAX; they are dropped whole rather than half-parsed, so this
    // is the only place it is visible.
    snprintf(buf, sizeof(buf), "overruns ..... %lu", (unsigned long)pOverruns_);
    label(body, buf, &lv_font_montserrat_14,
          pOverruns_ ? kTextWarn : kTextDim);

    snprintf(buf, sizeof(buf), "TX queue ..... %u / %u", qDepth_, qCap_);
    label(body, buf, &lv_font_montserrat_14, kTextDim);

    lv_obj_t *bar = lv_bar_create(body);
    lv_obj_set_size(bar, LV_PCT(100), 8);
    lv_bar_set_range(bar, 0, qCap_ ? qCap_ : 1);
    lv_bar_set_value(bar, qDepth_, LV_ANIM_OFF);

    makeFooter(scr_);
}

// ── SOS takeover ────────────────────────────────────────────────────────────

void UI::buildSos() {
    scr_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_, lv_color_hex(kSosBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_, LV_OPA_COVER, LV_PART_MAIN);
    stripStyle(scr_);
    lv_obj_set_flex_flow(scr_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr_, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr_, 10, LV_PART_MAIN);

    // A dedicated screen rather than lv_msgbox: PLAN.md calls this a
    // "full-screen takeover that must be acknowledged", and a screen models
    // exactly that. A msgbox is a modal layered over whatever was there,
    // which would leave the inbox visible behind it and still needs its own
    // dismissal plumbing.
    lv_obj_t *t = label(scr_, "SOS", &lv_font_montserrat_28, 0xffffff);
    lv_obj_set_style_pad_top(t, 20, LV_PART_MAIN);

    const Message *m = store_ ? store_->at(0) : nullptr;

    char from[32];
    snprintf(from, sizeof(from), "from node %u", m ? m->src : 0);
    label(scr_, from, &lv_font_montserrat_20, 0xfecaca);

    lv_obj_t *body = label(scr_, m ? m->text : "", &lv_font_montserrat_20,
                           0xffffff);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *ack = lv_button_create(scr_);
    lv_obj_set_size(ack, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(ack, lv_color_hex(0x7f1d1d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ack, lv_color_hex(0xef4444),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_t *al = label(ack, "ACKNOWLEDGE", &lv_font_montserrat_20, 0xffffff);
    lv_obj_center(al);
    lv_obj_add_event_cb(ack, onSosAck, LV_EVENT_CLICKED, this);

    // Send our own SOS without acknowledging theirs first. This screen was
    // the one place the "SOS on every screen" rule did not hold, and it is
    // the worst place to lose it: an incoming emergency is exactly when the
    // operator is most likely to need to raise one of their own.
    lv_obj_t *mine = lv_button_create(scr_);
    lv_obj_set_size(mine, LV_PCT(100), UI_MIN_TOUCH_PX);
    lv_obj_set_style_bg_color(mine, lv_color_hex(0x7f1d1d), LV_PART_MAIN);
    lv_obj_set_style_border_width(mine, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(mine, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_t *ml = label(mine, "SEND MY SOS", &lv_font_montserrat_16, 0xffffff);
    lv_obj_center(ml);
    lv_obj_add_event_cb(mine, onSosPanic, LV_EVENT_CLICKED, this);
}

// ── Events ──────────────────────────────────────────────────────────────────

void UI::onRowClicked(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    lv_obj_t *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const uintptr_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));

    ui->detailIdx_ = static_cast<uint8_t>(idx);
    if (ui->store_) ui->store_->markRead(ui->detailIdx_);
    ui->show(Screen::Detail);
}

void UI::onActionClicked(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    lv_obj_t *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const uintptr_t id = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));

    switch (id) {
        case kActSend:    ui->show(Screen::Presets); break;
        case kActCompose: ui->show(Screen::Compose); break;
        case kActStatus:  ui->show(Screen::Status);  break;
        case kActVitals:  ui->show(Screen::Vitals);  break;
        default: break;
    }
}

void UI::onPresetClicked(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    lv_obj_t *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const uintptr_t i = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn));

    if (i < kPresetCount) ui->trySend(kPresetMessages[i], lv_tick_get());
}

void UI::onComposeChanged(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    if (!ui->composeTa_ || !ui->composeCnt_) return;

    const char *txt = lv_textarea_get_text(ui->composeTa_);
    char buf[16];
    const unsigned n = txt ? (unsigned)strlen(txt) : 0;
    snprintf(buf, sizeof(buf), "%u/%u", n, (unsigned)MSG_MAX_LEN);
    lv_label_set_text(ui->composeCnt_, buf);
    lv_obj_set_style_text_color(ui->composeCnt_,
        lv_color_hex(n >= MSG_MAX_LEN ? kTextWarn : kTextDim), LV_PART_MAIN);
}

void UI::onComposeSend(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    if (!ui->composeTa_) return;
    const char *txt = lv_textarea_get_text(ui->composeTa_);
    if (txt && txt[0]) ui->trySend(txt, lv_tick_get());
}

// The persistent panic control.
void UI::onSosPanic(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    const uint32_t now = lv_tick_get();
    ui->noteActivity(now);
    ui->sendPanicSos(now);
}

void UI::onBackTapped(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    ui->noteActivity(lv_tick_get());
    ui->show(Screen::Inbox);
}

void UI::onSosAck(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    if (ui->store_) ui->store_->markRead(0);
    ui->show(Screen::Inbox);
}

// ── Sending ─────────────────────────────────────────────────────────────────

void UI::trySend(const char *text, uint32_t nowMs) {
    if (!text || !text[0]) return;

    const bool ok = sendFn_ ? sendFn_(text, user_) : false;

    // "sent", never "delivered". The mesh is fire-and-forget: +TX confirms
    // transmission only, and claiming delivery would be a lie the operator
    // might act on. PLAN.md risk R7.
    toast(ok ? "queued to send" : "SEND FAILED - queue full", nowMs);
    if (ok) show(Screen::Inbox);
}

void UI::sendPanicSos(uint32_t nowMs) {
    trySend(kPresetMessages[PRESET_SOS_INDEX], nowMs);
}

void UI::toast(const char *msg, uint32_t nowMs) {
    snprintf(toastMsg_, sizeof(toastMsg_), "%s", msg);
    toastUntil_ = nowMs + 3000;
    if (footHint_) lv_label_set_text(footHint_, toastMsg_);
}

// ── Periodic ────────────────────────────────────────────────────────────────

void UI::onMessageArrived(const Message &m, uint32_t nowMs) {
    // An incoming SOS takes the screen over and must be acknowledged.
    if (!m.outgoing && MessageStore::isSos(m.text)) {
        noteActivity(nowMs);
        applyBlank(false);
        show(Screen::Sos);
        return;
    }

    // Anywhere else, refreshing under the operator's fingers would move the
    // row they are about to press. Only the inbox rebuilds.
    if (cur_ == Screen::Inbox) show(Screen::Inbox);
    else refreshHeader();
}

void UI::setLinkState(LinkState s) {
    if (s == linkState_) return;
    linkState_ = s;
    refreshHeader();
}

void UI::setQueue(uint8_t depth, uint8_t capacity) {
    qDepth_ = depth;
    qCap_   = capacity;
}

void UI::setParserStats(uint32_t lines, uint32_t events, uint32_t overruns) {
    pLines_    = lines;
    pEvents_   = events;
    pOverruns_ = overruns;
}

void UI::setVitals(int16_t hr, int16_t spo2, int16_t tempMilliC,
                   bool hrValid, bool spo2Valid, bool tempValid,
                   const int32_t *wave, uint16_t waveLen) {
    vHr_ = hr;
    vSpo2_ = spo2;
    vTempMilliC_ = tempMilliC;
    vHrOk_ = hrValid;
    vSpo2Ok_ = spo2Valid;
    vTempOk_ = tempValid;
    vitalsEver_ = true;

    // Downsample the IR wave into the fixed sparkline buffer, keeping the
    // most recent portion, then normalise to 0..100 for the chart.
    if (wave && waveLen) {
        const uint16_t stride =
            (waveLen + kVitalsWaveMax - 1) / kVitalsWaveMax;
        int32_t tmp[kVitalsWaveMax];
        uint16_t n = 0;
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (uint16_t i = 0; i < waveLen && n < kVitalsWaveMax; i += stride) {
            const int32_t v = wave[i];
            tmp[n++] = v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        const int32_t span = hi - lo;
        for (uint16_t i = 0; i < n; i++) {
            int32_t v = span > 0 ? (tmp[i] - lo) * 100 / span : 50;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            vWave_[i] = (int8_t)v;
        }
        vWaveLen_ = n;
    }

    refreshVitalsStrip();
    if (cur_ == Screen::Vitals) refreshVitalsScreen();
}

// The compact strip under the inbox header. Dash-heavy by design: a dash is
// the truth "unknown", where a plausible number would be a lie (risk R9).
void UI::refreshVitalsStrip() {
    if (!hdrVitals_) return;

    char hrb[12], spb[12], tb[12];
    if (vitalsEver_ && vHrOk_)   snprintf(hrb, sizeof(hrb), "%d bpm", vHr_);
    else                         snprintf(hrb, sizeof(hrb), "-- bpm");
    if (vitalsEver_ && vSpo2Ok_) snprintf(spb, sizeof(spb), "%d%%", vSpo2_);
    else                         snprintf(spb, sizeof(spb), "--%%");
    if (vitalsEver_ && vTempOk_) snprintf(tb, sizeof(tb), "%.1f", vTempMilliC_ / 1000.0);
    else                         snprintf(tb, sizeof(tb), "--.-");

    char buf[56];
    snprintf(buf, sizeof(buf), "HR %s   SpO2 %s   %s C", hrb, spb, tb);
    lv_label_set_text(hdrVitals_, buf);
    lv_obj_set_style_text_color(hdrVitals_,
        lv_color_hex(vHrOk_ && vSpo2Ok_ ? 0x86efac : kTextDim), LV_PART_MAIN);
}

void UI::refreshVitalsScreen() {
    char b[16];

    if (vitalsHrL_) {
        if (vitalsEver_ && vHrOk_) snprintf(b, sizeof(b), "%d", vHr_);
        else                       snprintf(b, sizeof(b), "--");
        lv_label_set_text(vitalsHrL_, b);
    }
    if (vitalsSpo2L_) {
        if (vitalsEver_ && vSpo2Ok_) snprintf(b, sizeof(b), "%d", vSpo2_);
        else                         snprintf(b, sizeof(b), "--");
        lv_label_set_text(vitalsSpo2L_, b);
    }
    if (vitalsTempL_) {
        if (vitalsEver_ && vTempOk_) snprintf(b, sizeof(b), "%.1f", vTempMilliC_ / 1000.0);
        else                         snprintf(b, sizeof(b), "--.-");
        lv_label_set_text(vitalsTempL_, b);
    }

    if (vitalsStateL_) {
        const char *txt;
        uint32_t colour;
        if (!vitalsEver_) {
            txt = "sensor not connected";
            colour = kTextDim;
        } else if (!vHrOk_ && !vSpo2Ok_) {
            txt = "FINGER OFF - place finger on the sensor";
            colour = kTextWarn;
        } else if (vHrOk_ && vSpo2Ok_) {
            txt = "signal OK";
            colour = 0x86efac;
        } else {
            txt = "measuring...";
            colour = kTextWarn;
        }
        lv_label_set_text(vitalsStateL_, txt);
        lv_obj_set_style_text_color(vitalsStateL_, lv_color_hex(colour),
                                    LV_PART_MAIN);
    }

    if (vitalsChart_ && vitalsSer_) {
        for (uint16_t i = 0; i < kVitalsWaveMax; i++) {
            lv_chart_set_value_by_id(vitalsChart_, vitalsSer_, i,
                                     i < vWaveLen_ ? vWave_[i] : 50);
        }
        lv_chart_refresh(vitalsChart_);
    }
}

void UI::applyBlank(bool on) {
    if (on == blanked_) return;
    blanked_ = on;

    // While blanked the panel keeps being read, but presses are swallowed, so
    // the wake tap cannot also activate whatever it landed on. With no buttons
    // left this is the only way to wake, so it must not be lossy.
    lvglPortSetTouchSwallow(on);
    // No backlight control exists on this shield (it is hardwired to the
    // shield's own 3.3 V rail), so "blank" means painting the panel black.
    // That saves no power here, but it does stop a lit screen giving away a
    // position at night. PLAN.md section 2.
    if (scr_) {
        lv_obj_set_style_opa(scr_, on ? LV_OPA_TRANSP : LV_OPA_COVER,
                             LV_PART_MAIN);
        lv_obj_invalidate(scr_);
    }
}

void UI::tick(uint32_t nowMs) {
    // Touch is the only evidence of a user now, so the idle timer is driven
    // straight off the panel rather than off a button event.
    const uint32_t lastTouch = lvglPortLastTouchMs();
    if (lastTouch > lastActivity_) {
        lastActivity_ = lastTouch;
        if (blanked_) applyBlank(false);
    }

    if (toastUntil_ && nowMs >= toastUntil_) {
        toastUntil_ = 0;
        toastMsg_[0] = '\0';
        if (footHint_) lv_label_set_text(footHint_, "");
    }

    if (!blanked_ && SCREEN_BLANK_AFTER_MS > 0 &&
        nowMs - lastActivity_ > SCREEN_BLANK_AFTER_MS) {
        applyBlank(true);
    }
}
