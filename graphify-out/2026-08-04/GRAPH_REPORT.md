# Graph Report - .  (2026-08-04)

## Corpus Check
- Corpus is ~33,043 words - fits in a single context window. You may not need a graph.

## Summary
- 422 nodes · 860 edges · 22 communities (15 shown, 7 thin omitted)
- Extraction: 91% EXTRACTED · 8% INFERRED · 0% AMBIGUOUS · INFERRED: 73 edges (avg confidence: 0.83)
- Token cost: 310,951 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_nRF Link Protocol and UART Stage|nRF Link Protocol and UART Stage]]
- [[_COMMUNITY_UI Screens and Message Data|UI Screens and Message Data]]
- [[_COMMUNITY_LVGL Port and Display Config|LVGL Port and Display Config]]
- [[_COMMUNITY_LoraLink Parser and TX Queue|LoraLink Parser and TX Queue]]
- [[_COMMUNITY_Touch Hardware and Calibration Storage|Touch Hardware and Calibration Storage]]
- [[_COMMUNITY_Touch-Only Input Risks|Touch-Only Input Risks]]
- [[_COMMUNITY_Architecture and Payload Cap|Architecture and Payload Cap]]
- [[_COMMUNITY_Display Controller Findings|Display Controller Findings]]
- [[_COMMUNITY_Controller Identification (Stage 1a)|Controller Identification (Stage 1a)]]
- [[_COMMUNITY_Touch Interaction Rules|Touch Interaction Rules]]
- [[_COMMUNITY_Build-Time Guards and Memory Budget|Build-Time Guards and Memory Budget]]
- [[_COMMUNITY_Touch Class|Touch Class]]
- [[_COMMUNITY_TouchCal Namespace|TouchCal Namespace]]
- [[_COMMUNITY_LVGL Refresh Period|LVGL Refresh Period]]
- [[_COMMUNITY_LCD Read Strobe Pin|LCD Read Strobe Pin]]
- [[_COMMUNITY_LCD Reset Pin|LCD Reset Pin]]
- [[_COMMUNITY_LCD Write Strobe Pin|LCD Write Strobe Pin]]
- [[_COMMUNITY_Stage 0 Scaffolding|Stage 0 Scaffolding]]

## God Nodes (most connected - your core abstractions)
1. `show()` - 18 edges
2. `buildInbox()` - 16 edges
3. `feedStr()` - 16 edges
4. `readRaw()` - 15 edges
5. `makeFooter()` - 14 edges
6. `setup()` - 14 edges
7. `buildPresets()` - 13 edges
8. `label()` - 12 edges
9. `lvglPortInit()` - 12 edges
10. `Path B — Machine-Readable +RX/+TX Line` - 12 edges

## Surprising Connections (you probably didn't know these)
- `seen_check_and_add()` --semantically_similar_to--> `Beacon Filtering Into the Status Screen`  [INFERRED] [semantically similar]
  reference/nrf.cpp → PLAN.md
- `test_sent_messages_are_not_unread()` --conceptually_related_to--> `Path B — Machine-Readable +RX/+TX Line`  [INFERRED]
  test/test_logic_store/test_store.cpp → PLAN.md
- `onLinkEvent()` --conceptually_related_to--> `Inbox Screen (home)`  [INFERRED]
  src/main.cpp → PLAN.md
- `mockGenerator()` --semantically_similar_to--> `onLinkEvent()`  [INFERRED] [semantically similar]
  test_apps/t5_ui.cpp → src/main.cpp
