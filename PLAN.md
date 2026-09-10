# LoRa Messenger Terminal — Build Plan

ESP32 front-end (2.4" ILI9342 resistive touch TFT, touch-only input) that
drives an nRF52840 LoRa mesh node over a hardware UART and monitors the
operator's vitals locally — heart rate and SpO2 from a MAX30102, body
temperature from a MAX30205 — on a shared I2C bus.

---

## 1. System overview

```
   ┌──────────────────────────────┐          ┌───────────────────────────┐
   │  ESP32 DevKit V1 (WROOM-32)  │          │  nRF52840 + SX126x        │
   │                              │          │  (Zephyr, reference/)     │
   │  ┌────────────────────────┐  │          │                           │
   │  │ 2.4" ILI9342 240x320   │  │  UART2   │  ┌─────────────────────┐  │
   │  │ 4-wire resistive touch │  │◄────────►│  │ mesh_tx / handle_rx │  │
   │  │ (no controller)        │  │ 115200   │  │ TTL=3, dup-cache    │  │
   │  └────────────────────────┘  │  8N1     │  └──────────┬──────────┘  │
   │  ┌────────────────────────┐  │          │             │             │
   │  │ MAX30102  HR / SpO2    │  │          │        ((( LoRa )))       │
   │  │ MAX30205  body temp    │  │          │        865.1 MHz SF10     │
   │  │ I2C on GPIO32/33       │  │          │                           │
   │  └────────────────────────┘  │          │                           │
   │   touch is the only user     │          │                           │
   │   input; vitals ride I2C     │          │                           │
   └──────────────────────────────┘          └───────────────────────────┘
        UI + message store                        radio + mesh routing
```

**Division of labour.** The nRF52840 already owns everything radio-shaped:
framing, TTL, duplicate suppression, listen-before-talk, retries. The ESP32
owns *nothing* radio-shaped — it is a terminal. It renders received text,
collects outgoing text, and speaks one line-based protocol over UART. Keeping
that boundary sharp is what makes the ESP32 side unit-testable on a PC.

Vitals monitoring is the opposite: it is entirely terminal-local. The ESP32
owns the MAX30102/MAX30205, runs the HR/SpO2 DSP itself and renders the
results — nothing health-shaped crosses the UART in this revision, so the
radio protocol stays frozen while the health side is built (Stages 4a–4c).

---

## 2. Hardware / pin map

Board: **ESP32 DevKit V1, 30-pin DOIT, WROOM-32.** Every pin below is
confirmed present on the 30-pin header, which unlike the 38-pin does not
break out GPIO0 or GPIO6–11.

Single source of truth: [include/pins.h](include/pins.h). The same numbers are
mirrored into `build_flags` in [platformio.ini](platformio.ini) because
TFT_eSPI reads its configuration at compile time — `static_assert`s in
`pins.h` fail the build if the two ever drift apart.

**Physical header positions and per-module wiring: [WIRING.md](WIRING.md).**

| Function | GPIO | Shield pin | Notes |
|---|---|---|---|
| LCD_D0 | 13 | `D8` | ⚠️ crossover · also touch **XP** |
| LCD_D1 | 14 | `D9` | ⚠️ crossover · also touch **YM** |
| LCD_D2…D7 | 21, 22, 23, 18, 19, 5 | `D2`…`D7` | |
| LCD_RD | 4 | `A0` | read strobe |
| LCD_WR | 27 | `A1` | write strobe |
| LCD_RS | 25 | `A2` | register select · also touch **XM**, ADC2_CH8 |
| LCD_CS | 26 | `A3` | chip select · also touch **YP**, ADC2_CH9 |
| LCD_RST | 15 | `A4` | |
| UART2 TX → nRF RX | 17 | — | |
| UART2 RX ← nRF TX | 16 | — | |
| I2C SDA → MAX30102 + MAX30205 | 32 | — | health bus — see §2.3 |
| I2C SCL → MAX30102 + MAX30205 | 33 | — | health bus — see §2.3 |
| MAX30102 INT | 35 | — | optional, input-only; open-drain → external pull-up |

**The assignment is forced, not chosen.** Three constraints leave almost no
freedom:

1. TFT_eSPI's ESP32 parallel driver writes all eight data lines with one
   32-bit GPIO register store, which cannot reach pins ≥ 32. Every LCD pin is
   therefore below 32 — a `static_assert` in `pins.h` enforces it.
2. The touch panel is wired inside the shield onto `LCD_RS` and `LCD_CS`, and
   both are *sampled as analog voltages*. They must land on ADC pins, which
   is why they are GPIO25/26 (ADC2_CH8/CH9) and not anything else. ADC2 is
   unusable while WiFi runs — free here, since this device never enables it.
3. The health sensors need I2C: two bidirectional pins. The ESP32's default
   `Wire` pins are GPIO21/22, and both are `LCD_D2`/`LCD_D3` here — so the
   bus is constructed explicitly on GPIO32/33, the two clean pins the button
   removal freed. An optional MAX30102 INT line can take GPIO35.

Deliberately avoided: **GPIO6–11** (SPI flash, absent from this header),
**GPIO12** (MTDI strapping — high at boot switches the flash regulator to
1.8 V and the board will not start), **GPIO2** (its onboard LED fights an
input pull-up).

Dropping the push buttons in favour of touch is what freed 32–35, and the
health bus spends the two clean bidirectional pins among them. Free after the
health build: **GPIO2, 34, 36, 39** — none of them clean (2 carries the
onboard LED and strapping, 34 has no pull-up, 36/39 are analog-only), so this
is a one-way door: a future peripheral needing a *reliable* I/O pin will have
to reclaim something. The shield's SD-card slot (`D10`–`D13`) is left
unconnected.

**The backlight costs nothing.** It is hardwired to the shield's own 3.3 V
rail with no control pin on the Uno headers — so brightness is fixed, no GPIO
is spent, and the GPIO-current hazard an SPI module would have posed is gone.
Idle dimming is therefore not available; the idle timeout blanks to a dark
screen instead.

---

### 2.1 Orientation

**Portrait, 240×320** (`TFT_ROTATION 1`). The inbox gets a taller list
(254 px of rows vs 174 in landscape) and the device reads like a handheld
pager rather than lying on its side.

Note the rotation *number*: this panel is an ILI9342, whose native
orientation is landscape 320×240, so portrait is rotation **1** — the
opposite of a real ILI9341. See section 5.

The trade-off, stated plainly so it is designed for rather than discovered:
a full 32-character message — the hard cap the radio imposes — does not fit
on one 240 px line in the list font. The inbox list therefore shows long
messages truncated with an ellipsis, and the Detail screen wraps the full
text; the compose screen's live counter matters all the more because the
sender cannot judge length by eye against a single line.

The consequence to remember: **touch calibration is per-orientation and is not
transferable.** The raw ADC axes stay fixed to the glass while the screen axes
rotate under them. `TOUCH_CAL_VERSION` embeds the rotation, so changing
`TFT_ROTATION` makes a stored calibration fail to load and prompt for a fresh
one, rather than silently mapping taps to the wrong place.

### 2.2 Touch

A bare 4-wire resistive sheet with no controller, wired inside the shield onto
four lines that are also the LCD's. Reading a coordinate means driving two
opposite corners to form a voltage divider across one sheet and measuring
where the other sheet contacts it — so `LCD_D0`, `LCD_D1`, `LCD_RS` and
`LCD_CS` all get reconfigured as analog inputs and outputs mid-flight.

Three things follow, all owned by `lib/Touch/`:

1. **Every read restores the bus before returning** — data lines back to
   outputs, `CS` and `RS` back to idle HIGH. Skip it and the next draw writes
   commands into a display that is not listening, which surfaces later as a
   frozen or garbled screen with nothing pointing at the touch code.
2. **Reads happen between frames, never inside a draw.** Free in practice:
   LVGL polls input devices at the top of `lv_timer_handler()`, before any
   rendering, and both run from `loop()` so there is no concurrency.
3. **All four corners are on ADC2** (GPIO13/14/25/26 = channels 4/6/8/9),
   which is unusable while WiFi is active. Free here — this device never
   enables WiFi — but it is a one-way door: the parallel bus has already spent
   every ADC1 pin, so adding WiFi later would mean giving up touch.

Calibration taps four targets inset 30 px from the corners (a resistive panel
is least linear at its very edge, and corners cannot be tapped reliably), then
works out *from the data* whether the raw axes are swapped or inverted
relative to the screen. That is what makes it rotation-agnostic — nothing
assumes how the glass is mounted. Result is persisted to NVS.

Measured on this panel: idle `z = 0` against a threshold of 350, so open-panel
noise cannot produce a false press.

**Touch is now the only input.** The four push buttons were removed from the
design. What that buys: four GPIOs back (including two bidirectional), four
wires and two resistors off the build, and — the reason it came up — an end
to the phantom presses that floating input-only pins produce. GPIO34/35 have
no internal pull-up, and during Stage 5 an unpulled GPIO34 registered a long
press on its own and fired a **panic SOS that nobody asked for**. On a device
whose whole purpose is emergency traffic, an input that can invent an SOS is
worse than no input at all.

What it costs, stated plainly because it is now a single point of failure:

* **An uncalibrated panel is a dead device.** With buttons there was always a
  way in. Without them, `Touch::loadCal()` failing means nothing on screen can
  be reached. The UI must therefore detect this itself and drop into the
  calibration flow rather than presenting a UI it cannot receive taps for —
  the recovery path is no longer optional, it is the boot path.
* **The panic SOS needs a permanent home.** It used to be a long press on OK,
  which worked regardless of what held focus. It now has to be a persistent
  on-screen affordance present on every screen, because there is no
  out-of-band input left to carry it.
* Resistive touch through an ADC, sharing four lines with the LCD bus, is the
  least reliable subsystem in the build. Stage 2 stops being a bring-up step
  and becomes the thing the product depends on.

### 2.3 Health sensors — MAX30102 + MAX30205

The terminal also measures its operator's vitals, locally:

| Sensor | Measures | I2C addr | ID check |
|---|---|---|---|
| MAX30102 | Heart rate + SpO2 (red/IR photoplethysmography) | 0x57 (fixed) | `REV_ID` reg 0xFF reads 0x15 |
| MAX30205 | Body temperature | 0x48 (A0–A2 low; 0x48–0x4F range) | readback within 35–42 °C |

**The MAX30102 cannot measure temperature.** Its die-temperature register
(0x1F) exists only to compensate the LED wavelengths internally; it is not a
body sensor. That is why a MAX30205 sits on the same bus — the two share SDA,
SCL, power and ground, and cost one extra I2C address.

Why GPIO32/33: the ESP32's default `Wire` pins are GPIO21/22, and both are
LCD_D2/D3 on this build. `Wire.begin(32, 33)` — the bus is explicit, and
`pins.h` gains `PIN_I2C_SDA` / `PIN_I2C_SCL` as the single source of truth.
Pull-ups: the breakouts normally carry the 4.7 kΩ pair; Stage 4a's I2C scan
proves it either way.

INT: the MAX30102's interrupt is open-drain active-low. The first cut polls
the FIFO instead — one wire fewer, and LVGL already provides the between-frame
slot that touch uses. If a later stage wants the interrupt, GPIO35
(input-only, fine for an input) takes it with an external pull-up, since
GPIO34/35 have no internal one.

Power comes from the **ESP32's own 3V3 pin, not the shield's regulator** (see
R4b). LED budget: both PPG LEDs are configurable 0–51 mA in 0.2 mA steps;
fingertip use starts at 6.4 mA and stays capped low until the DevKit's
regulator is proven to hold (R6).

## 3. The ESP32 ↔ nRF52840 link protocol

### 3.1 ESP32 → nRF (outgoing messages) — works today, no firmware change

`serial_poll()` in [reference/nrf.cpp:237](reference/nrf.cpp#L237) already does exactly
what we need: it accumulates console characters and calls `originate()` on
`\r` or `\n`. So the ESP32 just writes:

```
NEED REINFORCEMENT\n
```

**Hard constraint: `MAX_PAYLOAD_LEN` is 32 bytes**
([reference/nrf.cpp:32](reference/nrf.cpp#L32)). Characters past 32 on a line are
silently dropped by the nRF. The compose screen must enforce a 32-character
limit with a visible counter, and every preset string must fit.

### 3.2 nRF → ESP32 (incoming messages) — needs a decision

Right now a received packet only surfaces as a *Zephyr log line*
([reference/nrf.cpp:193](reference/nrf.cpp#L193)):

```
[00:00:12.345,000] <inf> lora_mesh: RX from node 2 seq 5 ttl 3 (RSSI -80 dBm, SNR 9 dB): sos
```

Two ways to consume that:

- **Path A — parse the log line (zero nRF changes).** Doable, and Stage 6
  implements a parser for it. But it is brittle: it breaks if log colour is
  on (ANSI escapes), if the timestamp format changes, if a deferred-mode log
  drops lines under load, or if the log level is ever lowered.
- **Path B — add a machine-readable line (recommended, ~6 lines of Zephyr).**
  Emit a `printk()` alongside the existing `LOG_INF`, and have the ESP32
  accept only lines starting with `+`:

  ```c
  /* in handle_rx(), next to the existing LOG_INF */
  printk("+RX,%u,%u,%d,%d,%s\n", hdr->src, hdr->seq, rssi, snr, msg);

  /* in originate(), after a successful mesh_tx() */
  printk("+TX,%u,%s\n", tx_seq, text);
  ```

  Anything not starting with `+` is human log noise and gets ignored. This
  survives log reformatting, and `+TX` gives the UI a real send-confirmation
  instead of a hopeful assumption.

**The parser is written against Path B and falls back to Path A**, so the
project builds and runs either way — but Path B is what should ship.

### 3.3 Required Zephyr-side configuration

The console must be a **hardware UART**, not USB CDC ACM. `uart_dev` is
`DT_CHOSEN(zephyr_console)` ([reference/nrf.cpp:75](reference/nrf.cpp#L75)), so on a
board whose console is USB CDC (Xiao nRF52840, nRF52840 Dongle) the ESP32 can
never reach it — the ESP32 cannot be a USB host. Needed in the nRF project:

```
# prj.conf
CONFIG_LOG_MODE_IMMEDIATE=y        # no dropped/deferred lines
CONFIG_LOG_BACKEND_SHOW_COLOR=n    # no ANSI escapes to parse around
CONFIG_UART_CONSOLE=y
```

```dts
/* board overlay */
/ { chosen { zephyr,console = &uart0; }; };
&uart0 { status = "okay"; current-speed = <115200>; };
```

This is tracked as a Stage 4 task and is the single biggest integration risk.

---

## 4. Software architecture

```
esp32loralcd/
├── platformio.ini          one env per bring-up stage + the app + native tests
├── include/
│   ├── pins.h              wiring, the only place GPIO numbers are written
│   └── app_config.h        baud rates, timings, limits, preset messages
├── test_apps/              standalone bring-up apps, one per stage
│   ├── t1a_lcd_id.cpp      ← Stage 1a, bit-bangs the bus, no library
│   ├── t1_display.cpp      ← Stage 1b (both built now)
│   ├── t2_touch.cpp        Stage 2
│   ├── t3_touchui.cpp      Stage 3  touch as an LVGL input device
│   ├── t4_uart.cpp         Stage 4
│   └── t5_ui.cpp           Stage 5
├── include/lv_conf.h       LVGL build configuration (overrides only)
├── lib/
│   ├── lvgl_port/          LVGL <-> TFT_eSPI glue: draw buffer, flush, tick
│   ├── LoraLink/           UART framing + line parser + TX queue
│   ├── MessageStore/       fixed-capacity ring buffer of received messages
│   ├── Health/             MAX30102 + MAX30205: register maps, DSP, transport
│   └── UI/                 LVGL screens built on lvgl_port
├── src/main.cpp            Stage 7 integration — wiring only, no logic
└── test/
    ├── nrf.cpp             Zephyr reference (NOT built by PlatformIO)
    └── test_logic_*/       native Unity tests
```

**The rule that makes this testable:** `LoraLink`'s parser, `MessageStore`,
contain **no Arduino calls**. They
take bytes and a millisecond count as arguments. That way `pio test -e native`
runs the protocol and UI-state logic on the PC in under a second, and the
hardware stages only ever have to prove *wiring*, not logic.

`Health` follows the same rule: the register maps, the peak detector and the
SpO2 ratio math are a pure core that takes sample arrays and returns numbers —
no I2C, no Arduino — so `pio test -e native` feeds it captured PPG waveforms
and grades its heart rate against known truth (Stage 4b). Only a thin
transport layer speaks `Wire`.

### 4.0 The UI stack: LVGL v9.5

The UI is **LVGL v9.5.0**, not hand-rolled drawing. What that buys, concretely:
a real widget set (`lv_list`, `lv_textarea`, `lv_keyboard`, `lv_msgbox`),
flexbox layout so screens reflow instead of being pinned to pixel constants,
and a scrolling model that already knows the difference between a drag and a
tap — which on a noisy resistive sheet is worth more than it sounds, and is
the one piece of input handling touch-only still genuinely needs.

**The port layer is the only place that knows about hardware.**
`lib/lvgl_port/` owns the draw buffer, the flush callback and the tick; screens
above it never see TFT_eSPI and never learn the panel is on a parallel bus.

Measured at Stage 1c on this hardware:

| | |
|---|---|
| Refresh rate | **30–31 /s**, exactly the configured 33 ms cap — not CPU-bound |
| Draw buffer | 19.2 KB (240 × 40 px, RGB565, `RENDER_MODE_PARTIAL`) |
| LVGL pool | 26% of 48 KB used by a full inbox screen, fragmentation 2% |
| Free heap | 280 KB, flat across a 15 s run — no per-frame leak |

`RENDER_MODE_PARTIAL` with a single buffer is deliberate: `DIRECT`/`FULL` want
a screen-sized 150 KB buffer, and with a synchronous, DMA-less parallel bus
there is nothing for a second buffer to overlap with.

**Three v9-specific traps**, all of which cost real time if hit later:

1. **`lv_color_t` is a 3-byte RGB888 struct in v9 regardless of
   `LV_COLOR_DEPTH`.** Every v8 example sizes the draw buffer with
   `sizeof(lv_color_t)`; doing that here over-allocates by 50% and hands LVGL
   a stride it disagrees with — the output skews rather than failing cleanly.
   Buffers are sized from the colour *format*, at 2 bytes/px.
2. **A missing `lv_conf.h` does not fail the build.** LVGL falls back to
   built-in defaults, so a config that is only on the include path for some
   translation units silently produces one library compiled two different
   ways. This actually happened: PlatformIO puts `include/` on the path for
   `src/` but not for `lib/`, and it took `-I include` to fix.
   `static_assert`s in `lvgl_port.cpp` check three non-default values, and are
   what caught it — LVGL's own warning is only a `#pragma message`.
3. **RGB565 byte order.** LVGL renders little-endian, the ILI9341 wants
   MSB-first, so `pushColors(..., true)` does the swap. Getting this wrong
   scrambles colours *per pixel*, which looks similar to — but is not — the
   BGR panel-order fault from Stage 1b.
4. **`lv_list_add_button(list, NULL, "")` is not the same as `(…, NULL, NULL)`.**
   The widget only skips creating its label on a *NULL pointer*; an empty
   string still builds a label, sets `LV_LABEL_LONG_MODE_SCROLL_CIRCULAR` on
   it and gives it `flex_grow 1`. That invisible label then expands to absorb
   the row and shoves the real content out of place. Nothing about it looks
   wrong in the source.
5. **The default theme styles every container.** `lv_obj_create` arrives with
   a rounded border, an inset pad and a background, so a stack of containers
   shows gutters between the bands instead of a full-bleed layout. Stripping
   them is what `makeBand()` in `t1c_lvgl.cpp` exists for.

**Verified full-panel coverage.** Two independent checks, because "the layout
does not reach the edges" and "the port is not addressing the whole panel"
look identical on the bench and have nothing to do with each other:

```
band 0  x=0 y=  0  240x32          <- header
band 1  x=0 y= 32  240x254         <- list, flex_grow
band 2  x=0 y=286  240x34          <- footer
bands total height 320 of 320      OK - tiles the panel exactly
flush coverage (0,0)-(239,319) over 72 flushes
panel          (0,0)-(239,319)     OK - complete display used
```

`lvglPortGetCoverage()` accumulates the union of every area handed to the
flush callback, so the second check is measured at the hardware boundary
rather than inferred from the widget tree.

| Screen | LVGL widgets | Purpose |
|---|---|---|
| **Inbox** (home) | `lv_list` | Received messages, newest first: sender node, text, age, RSSI. Unread count in the header, which also carries a compact live vitals strip — `♥ 72 · SpO₂ 98% · 36.6°C` — so the operator sees them without leaving the screen. |
| **Detail** | `lv_label` | One message full-screen + metadata, with quick-reply actions. |
| **Presets** | scrolling `lv_list` | Canned messages — the primary path, two taps from the inbox. A list, not an `lv_buttonmatrix`: eight presets sharing a 240 px body would give 30 px rows, under the touch floor, and a matrix cannot scroll to buy room. |
| **Compose** | `lv_textarea` + `lv_keyboard` | Free text, hard 32-char limit with live counter. |
| **Vitals** | `lv_chart`, `lv_label`, `lv_bar` | Heart rate, SpO2 and temperature as large numbers, plus a scrolling PPG waveform and a signal-quality bar. Read-only: there is nothing to input. |
| **Status** | `lv_label`, `lv_bar` | Link state (three states, §4.1a), last TX/RX, seq numbers, RSSI/SNR, TX queue depth. |
| **SOS alert** | `lv_msgbox` | Full-screen takeover on an incoming SOS; must be acknowledged. |

### 4.1a Link state: three states, not two

The header indicator and the Status screen present the radio link in three
states, because "no messages yet" and "no radio at all" are different
conditions and must look different:

| State | Meaning | Evidence |
|---|---|---|
| **LINK UP** | UART healthy *and* mesh traffic observed | a `+RX`/`+TX` (or parsed log line) within the last `NRF_LINK_TIMEOUT_MS` |
| **SEARCHING FOR NETWORK** | UART healthy, no mesh traffic yet | lines (the nRF's beacons) arrive within the timeout, but no events — beacons only |
| **LINK DOWN** | the UART itself is silent | no complete line for `NRF_LINK_TIMEOUT_MS` |

The nRF beacons `hello <n>` every 10 s (`reference/nrf.cpp:314`), which makes
the middle state *decidable rather than a guess*: a beacon proves the wire,
and the absence of anything except beacons proves there is no peer to talk to.

**LINK DOWN is reserved for a fault the operator can act on** — wiring, power,
or the console still on USB CDC (PLAN.md §3.3). **SEARCHING FOR NETWORK is a
deployment condition, not a fault**, and must not be styled or worded like
one: no red, no "not responding". A device left alone with no peer in range
will sit in SEARCHING indefinitely, and that is correct operation.

Implementation note: `LoraLink::linkUp()` already measures exactly the UART
liveness half (any complete line refreshes `lastLine_`, including beacons).
What the UI needs on top is one flag, *mesh traffic seen*, set by any Rx or
TxConfirm event — never by a beacon — and reset on the same timeout as the
UART. The state is then the pair (uart alive, traffic seen).

Vitals update on two clocks that must not meet. The sensor FIFO is drained at
100 Hz into a ring buffer by the port layer, while the screen redraws its
numbers at ~1 Hz from cached values — the same "sample between frames, never
mid-draw" rule touch obeys. Values are only shown when the signal-quality
gate passes; otherwise the screen shows a `FINGER OFF` state rather than a
plausible-looking number. The Vitals screen adds nothing to the input rules
above: it is read-only.

**One input device, one widget tree.** The touch panel registers as a single
`LV_INDEV_TYPE_POINTER`. There is no keypad and no `lv_group_t`: with nothing
but a pointer, LVGL's focus model earns nothing — a tap addresses a widget
directly, so focus never has to be moved to it first.

That simplification is the main dividend of dropping the buttons, and it
removes the awkward corner the old design had: navigating an `lv_keyboard`
cell-by-cell with two buttons was always going to be miserable, and only
existed because the keypad had to reach every widget the pointer could.

Three rules the touch-only design has to hold to, because there is no longer
a second way in when one of them fails:

1. **Every target is at least `UI_MIN_TOUCH_PX` (40 px) on its side.** A
   fingertip on a resistive sheet is not precise and calibration error adds a
   few pixels on top. On a 240 px-wide panel that allows one full-width
   action per row, which is why the presets are a single scrolling column
   rather than a 2×4 grid. `static_assert`s in `lib/UI/UI.cpp` tie the row
   and footer heights to this constant, so a future resize cannot quietly
   drop below it.

   **The compose keyboard is the one exception, and it is unavoidable:** a
   10-column QWERTY across 240 px gives 24 px keys, and no arrangement fixes
   that while it is still a keyboard. `lv_keyboard_set_popovers()` draws the
   pressed key enlarged above the fingertip, so what is about to be typed is
   visible even though the finger covers the key. This is exactly why presets
   are the primary send path and free text is the fallback.
2. **SOS is reachable on every screen**, including the SOS takeover itself —
   an incoming emergency is exactly when the operator is most likely to need
   to raise one of their own, so that screen carries a `SEND MY SOS` control
   alongside `ACKNOWLEDGE`. Everywhere else it lives in the footer action
   bar, which is built by the shared chrome rather than by each screen, so no
   screen can forget it. One tap sends, with no confirmation step: that is
   what the long press on OK did too, and a confirm dialog is what someone
   under stress cannot deal with. The trade is that a stray tap transmits.
3. **A failed calibration boots into calibration.** With no fallback input, a
   UI drawn on an uncalibrated panel is unreachable. `Touch::loadCal()`
   returning false must therefore route straight into the Stage 2 flow, not
   into the inbox.

An incoming message whose text starts with `SOS` takes over the screen as a
red full-screen alert that must be acknowledged.

---

## 5. Staged workflow

Each stage is its own PlatformIO environment, flashed and signed off on its
own. **No stage begins before the previous stage's exit criteria are met.**
This is the point of the whole plan: when the integrated app misbehaves at
Stage 7, every layer underneath it has already been proven in isolation, so
the bug has nowhere to hide.

| # | Stage | Build & flash | Exit criteria |
|---|---|---|---|
| 0 | Scaffolding, pin map, config | `pio run -e t1_display` | Project compiles |
| **1a** | **Identify the LCD controller** | `pio run -e t1a_lcdid -t upload` | ✅ **done** — no ID available (see below); fell back to trying ILI9341 |
| **1b** | **TFT display bring-up** | `pio run -e t1_display -t upload` | ⏳ init + geometry + readback + throughput all pass; awaiting visual sign-off on colours and rotation |
| **1c** | **LVGL port bring-up** | `pio run -e t1c_lvgl -t upload` | ✅ **done** — 30–31 refresh/s, pool flat at 26%, heap flat at 280 KB, no leak |
| **2** | **Touch + calibration** | `pio run -e t2_touch -t upload` | ⏳ idle `z=0` confirmed (no false presses); awaiting the interactive calibration tap sequence |
| 3 | Touch as an LVGL input | `pio run -e t3_touchui -t upload` | Touch registered as `LV_INDEV_TYPE_POINTER`; every target ≥ 40 px is hit first time across the whole panel; scroll and tap are distinguishable (no accidental scroll swallowing a tap); an uncalibrated panel routes into calibration instead of an unreachable UI |
| 4 | UART link to nRF | `pio run -e t4_uart -t upload` | **TX** ESP32→nRF: typed line goes on air at the far node, 32-char boundary intact, `+TX` confirms send. **RX** nRF→ESP32: `+RX`/log lines parse into inbox entries with correct sender/seq/RSSI. **Round-trip**: 100 numbered pings answered in order, zero loss. **Soak**: 10 min continuous traffic at 115200, zero framing errors, zero partial lines — counters displayed throughout. **States**: a beacon-only link shows SEARCHING FOR NETWORK, never LINK UP; silence shows LINK DOWN |
| 4x | UART link diagnostic | `pio run -e t4x_linkdiag -t upload` | Self-scoring battery over the live link: loopback, round-trip ping with sequence numbers, framing-error and dropped-line counters, 32-char boundary cases. A 5/5 verdict is the sign-off; rerun whenever the link misbehaves |
| 4a | Health bring-up: MAX30102 + MAX30205 | `pio run -e t4a_health -t upload` | I2C scan at 100/400 kHz finds 0x57 and 0x48; MAX30102 `REV_ID` = 0x15; FIFO streams red+IR at 100 Hz with no corruption; MAX30205 reads 35–42 °C, stable within ±0.1 °C over a minute |
| 4b | HR/SpO2 algorithm + native tests | `pio test -e native` | Peak-detected HR within ±3 bpm of truth on captured waveforms (clean and noisy fixtures); SpO2 ratio-of-ratios computed; finger-off waveforms classify as no-valid-signal |
| 4c | Vitals UI on mock data | `pio run -e t5_ui -t upload` | Vitals screen + inbox header strip render from the fake generator; chart scrolls; `FINGER OFF` state shows correctly |
| 5 | LVGL UI (mock data) | `pio run -e t5_ui -t upload` | All **7** screens navigable by touch alone, SOS reachable in one tap from every screen, driven by a fake message generator and fake vitals — **no radio involved** |
| 6 | Protocol layer + native tests | `pio test -e native` | Parser handles both Path A and Path B, plus truncation, garbage, partial lines, buffer overrun |
| 7 | Integration | `pio run -e app -t upload` | End-to-end: two nodes, message sent from one appears on the other's inbox; live vitals update in the header and Vitals screen from the real sensors; link states correct — DOWN only when the UART is silent, SEARCHING on beacon-only traffic, UP once mesh traffic flows |
| 8 | Hardening | — | Watchdog, TX queue backpressure, UART-silence detection ("nRF not responding"), brownout check with backlight at full, I2C timeout + recovery, finger-off/poor-signal gating, LED current capped under the regulator budget |

**Proving the link, not just trying it.** Stage 4's exit criteria are
bidirectional and measured, because a UART with no parity and no CRC cannot
tell you it dropped a byte — only traffic counts can. Every check writes a
number to the screen: lines sent, `+TX` acks, parsed `+RX` lines, framing
errors, dropped lines. A link that "looks fine" in a demo but drops one byte
in a thousand is exactly the failure the soak with counters catches, and a
silent 32-char truncation at the nRF boundary would show up as a short
message at the far node. The 4x diagnostic re-runs the same battery on
demand after any wiring or firmware change, same as the touch diagnostics do
for the panel.

**Why the buttons went away.** They were in the design to guarantee a second
way in, and instead they introduced a way for the device to act on its own:
GPIO34/35 are input-only with no internal pull-up, and during Stage 5 an
unpulled GPIO34 registered a long press by itself and fired a panic SOS. The
fix was two resistors, but the episode exposed that four extra wires and two
resistors were buying an input path that the touch panel already covered —
while doubling the input code, forcing an `lv_group_t` focus model the
pointer did not need, and making the compose keyboard navigable only in a
miserable cell-by-cell way. Touch-only is smaller, cheaper and cannot invent
an emergency. The cost is R4d/R4e above, which are handled rather than
ignored.

**Why 1a exists.** These shields ship with any of a dozen controllers behind
an identical-looking panel and the silkscreen never says which. The driver is
a compile-time choice, so guessing it wrong produces a white screen that is
indistinguishable from a wiring fault — and the whole point of this workflow
is that a symptom always has one possible cause. Stage 1a settles it by
bit-banging the bus directly, depending on no display library at all.

**What 1a actually found on this hardware.** No ID. This panel does not
implement the ID registers (`0x04`, `0xD3`, `0xBF` all return only the ESP32's
own pin capacitance) even though it implements the ILI9341 *command set*
perfectly. That is common on clone shields, and it exposed a limit of the
tool worth stating plainly: probing with an arbitrary command and re-reading
cannot distinguish "bus is broken" from "controller correctly ignores a
command that is not a read command". Its floating-bus report is a hint, never
a verdict — the tool now says so and points at Stage 1b instead.

**Controller: ILI9342, confirmed empirically — not ILI9341.** This cost real
time and is worth recording precisely.

The two parts share an init sequence, a rotation table and a command set, and
differ in one thing: native geometry. ILI9341 is 240×320; **ILI9342 is
320×240**. Because everything else matches, the wrong choice does *not* fail
loudly — the panel initialises, throughput measures **51.7 fps / 3.97
Mpixel/s**, and Stage 1b's own geometry test reports exactly the size the
driver was told to expect. It is self-consistent and wrong.

The only symptom is at the edges: drawing lands in the left 240 of 320
columns while the bottom 80 rows fall off the glass, leaving a vertical strip
of stale pixels down the right-hand side that `fillScreen()` can no longer
reach. On a test that repaints continuously, that strip holds fragments of an
*earlier* frame drawn under a different rotation — text at 90° to everything
else, which reads as bus corruption rather than a geometry mismatch.

The lesson for Stage 1a: matching a *command set* does not identify a
controller. Only geometry does, and geometry can only be checked against the
physical edges of the glass.

Because native is landscape, the rotation numbering is inverted relative to a
real ILI9341: **portrait 240×320 is `TFT_ROTATION 1`**, not 0.

**Why this order.** Stages 1–3 prove the three input/output surfaces
independently, so a display fault can never be mistaken for a touch fault.

**Why 4a/4b/4c exist.** A wrong SpO2 number has three possible authors — bad
sensor, bad DSP, bad rendering — and the health stages keep them apart
exactly as Stages 1–5 keep display, touch and UI apart. 4a proves the sensor
and the bus in raw form (part IDs, FIFO rate, temperature readback); 4b
proves the algorithm on the PC against known waveforms, where a bug costs a
second instead of a finger on the glass; 4c proves the screen against fake
vitals, so at Stage 7 the only new thing is one live data feed — the same
reasoning that keeps the radio path honest.
Stage 5 builds the entire UI against *fake* messages, which means the UI is
finished and debugged before the radio is ever attached — at Stage 7 the only
new thing in the system is one function call replacing the fake generator.

---

## 6. Risks and open issues

| | Risk | Mitigation |
|---|---|---|
| **R1** | **nRF console is on USB CDC**, so the ESP32 can never reach it | Zephyr overlay repointing `zephyr,console` to `uart0`. Blocks Stage 4 — resolve early. |
| **R2** | 32-byte payload cap silently truncates messages | Enforce in the compose UI with a live counter; unit-test the boundary |
| **R3** | **Unknown LCD controller.** A wrong `*_DRIVER` flag looks exactly like a wiring fault | Stage 1a identifies it empirically before any driver is compiled |
| **R11** | The UART carries no integrity check — a dropped or corrupted byte silently mangles a message | Line-based framing with on-screen counters; Stage 4's 10 min soak and the 4x diagnostic require zero errors; lines that fail to parse are counted, never shown |
| **R4** | **Touch shares four LCD lines and has no controller.** Every read reconfigures `CS`/`RS` as analog inputs and hand-drives two data lines, leaving the display bus in the wrong state | Save/restore pin modes around each read; sample only between frames, never mid-draw. Owned by Stage 2 |
| **R4b** | The shield's 3.3 V regulator is fed from its `5V` pin. Feeding it 3.3 V yields ~2.2 V and the controller never starts — presenting as a dead bus | Feed `5V` from `VIN`. A minority of shields instead have 5 V input dividers needing the opposite fix; Stage 1a's failure text distinguishes the two ([WIRING.md](WIRING.md) §1) |
| **R4c** | ESP32 ADC2 (used for touch) is unavailable whenever WiFi is active | This device never enables WiFi. Worth a comment at any future point someone reaches for it |
| **R4d** | **Touch is now the only input, so it is a single point of failure.** A lost or corrupt calibration leaves a UI nothing can reach | Boot checks `Touch::loadCal()` and routes into the Stage 2 calibration flow on failure. The flow itself must be usable *uncalibrated* — it is, because it works in raw ADC space and asks for taps at known targets rather than reading widgets |
| **R4e** | Dropping the buttons removes the out-of-band panic path. A long press on OK used to fire an SOS regardless of what held focus | A persistent on-screen SOS control on every screen, sized ≥ 40 px, never more than one tap from anywhere |
| **R5** | Zephyr log lines could be dropped in deferred mode under load | `CONFIG_LOG_MODE_IMMEDIATE=y`, and prefer Path B |
| **R6** | 3.3V regulator on a DevKit V1 can sag with the TFT at full brightness — and the MAX30102's two LED pulse channels add to the same rail | Check `esp_reset_reason()` for brownout during Stage 1; power the TFT from a separate 3.3V rail if needed; start PPG LEDs at 6.4 mA and cap below 12 mA until Stage 8's brownout check passes |
| **R7** | No delivery guarantee — the mesh is fire-and-forget | `+TX` confirms *transmission*, not reception. Show "sent" not "delivered". App-level ACK is a possible Stage 9. |
| **R8** | A knockoff MAX30102 can be missing registers or return a wrong/absent `REV_ID` — and on a bare read, a dead sensor and a dead bus look identical | Stage 4a checks part IDs before anything else, exactly the Stage 1a discipline; unknown IDs are treated as unsupported, not guessed at |
| **R9** | Motion and ambient light corrupt the PPG waveform — SpO2 in particular is easy to compute *confidently and wrongly* | Values display only when the signal-quality gate passes; `FINGER OFF` state otherwise. The algorithm is graded on noisy captured fixtures in 4b, not just clean ones |
| **R10** | Temperature and HR can read "fine" while the sensor is broken, because nothing cross-checks them | MAX30205 readback must sit in 35–42 °C at boot (4a); HR is only shown alongside a passing waveform gate, and a vitals row stuck at a constant value for >60 s is flagged as sensor-dead |

Open question deferred to Stage 7: the nRF beacons `hello <n>` every 10 s
([reference/nrf.cpp:314](reference/nrf.cpp#L314)). The inbox should almost certainly
filter these into the Status screen rather than showing them as messages.
