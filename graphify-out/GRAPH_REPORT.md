# Graph Report - .  (2026-09-22)

## Corpus Check
- 27 files · ~55,663 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 673 nodes · 1315 edges · 50 communities (20 shown, 30 thin omitted)
- Extraction: 93% EXTRACTED · 7% INFERRED · 0% AMBIGUOUS · INFERRED: 91 edges (avg confidence: 0.83)
- Token cost: 0 input · 357,911 output

## Community Hubs (Navigation)
- [[_COMMUNITY_App Config Constants|App Config Constants]]
- [[_COMMUNITY_nRF Link Protocol & Status Screen|nRF Link Protocol & Status Screen]]
- [[_COMMUNITY_Pin Wiring & Boot Diagnostics|Pin Wiring & Boot Diagnostics]]
- [[_COMMUNITY_Display Controller & Pin Budget|Display Controller & Pin Budget]]
- [[_COMMUNITY_Core Classes (MessageStoreUILoraLink)|Core Classes (MessageStore/UI/LoraLink)]]
- [[_COMMUNITY_LVGL & Display Config|LVGL & Display Config]]
- [[_COMMUNITY_Touch Calibration Constants|Touch Calibration Constants]]
- [[_COMMUNITY_LoRa Link Queue & Pins|LoRa Link Queue & Pins]]
- [[_COMMUNITY_Build Plan Document|Build Plan Document]]
- [[_COMMUNITY_Health Core Data Model|Health Core Data Model]]
- [[_COMMUNITY_Touch Diagnostics|Touch Diagnostics]]
- [[_COMMUNITY_Touch Calibration Unit Tests|Touch Calibration Unit Tests]]
- [[_COMMUNITY_UI Screens & Message Protocol|UI Screens & Message Protocol]]
- [[_COMMUNITY_Health Core Unit Tests|Health Core Unit Tests]]
- [[_COMMUNITY_PPG Validity Gating Tests|PPG Validity Gating Tests]]
- [[_COMMUNITY_Shield Power Trap Risk|Shield Power Trap Risk]]
- [[_COMMUNITY_HealthSensor Class|HealthSensor Class]]
- [[_COMMUNITY_MAX30102 Driver|MAX30102 Driver]]
- [[_COMMUNITY_MAX30205 Driver|MAX30205 Driver]]
- [[_COMMUNITY_Touch Sample Count Constant|Touch Sample Count Constant]]
- [[_COMMUNITY_LVGL Refresh Period Constant|LVGL Refresh Period Constant]]
- [[_COMMUNITY_LCD Read Strobe Pin|LCD Read Strobe Pin]]
- [[_COMMUNITY_LCD Reset Pin|LCD Reset Pin]]
- [[_COMMUNITY_LCD Write Strobe Pin|LCD Write Strobe Pin]]
- [[_COMMUNITY_Pin Assignment Rationale|Pin Assignment Rationale]]
- [[_COMMUNITY_UART Integrity Risk|UART Integrity Risk]]
- [[_COMMUNITY_Touch SPOF Risk|Touch SPOF Risk]]
- [[_COMMUNITY_Stage 0 Scaffolding|Stage 0: Scaffolding]]
- [[_COMMUNITY_traceLine Function|traceLine Function]]
- [[_COMMUNITY_PPG Waveform Fixture Generator|PPG Waveform Fixture Generator]]
- [[_COMMUNITY_HR 120bpm Test|HR 120bpm Test]]
- [[_COMMUNITY_HR 180bpm Test|HR 180bpm Test]]
- [[_COMMUNITY_HR 60bpm Noisy Test|HR 60bpm Noisy Test]]
- [[_COMMUNITY_SpO2 Oxygenation Test|SpO2 Oxygenation Test]]
- [[_COMMUNITY_Touch Center Mapping Test|Touch Center Mapping Test]]
- [[_COMMUNITY_Touch Clamp Test|Touch Clamp Test]]
- [[_COMMUNITY_Touch Edge Disagreement Test|Touch Edge Disagreement Test]]
- [[_COMMUNITY_Touch Edge Disagreement Test (Swapped)|Touch Edge Disagreement Test (Swapped)]]
- [[_COMMUNITY_Touch Inverted Range Test|Touch Inverted Range Test]]
- [[_COMMUNITY_Touch Zero-Span Rejection Test|Touch Zero-Span Rejection Test]]
- [[_COMMUNITY_Touch Normal Mapping Test|Touch Normal Mapping Test]]
- [[_COMMUNITY_Touch Swapped+Inverted Test|Touch Swapped+Inverted Test]]
- [[_COMMUNITY_Touch Swapped Axes Test|Touch Swapped Axes Test]]
- [[_COMMUNITY_Touch Zero Inset Test|Touch Zero Inset Test]]
- [[_COMMUNITY_VSCode Extensions Config|VSCode Extensions Config]]