- `realSend()` --implements--> `ESP32 to nRF Outgoing Line Protocol`  [INFERRED]
  src/main.cpp → PLAN.md

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Touch panel and LCD share four GPIOs, so every read must save and restore the bus** — include_pins_pin_touch_xp, include_pins_pin_touch_xm, include_pins_pin_touch_yp, include_pins_pin_touch_ym, touch_touch_readraw, touch_touch_restorelcdbus, lvgl_port_lvgl_port_flushcb, include_pins_touch_lcd_pin_sharing [EXTRACTED 1.00]
- **One-tap SOS panic path, present on every screen** — include_app_config_kpresetmessages, include_app_config_preset_sos_index, ui_ui_makefooter, ui_ui_onsospanic, ui_ui_sendpanicsos, ui_ui_trysend, ui_ui_buildsos, messagestore_messagestore_issos, include_pins_button_removal [EXTRACTED 1.00]
- **32-character radio cap enforced at every layer that touches message text** — include_app_config_msg_max_len, loralink_loralink_copycapped, loralink_loralink_send, messagestore_messagestore_copytext, messagestore_messagestore_message, ui_ui_buildcompose, ui_ui_oncomposechanged [EXTRACTED 1.00]
- **Staged Bring-Up Workflow — One PlatformIO Environment Per Stage** — plan_stage_1a, plan_stage_1b, plan_stage_1c, plan_stage_2, plan_stage_3, plan_stage_4, plan_stage_5, plan_stage_6, plan_stage_7 [EXTRACTED 1.00]
- **Touch-Only Input and Its Compensations** — plan_touch_only_input, plan_phantom_press_incident, plan_r4d, plan_r4e, plan_calibration_is_boot_path, plan_sos_everywhere [EXTRACTED 1.00]
- **32-Byte Payload Cap Propagating Through the Whole Stack** — reference_nrf_max_payload_len, plan_max_payload_len_cap, plan_r2, plan_screen_compose, test_logic_link_test_link_test_text_truncated_to_msg_max_len, test_logic_store_test_store_test_all_presets_fit [INFERRED 0.85]

## Communities (22 total, 7 thin omitted)

### Community 0 - "nRF Link Protocol and UART Stage"
Cohesion: 0.07
Nodes (53): Beacon Filtering Into the Status Screen, Path A — Parse the Zephyr Log Line, Path B — Machine-Readable +RX/+TX Line, R1 — nRF Console on USB CDC, R5 — Zephyr Log Lines Dropped in Deferred Mode, R7 — No Delivery Guarantee (Fire-and-Forget Mesh), Stage 4 — UART Link to the nRF (env t4_uart), Required Zephyr-Side Console Configuration (+45 more)

### Community 1 - "UI Screens and Message Data"
Cohesion: 0.10
Nodes (52): kPresetCount, kPresetMessages, MSG_HISTORY_LEN, PRESET_SOS_INDEX, Montserrat 14/16/20/28 font set, Widget trimming to shrink flash and API surface, Message, lv_event_t (+44 more)

### Community 2 - "LVGL Port and Display Config"
Cohesion: 0.07
Nodes (47): SCREEN_BLANK_AFTER_MS (light discipline), LV_COLOR_DEPTH 16 (RGB565 render format), Dark theme (night light discipline), LV_USE_OS LV_OS_NONE (pumped from loop), Backlight hardwired, no control GPIO, TFT_eSPI, Per-call UART drain budget protects lv_timer_handler, lv_area_t (+39 more)

### Community 3 - "LoraLink Parser and TX Queue"
Cohesion: 0.08
Nodes (40): MSG_MAX_LEN (32-char radio payload cap), NRF_LINE_MAX, NRF_LINK_TIMEOUT_MS (3 missed beacons), NRF_TX_GAP_MS (inter-line spacing), NRF_TXQ_LEN (bounded TX queue depth), PIN_NRF_RX (GPIO16), PIN_NRF_TX (GPIO17), LinkEvent (+32 more)

### Community 4 - "Touch Hardware and Calibration Storage"
Cohesion: 0.08
Nodes (44): TOUCH_ADC_MAX, TOUCH_CAL_VERSION (embeds rotation), TOUCH_NVS_KEY, TOUCH_NVS_NAMESPACE, TOUCH_SAMPLES, TOUCH_SETTLE_US, TOUCH_Z_THRESHOLD, ADC2 unusable with WiFi; device never runs WiFi (+36 more)

### Community 5 - "Touch-Only Input Risks"
Cohesion: 0.08
Nodes (35): MessageStore, A Failed Calibration Boots Into Calibration, Floating GPIO34 Fired an Unasked-For Panic SOS, R4 — Touch Shares Four LCD Lines and Has No Controller, R4d — Touch Is a Single Point of Failure, R4e — No Out-of-Band Panic Path, Status Screen, SOS Reachable in One Tap From Every Screen (+27 more)

