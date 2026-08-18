# LoRa Messenger Terminal

An ESP32 front-end for an nRF52840 LoRa mesh node — a 2.4" touch TFT terminal
for sending and receiving short text messages over a LoRa mesh, with no
infrastructure of any kind behind it.

```
   ┌──────────────────────────────┐          ┌───────────────────────────┐
   │  ESP32 DevKit V1 (WROOM-32)  │          │  nRF52840 + SX126x        │
   │  ┌────────────────────────┐  │          │  (Zephyr, reference/)     │
   │  │ 2.4" ILI9342 240x320   │  │  UART2   │  ┌─────────────────────┐  │
   │  │ 4-wire resistive touch │  │◄────────►│  │ mesh_tx / handle_rx │  │
   │  │ (no controller)        │  │ 115200   │  │ TTL=3, dup-cache    │  │
   │  └────────────────────────┘  │  8N1     │  └──────────┬──────────┘  │
   │                              │          │             │             │
   │   touch is the ONLY input    │          │        ((( LoRa )))       │
   │                              │          │        865.1 MHz SF10     │
   └──────────────────────────────┘          └───────────────────────────┘
        UI + message store                        radio + mesh routing
```

**The ESP32 owns nothing radio-shaped.** The nRF52840 already handles framing,
TTL, duplicate suppression, listen-before-talk and retries. This side is a
terminal: it renders received text, collects outgoing text, and speaks one
line-based protocol over a UART. Keeping that boundary sharp is what makes the
ESP32 half unit-testable on a PC.

## Documentation

| | |
|---|---|
| [PLAN.md](PLAN.md) | Architecture, the staged bring-up workflow, risks |
| [WIRING.md](WIRING.md) | Every wire, with the failure mode each one causes |

## Layout

```
include/       pins.h (the only place GPIO numbers are written), app_config.h, lv_conf.h
lib/
  Touch/       4-wire resistive panel sharing the LCD bus
  TouchCal/    calibration flow — usable on an uncalibrated panel, so it can be the boot path
  lvgl_port/   LVGL <-> TFT_eSPI glue: draw buffer, flush callback, tick
  LoraLink/    UART line parser (two wire formats) + bounded TX queue
  MessageStore/ fixed-capacity ring of messages
  UI/          six LVGL screens, laid out for 240x320
test_apps/     one standalone bring-up app per stage
test/          native Unity tests — run on a PC, no hardware
reference/     Zephyr source for the radio node (NOT built by PlatformIO)
src/main.cpp   integration: wiring only, no logic
```

## Build

One environment per bring-up stage. Each is flashed and signed off on its own,
so when the integrated app misbehaves every layer under it has already been
proven and the bug has nowhere to hide.

```bash
pio run -e t1a_lcdid  -t upload   # identify the LCD controller
pio run -e t1_display -t upload   # panel: colour, geometry, readback, throughput
pio run -e t1c_lvgl   -t upload   # LVGL port: buffer, flush, tick, memory
pio run -e t2_touch   -t upload   # touch: raw readout, calibration, verify
pio run -e t3_touchui -t upload   # touch as an LVGL input device
pio run -e t4_uart    -t upload   # UART link to the nRF52840
pio run -e t5_ui      -t upload   # the whole UI on mock data, no radio
pio run -e t6_touchdiag -t upload # self-scoring touch diagnostic suite
pio test -e native                # protocol/logic tests, on the host
pio run -e app        -t upload   # the integrated application
```

`pio test -e native` builds no board, no framework, no TFT_eSPI and no LVGL —
`LoraLink` and `MessageStore` are written free of Arduino calls precisely so
they run on a PC in under a second. If either ever picks up an Arduino
dependency, that environment stops compiling and says so.

## Two findings worth knowing before you buy one of these shields

**The controller is an ILI9342, not an ILI9341.** They share an init sequence,
a rotation table and a command set, and differ in native geometry: 240×320
against 320×240. The wrong choice does not fail loudly — the panel initialises,
GRAM round-trips, throughput measures fine — and the only symptom is that
drawing lands in the left 240 of 320 columns while the bottom 80 rows fall off
the glass. Matching a *command set* does not identify a controller; only
geometry does. Because native is landscape, portrait is `TFT_ROTATION 1`.

**The panel is RGB-ordered** where TFT_eSPI defaults these controllers to BGR,
so `-DTFT_RGB_ORDER=1` is required or red and blue are exchanged.

## Hardware

ESP32 DevKit V1 (30-pin DOIT, WROOM-32) and a 2.4" Uno-format TFT shield on an
8-bit parallel bus. 18 jumpers total; the shield will not plug into a DevKit,
so every line is wired by hand. The touch panel costs **zero** additional GPIO —
its four corners are already wired, inside the shield, onto lines the LCD uses.

Read [WIRING.md §1](WIRING.md) before anything else: the shield must be fed
**5 V**, not 3.3 V, because its onboard regulator is what makes the panel's
3.3 V. Getting that wrong is the single most common way to kill one of these
builds before it starts.

## Licence

Apache-2.0.