## God Nodes (most connected - your core abstractions)
1. `show()` - 19 edges
2. `lvglPortInit()` - 18 edges
3. `setup()` - 18 edges
4. `buildInbox()` - 17 edges
5. `readRaw()` - 16 edges
6. `makeFooter()` - 16 edges
7. `feedStr()` - 16 edges
8. `solve()` - 16 edges
9. `label()` - 15 edges
10. `loop()` - 15 edges

## Surprising Connections (you probably didn't know these)
- `seen_check_and_add()` --semantically_similar_to--> `Beacon Filtering Into the Status Screen`  [INFERRED] [semantically similar]
  reference/nrf.cpp → PLAN.md
- `loop()` --conceptually_related_to--> `Status Screen`  [INFERRED]
  src/main.cpp → PLAN.md
- `loop()` --implements--> `Stage 8 — Hardening`  [INFERRED]
  src/main.cpp → PLAN.md
- `testReadback()` --conceptually_related_to--> `Risk R4: Touch Shares LCD Lines, No Controller`  [INFERRED]
  test_apps/t1_display.cpp → PLAN.md
- `onLinkEvent()` --conceptually_related_to--> `Inbox Screen (home)`  [INFERRED]
  src/main.cpp → PLAN.md

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Vitals acquisition pipeline (FIFO drain to UI render)** — health_healthsensor_poll, health_healthcore_push, health_healthcore_analyze, health_health_latest, src_main_loop [INFERRED 0.85]
- **Touch panel sharing the LCD parallel bus** — touch_touch_restorelcdbus, include_pins_pin_lcd, include_pins_pin_touch, lvgl_port_lvgl_port_touchreadcb [INFERRED 0.80]
- **Touch calibration capture-and-solve flow** — touch_touch_solve, touch_touch_readraw, touchcal_touchcal_run, touchcal_touchcal_awaitstabletap [INFERRED 0.85]
- **Calibration-as-boot-path pattern (R4d)** — test_apps_t7_touchcheck_ensurecalibrated, test_apps_t8_app_cal_ensurecalibratedandaligned, test_apps_t9_app_rtos_ensurecalibratedandaligned, touchcal_touchcal_touchcalui, plan_risk_r4d [INFERRED 0.85]
- **Three-state link model implementation across apps** — plan_three_state_link_model, test_apps_t8_app_cal_loop, test_apps_t9_app_rtos_uitask, loralink_loralink_loralink [EXTRACTED 1.00]
- **Bus-integrity readback verification across stages** — test_apps_t1_display_testreadback, test_apps_t6_touchdiag_phasebusintegrity, plan_risk_r4 [INFERRED 0.85]

## Communities (50 total, 30 thin omitted)

### Community 0 - "App Config Constants"
Cohesion: 0.06
Nodes (87): Fail-Honest Display Principle, kPresetCount, kPresetMessages, MSG_HISTORY_LEN, NRF_LINK_TIMEOUT_MS (3 missed beacons), PRESET_SOS_INDEX, SCREEN_BLANK_AFTER_MS (light discipline), UI_MIN_TOUCH_PX (40 px touch-target floor) (+79 more)