### Community 6 - "Architecture and Payload Cap"
Cohesion: 0.08
Nodes (26): PLAN.md — LoRa Messenger Terminal Build Plan, Arduino-Free Logic Layer, Division of Labour — ESP32 Owns Nothing Radio-Shaped, 32-Byte Payload Cap (MAX_PAYLOAD_LEN), ESP32 to nRF Outgoing Line Protocol, Presets as a Scrolling List, Not a Button Matrix, R2 — 32-Byte Payload Cap Silently Truncates, Compose Screen (+18 more)

### Community 7 - "Display Controller Findings"
Cohesion: 0.13
Nodes (27): The Pin Assignment Is Forced, Not Chosen, Finding — The Controller Is an ILI9342, Not an ILI9341, Portrait 240x320 Is TFT_ROTATION 1, R4c — ADC2 Unavailable While WiFi Is Active, R6 — 3.3 V Regulator Sag With the TFT at Full Brightness, Stage 1b — TFT Display Bring-Up (env t1_display), Finding — The Panel Is RGB-Ordered, Not BGR, loop() (+19 more)

### Community 8 - "Controller Identification (Stage 1a)"
Cohesion: 0.16
Nodes (26): Only Geometry Identifies a Controller, Stage 1a Finding — This Panel Answers No ID Register, R3 — Unknown LCD Controller, R4b — Shield Regulator Fed From Its 5V Pin, Stage 1a — Identify the LCD Controller (env t1a_lcdid), busBegin(), busIsFloating(), dataAsInput() (+18 more)

### Community 9 - "Touch Interaction Rules"
Cohesion: 0.21
Nodes (13): UI_MIN_TOUCH_PX (40 px touch-target floor), Push buttons removed (floating GPIO34/35 fired a phantom SOS), Touch, lvglPortInitTouch(), One pointer indev, no keypad and no lv_group_t, 18 px scroll limit absorbs resistive-panel jitter, Calibration is a boot path, not a bring-up step (risk R4d), kFooterH (48 px action bar) (+5 more)

### Community 10 - "Build-Time Guards and Memory Budget"
Cohesion: 0.22
Nodes (9): TFT_ROTATION (ILI9342 inverted numbering), LV_DPI_DEF 167, LV_MEM_SIZE (48 KB LVGL pool), pins.h vs platformio.ini drift guards, GPIO < 32 constraint for the parallel bus, static_asserts proving lv_conf.h is in force, LVGL_BUF_LINES (40-scanline strip buffer), kInboxMaxRows (16 rows, bounded by LV_MEM_SIZE) (+1 more)

## Ambiguous Edges - Review These
- `readRaw()` → `TOUCH_Z_THRESHOLD`  [AMBIGUOUS]
  lib/Touch/Touch.cpp · relation: references
- `lvglPortInit()` → `touchReadCb()`  [AMBIGUOUS]
  lib/lvgl_port/lvgl_port.cpp · relation: shares_data_with

## Knowledge Gaps
- **50 isolated node(s):** `class`, `class`, `TouchRaw`, `namespace`, `lv_font_t` (+45 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **7 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `readRaw()` and `TOUCH_Z_THRESHOLD`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `lvglPortInit()` and `touchReadCb()`?**
  _Edge tagged AMBIGUOUS (relation: shares_data_with) - confidence is low._
- **Why does `MessageStore` connect `Touch-Only Input Risks` to `nRF Link Protocol and UART Stage`, `UI Screens and Message Data`, `Architecture and Payload Cap`?**
  _High betweenness centrality (0.221) - this node is a cross-community bridge._
- **Why does `begin()` connect `UI Screens and Message Data` to `Touch-Only Input Risks`?**
  _High betweenness centrality (0.188) - this node is a cross-community bridge._
- **Why does `TouchCal` connect `Touch Hardware and Calibration Storage` to `Touch-Only Input Risks`?**
  _High betweenness centrality (0.105) - this node is a cross-community bridge._
- **What connects `class`, `class`, `TouchRaw` to the rest of the system?**
  _60 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `nRF Link Protocol and UART Stage` be split into smaller, more focused modules?**
  _Cohesion score 0.06896551724137931 - nodes in this community are weakly interconnected._