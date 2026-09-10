#pragma once
//
// The LVGL screens for the LoRa Messenger Terminal, laid out for the panel's
// native 240x320 portrait (PLAN.md section 2.1).
//
// This layer never touches TFT_eSPI and never learns the panel is on a
// parallel bus - lib/lvgl_port owns all of that. It also never touches the
// radio: it asks for a send through a callback, so Stage 5 can drive the
// entire UI from a fake message generator with no nRF attached at all.
//
// ── Touch is the only input ─────────────────────────────────────────────────
// One LV_INDEV_TYPE_POINTER, no keypad, no lv_group_t: a tap addresses a
// widget directly, so focus never has to be moved to it first. Three rules
// follow, because there is no second way in when one of them fails
// (PLAN.md section 4.1):
//
//   1. every tappable target is at least UI_MIN_TOUCH_PX on its side
//   2. SOS is one tap from every screen - hence the footer action bar, which
//      is part of the shared chrome rather than something each screen builds
//   3. an uncalibrated panel must never reach this class at all; the caller
//      runs TouchCalUI::run() first (risk R4d)
//
// ── Screen lifetime ─────────────────────────────────────────────────────────
// Exactly one screen exists at a time: show() builds the new one, loads it,
// then deletes the old. Building all six up front would be simpler but does
// not fit - a 32-row inbox alone would outgrow LV_MEM_SIZE (Stage 1c measured
// 27% of 48 KB for seven rows). Rebuilding costs a few milliseconds and
// guarantees the screen always reflects current data.

#include <lvgl.h>

#include "LoraLink.h"
#include "MessageStore.h"
#include "app_config.h"

enum class Screen : uint8_t {
    Inbox = 0,   // home: received messages, newest first
    Detail,      // one message full-screen + metadata
    Presets,     // canned messages - the primary send path
    Compose,     // free text, hard 32-char limit
    Vitals,      // HR / SpO2 / temperature + PPG sparkline (read-only)
    Status,      // link health, counters, queue depth
    Sos,         // full-screen takeover on an incoming SOS
};

// The radio link's three states (PLAN.md 4.1a). DOWN is reserved for a
// genuinely silent UART; SEARCHING means the wire is alive (beacons arrive)
// but no mesh traffic has been seen; UP means traffic flows.
enum class LinkState : uint8_t {
    Down = 0,
    Searching,
    Up,
};

class UI {
public:
    // Returns false if the send could not be queued, which the UI reports
    // rather than swallowing - a message that silently never went is the
    // worst failure available on this device.
    using SendFn = bool (*)(const char *text, void *user);

    // lvglPortInit() and lvglPortInitTouch() must already have succeeded.
    void begin(MessageStore &store, SendFn sendFn, void *user);

    // Pump periodic UI work: relative timestamps, the transient toast, and
    // the idle blank timer. Cheap; call it every loop().
    void tick(uint32_t nowMs);

    void show(Screen s);
    Screen current() const { return cur_; }

    // A message just landed in the store. Refreshes the inbox and, if it is
    // an SOS, takes the screen over.
    void onMessageArrived(const Message &m, uint32_t nowMs);

    // Link telemetry for the header indicator and the Status screen. The
    // caller decides the state from the pair (UART alive, mesh traffic seen)
    // described in PLAN.md 4.1a; the UI only renders it.
    void setLinkState(LinkState s);
    void setQueue(uint8_t depth, uint8_t capacity);
    void setParserStats(uint32_t lines, uint32_t events, uint32_t overruns);
    void setLastTxSeq(uint16_t seq) { lastTxSeq_ = seq; hasTx_ = true; }

    // Vitals from the health side, ~1 Hz. Invalid flags carry the decision
    // made by HealthCore's gates - the UI never invents a number, it renders
    // a dash and the FINGER OFF / sensor-missing state instead (risk R9).
    // `wave` is the IR waveform, oldest first; the UI downsamples it into a
    // fixed sparkline buffer, so any length is accepted.
    void setVitals(int16_t hr, int16_t spo2, int16_t tempMilliC,
                   bool hrValid, bool spo2Valid, bool tempValid,
                   const int32_t *wave, uint16_t waveLen);