### Community 1 - "nRF Link Protocol & Status Screen"
Cohesion: 0.07
Nodes (54): Beacon Filtering Into the Status Screen, ESP32 to nRF Outgoing Line Protocol, Path A — Parse the Zephyr Log Line, Path B — Machine-Readable +RX/+TX Line, R1 — nRF Console on USB CDC, R5 — Zephyr Log Lines Dropped in Deferred Mode, Status Screen, Stage 4 — UART Link to the nRF (env t4_uart) (+46 more)

### Community 2 - "Pin Wiring & Boot Diagnostics"
Cohesion: 0.06
Nodes (50): capture_boot.py boot capture script, HealthSensor (I2C transport for MAX30102/MAX30205), I2C health bus pins (SDA/SCL), nRF UART link pins (TX/RX), namespace, A Failed Calibration Boots Into Calibration, Floating GPIO34 Fired an Unasked-For Panic SOS, R4 — Touch Shares Four LCD Lines and Has No Controller (+42 more)

### Community 3 - "Display Controller & Pin Budget"
Cohesion: 0.08
Nodes (53): The Pin Assignment Is Forced, Not Chosen, Only Geometry Identifies a Controller, Finding — The Controller Is an ILI9342, Not an ILI9341, Stage 1a Finding — This Panel Answers No ID Register, Portrait 240x320 Is TFT_ROTATION 1, R3 — Unknown LCD Controller, R4b — Shield Regulator Fed From Its 5V Pin, R4c — ADC2 Unavailable While WiFi Is Active (+45 more)

### Community 4 - "Core Classes (MessageStore/UI/LoraLink)"
Cohesion: 0.07
Nodes (44): class, class, class, LoraLink(), LoraLinkParser(), Parser is Arduino-free so it runs under pio test -e native, Only lvgl_port knows about TFT_eSPI and the parallel bus, MessageStore (+36 more)

### Community 5 - "LVGL & Display Config"
Cohesion: 0.08
Nodes (47): TFT_ROTATION (ILI9342 inverted numbering), LV_COLOR_DEPTH 16 (RGB565 render format), LV_DPI_DEF 167, LV_MEM_SIZE (48 KB LVGL pool), LV_USE_OS LV_OS_NONE (pumped from loop), TFT_eSPI, Touch, lv_area_t (+39 more)

### Community 6 - "Touch Calibration Constants"
Cohesion: 0.08
Nodes (46): TOUCH_ADC_MAX, TOUCH_CAL_VERSION (embeds rotation), TOUCH_NVS_KEY, TOUCH_NVS_NAMESPACE, TOUCH_SETTLE_US, TOUCH_Z_THRESHOLD, ADC2 unusable with WiFi; device never runs WiFi, pins.h vs platformio.ini drift guards (+38 more)

### Community 7 - "LoRa Link Queue & Pins"
Cohesion: 0.10
Nodes (33): MSG_MAX_LEN (32-char radio payload cap), NRF_LINE_MAX, NRF_TX_GAP_MS (inter-line spacing), NRF_TXQ_LEN (bounded TX queue depth), PIN_NRF_RX (GPIO16), PIN_NRF_TX (GPIO17), LinkEvent, Message (+25 more)

### Community 8 - "Build Plan Document"
Cohesion: 0.06
Nodes (35): 1. System overview, 2.1 Orientation, 2.2 Touch, 2. Hardware / pin map, 3.1 ESP32 → nRF (outgoing messages) — works today, no firmware change, 3.2 nRF → ESP32 (incoming messages) — needs a decision, 3.3 Required Zephyr-side configuration, 3. The ESP32 ↔ nRF52840 link protocol (+27 more)

### Community 9 - "Health Core Data Model"
Cohesion: 0.07
Nodes (33): HealthCore(), struct HealthReading, HealthSensor::latest, HealthCore::setTempMilliC, struct VitalPair, analyze(), copyIrWave(), copyWindow() (+25 more)