    // Send the panic preset immediately. Bound to the persistent SOS control
    // in the footer, which is on every screen.
    void sendPanicSos(uint32_t nowMs);

    // Any input at all - used to un-blank the idle screen.
    void noteActivity(uint32_t nowMs) { lastActivity_ = nowMs; }
    bool blanked() const { return blanked_; }

private:
    // Screen builders. Each creates a fresh screen object.
    void buildInbox();
    void buildDetail();
    void buildPresets();
    void buildCompose();
    void buildVitals();
    void buildStatus();
    void buildSos();

    // Shared chrome, so every screen has the same header/footer geometry.
    lv_obj_t *makeScreen();
    lv_obj_t *makeHeader(lv_obj_t *scr, const char *title);
    lv_obj_t *makeBody(lv_obj_t *scr);

    // The footer is an action bar, not a caption: it carries the persistent
    // SOS control and the back control, so both are present on every screen
    // by construction rather than by each screen remembering to add them.
    lv_obj_t *makeFooter(lv_obj_t *scr);

    void trySend(const char *text, uint32_t nowMs);
    void toast(const char *msg, uint32_t nowMs);
    void refreshVitalsStrip();
    void refreshVitalsScreen();
    void refreshHeader();
    void applyBlank(bool on);

    // LVGL event trampolines. LVGL is C, so these are statics that recover
    // `this` from the event's user data.
    static void onRowClicked(lv_event_t *e);
    static void onActionClicked(lv_event_t *e);
    static void onPresetClicked(lv_event_t *e);
    static void onComposeSend(lv_event_t *e);
    static void onComposeChanged(lv_event_t *e);
    static void onSosAck(lv_event_t *e);
    static void onSosPanic(lv_event_t *e);
    static void onBackTapped(lv_event_t *e);

    MessageStore *store_  = nullptr;
    SendFn        sendFn_ = nullptr;
    void         *user_   = nullptr;

    Screen    cur_ = Screen::Inbox;
    lv_obj_t *scr_ = nullptr;

    // Widgets the tick loop updates in place, rather than rebuilding.
    lv_obj_t *hdrRight_  = nullptr;
    lv_obj_t *footHint_  = nullptr;
    lv_obj_t *composeTa_ = nullptr;
    lv_obj_t *composeCnt_ = nullptr;

    uint8_t  detailIdx_ = 0;      // which message the Detail screen shows
    LinkState linkState_ = LinkState::Down;
    uint8_t  qDepth_    = 0;
    uint8_t  qCap_      = NRF_TXQ_LEN;
    uint32_t pLines_ = 0, pEvents_ = 0, pOverruns_ = 0;
    uint16_t lastTxSeq_ = 0;
    bool     hasTx_     = false;

    // Vitals, as delivered by the health side. Values are only meaningful
    // when their valid flag is set.
    static constexpr uint16_t kVitalsWaveMax = 120;
    int16_t vHr_ = -1, vSpo2_ = -1, vTempMilliC_ = 0;
    bool    vHrOk_ = false, vSpo2Ok_ = false, vTempOk_ = false;
    bool    vitalsEver_ = false;
    int8_t  vWave_[kVitalsWaveMax] = {0};
    uint16_t vWaveLen_ = 0;

    // Vitals widgets, rebuilt with their screen.
    lv_obj_t          *hdrVitals_  = nullptr;   // inbox strip label
    lv_obj_t          *vitalsHrL_  = nullptr;
    lv_obj_t          *vitalsSpo2L_ = nullptr;
    lv_obj_t          *vitalsTempL_ = nullptr;
    lv_obj_t          *vitalsStateL_ = nullptr;
    lv_obj_t          *vitalsChart_ = nullptr;
    lv_chart_series_t *vitalsSer_  = nullptr;

    char     toastMsg_[32] = {0};
    uint32_t toastUntil_   = 0;

    uint32_t lastActivity_ = 0;
    bool     blanked_      = false;
};