### Community 10 - "Touch Diagnostics"
Cohesion: 0.10
Nodes (32): class, Risk R4: Touch Shares LCD Lines, No Controller, 40px Minimum Touch Target (UI_MIN_TOUCH_PX), TapResult, banner(), drawTarget(), phaseBusIntegrity(), phaseCalibration() (+24 more)

### Community 11 - "Touch Calibration Unit Tests"
Cohesion: 0.13
Nodes (28): mapToScreen(), solve(), test_center_maps_to_center(), test_clamp_out_of_range(), test_disagreeing_edges_fall_back(), test_disagreeing_edges_fall_back_swapped(), test_inverted_range(), test_large_panel() (+20 more)

### Community 12 - "UI Screens & Message Protocol"
Cohesion: 0.12
Nodes (16): 32-Byte Payload Cap (MAX_PAYLOAD_LEN), Presets as a Scrolling List, Not a Button Matrix, R2 — 32-Byte Payload Cap Silently Truncates, Compose Screen, Detail Screen, Inbox Screen (home), Presets Screen, Stage 6 — Protocol Layer and Native Tests (pio test -e native) (+8 more)

### Community 13 - "Health Core Unit Tests"
Cohesion: 0.26
Nodes (14): HealthCore, feed(), synthPpg(), test_finger_off_is_invalid(), test_hr_120bpm_clean(), test_hr_180bpm_clean(), test_hr_60bpm_clean(), test_hr_60bpm_noisy() (+6 more)

### Community 14 - "PPG Validity Gating Tests"
Cohesion: 0.40
Nodes (5): Risk R9: Motion/Ambient Light Corrupts the PPG Waveform, test_finger_off_is_invalid, test_partial_window_is_invalid, test_saturated_channel_is_invalid, test_stale_data_is_invalid

## Ambiguous Edges - Review These
- `readRaw()` → `TOUCH_Z_THRESHOLD`  [AMBIGUOUS]
  lib/Touch/Touch.cpp · relation: references
- `lvglPortInit()` → `touchReadCb()`  [AMBIGUOUS]
  lib/lvgl_port/lvgl_port.cpp · relation: shares_data_with

## Knowledge Gaps
- **142 isolated node(s):** `class`, `class`, `TouchRaw`, `namespace`, `lv_font_t` (+137 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **30 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `readRaw()` and `TOUCH_Z_THRESHOLD`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `lvglPortInit()` and `touchReadCb()`?**
  _Edge tagged AMBIGUOUS (relation: shares_data_with) - confidence is low._
- **Why does `setup()` connect `Pin Wiring & Boot Diagnostics` to `App Config Constants`, `nRF Link Protocol & Status Screen`, `LVGL & Display Config`, `Touch Calibration Constants`, `Health Core Data Model`?**
  _High betweenness centrality (0.142) - this node is a cross-community bridge._
- **Why does `onLinkEvent()` connect `Pin Wiring & Boot Diagnostics` to `App Config Constants`, `nRF Link Protocol & Status Screen`, `UI Screens & Message Protocol`, `LoRa Link Queue & Pins`?**
  _High betweenness centrality (0.113) - this node is a cross-community bridge._
- **Why does `TouchCal` connect `Pin Wiring & Boot Diagnostics` to `Touch Diagnostics`, `Core Classes (MessageStore/UI/LoraLink)`, `Touch Calibration Constants`?**
  _High betweenness centrality (0.111) - this node is a cross-community bridge._
- **Are the 5 inferred relationships involving `lvglPortInit()` (e.g. with `setup()` and `setup()`) actually correct?**
  _`lvglPortInit()` has 5 INFERRED edges - model-reasoned connections that need verification._
- **What connects `class`, `class`, `TouchRaw` to the rest of the system?**
  _157 weakly-connected nodes found - possible documentation gaps or missing edges._