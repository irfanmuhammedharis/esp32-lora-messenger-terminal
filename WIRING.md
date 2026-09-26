# Wiring — LoRa Messenger Terminal

**Board:** ESP32 DevKit V1, 30-pin DOIT, WROOM-32
**Display:** 2.4" TFT LCD **shield**, Arduino Uno form factor, 8-bit parallel bus,
**ILI9342** controller (320×240 native, driven portrait at 240×320),
4-wire resistive touch with **no touch controller**
**Input:** touch only — no push buttons, no extra wires
**Radio:** nRF52840 + SX126x running [reference/nrf.cpp](reference/nrf.cpp)

GPIO numbers come from [include/pins.h](include/pins.h), which is the only
place in the project they are written down.

> The shield is built for an Uno, so it will not plug into an ESP32 DevKit.
> This is a **20-jumper build** (15 of them just for the display). Male-to-female
> dupont leads, kept short — the parallel bus has no error checking.

---

## 1. Power — read this first

This is the step that most often kills a shield-on-ESP32 build before it
starts. The shield carries its **own 3.3 V regulator**, fed from its `5V`
header pin, and that regulator is what powers the LCD controller and the
backlight.

```
   shield 5V pin ──► [ AMS1117-3.3 ] ──► LCD controller + backlight
```

**So the shield must be fed 5 V, not 3.3 V.** Put 3.3 V into that regulator
and you get roughly 2.2 V out, the controller never comes out of reset, and
every diagnostic reads back `0xFF`.

| Shield pin | Connect to | Why |
|---|---|---|
| `5V` | ESP32 **`VIN`** | VIN is USB 5 V; feeds the shield's regulator |
| `GND` | ESP32 `GND` | |
| `3V3` | **leave unconnected** | it is the regulator's *output* |
| `IOREF`, `RESET`, `AREF`, `A5`, `D0`, `D1`, `D10`–`D13` | leave unconnected | `D10`–`D13` are the unused SD-card slot |

The logic lines are a separate matter and need no level shifting: the LCD
controller is a 3.3 V part, so the ESP32's 3.3 V GPIOs drive it directly —
in fact more correctly than the 5 V Uno the shield was designed for.

> ⚠️ **The one exception.** A minority of these shields put 5 V→3.3 V resistor
> dividers on the logic inputs, assuming a 5 V host. Driving those with 3.3 V
> leaves about 2.2 V at the controller, which may sit under its input
> threshold. Symptom: Stage 1a reads `0xFF` everywhere even though power is
> correct. Look for resistor packs crowded against the headers. If that is
> what you have, feed the shield's `3V3` pin from ESP32 `3V3` and leave `5V`
> unconnected instead — that puts host and panel on the same rail.

---

## 2. Display — 15 wires

⚠️ **The single most common miswire:** the shield's header pins `D8` and `D9`
carry **LCD_D0 and LCD_D1**, while its `D2`–`D7` carry LCD_D2–LCD_D7. The
numbering does *not* line up. Wire `D8`→GPIO13 and `D9`→GPIO14, not to
whatever "D0/D1" suggests.

| Shield pin | Signal | ESP32 GPIO | Notes |
|---|---|---|---|
| `A0` | LCD_RD | **4** | read strobe |
| `A1` | LCD_WR | **27** | write strobe |
| `A2` | LCD_RS | **25** | register select (a.k.a. DC) · also touch **XM** |
| `A3` | LCD_CS | **26** | chip select · also touch **YP** |
| `A4` | LCD_RST | **15** | |
| `D8` | LCD_**D0** | **13** | ⚠️ note the crossover · also touch **XP** |
| `D9` | LCD_**D1** | **14** | ⚠️ note the crossover · also touch **YM** |
| `D2` | LCD_D2 | **16** | moved off 21 to free it for I2C (§5) |
| `D3` | LCD_D3 | **17** | moved off 22 to free it for I2C (§5) |
| `D4` | LCD_D4 | **23** | |
| `D5` | LCD_D5 | **18** | |
| `D6` | LCD_D6 | **19** | |
| `D7` | LCD_D7 | **5** | |
| `5V` | power | **VIN** | see §1 |
| `GND` | ground | **GND** | |

```
        2.4" TFT shield (Uno headers)                ESP32 DevKit V1
   ┌──────────────────────────────────┐
   │  POWER                           │
   │    5V   ─────────────────────────┼──────────────►  VIN   (5V)
   │    GND  ─────────────────────────┼──────────────►  GND
   │    3V3  ── (regulator output, leave alone)
   │                                  │
   │  ANALOG                          │
   │    A0  LCD_RD  ──────────────────┼──────────────►  GPIO4
   │    A1  LCD_WR  ──────────────────┼──────────────►  GPIO27
   │    A2  LCD_RS  ──────────────────┼──────────────►  GPIO25  ◄─┐ ADC
   │    A3  LCD_CS  ──────────────────┼──────────────►  GPIO26  ◄─┤ needed
   │    A4  LCD_RST ──────────────────┼──────────────►  GPIO15    │ for touch
   │                                  │                           │
   │  DIGITAL                         │                           │
   │    D2  LCD_D2  ──────────────────┼──────────────►  GPIO16    │
   │    D3  LCD_D3  ──────────────────┼──────────────►  GPIO17    │
   │    D4  LCD_D4  ──────────────────┼──────────────►  GPIO23    │
   │    D5  LCD_D5  ──────────────────┼──────────────►  GPIO18    │
   │    D6  LCD_D6  ──────────────────┼──────────────►  GPIO19    │
   │    D7  LCD_D7  ──────────────────┼──────────────►  GPIO5     │
   │    D8  LCD_D0  ──────────────────┼──────────────►  GPIO13  ──┤
   │    D9  LCD_D1  ──────────────────┼──────────────►  GPIO14  ──┘
   │    D10..D13  (SD card, unused)   │
   └──────────────────────────────────┘
```

**Backlight:** hardwired to the shield's own 3.3 V rail. There is no control
pin on the Uno headers, so brightness is fixed and no GPIO is spent on it —
which also removes the GPIO-current risk an SPI module would have had.

---

## 3. Touch — 0 extra wires

The panel is a bare 4-wire resistive sheet with **no controller**. Its four
corners are already wired, inside the shield, onto four lines you have
connected:

```
        XP ── LCD_D0 (GPIO13)          To read X:  drive XP high, XM low,
        XM ── LCD_RS (GPIO25)   ADC                sample YP as an analog voltage
        YP ── LCD_CS (GPIO26)   ADC    To read Y:  drive YP high, YM low,
        YM ── LCD_D1 (GPIO14)                      sample XM as an analog voltage
```

This is why `LCD_RS` and `LCD_CS` are on GPIO25 and GPIO26 specifically — they
are ADC2 channels 8 and 9. Any other assignment would leave the touch panel
unreadable. (ADC2 is unavailable while WiFi is running, which costs nothing
here: this device never brings WiFi up.)

Two consequences that shape the software, both owned by Stage 2:

1. **A touch read scrambles the LCD bus.** It reconfigures `CS` and `RS` as
   analog inputs and drives two data lines by hand. The display driver's pin
   modes have to be saved and restored around every read, and reads only
   happen between frames — never mid-draw.
2. **There is no calibration data and no interrupt.** Raw ADC counts have to
   be mapped to pixels by a calibration routine, then persisted to NVS. The
   ESP32's ADC is 12-bit and noticeably non-linear near the rails, so the
   readings need averaging and the panel needs `analogSetAttenuation(ADC_11db)`.

---

## 4. Push buttons — removed from the design

There are none. **Touch is the only input**, and it needs no wires at all
(section 3) because the panel is already connected through the LCD lines.

This used to be four switches on GPIO32–35 plus two 10 kΩ resistors. If you
have already fitted them, they are harmless — nothing reads those pins now —
but they can be removed.

> ⚠️ **If you keep them wired, fit the resistors.** GPIO34/35 are input-only
> and have **no internal pull-up**. Left floating they drift, and during
> Stage 5 an unpulled GPIO34 registered an 800 ms long press on its own and
> fired a **panic SOS nobody asked for**. That episode is why the buttons
> came out (PLAN.md section 5).

Freed by this change: **GPIO32, 33, 34, 35** — 32 and 33 are fully
bidirectional. GPIO35 is spoken for (section 5, MAX30102 INT), GPIO32/33
carry the nRF link (section 6), and GPIO34 is **reserved for a future NEO-6M
GPS module**, see section 7.

---

## 5. Health sensors — 4 wires (+1 optional)

MAX30102 (heart rate + SpO2) and MAX30205 (body temperature), sharing one I2C
bus. **The MAX30102 has no temperature sensor** — the MAX30205 is the
temperature source (PLAN.md §2.3).

```
   MAX30102 / MAX30205 breakouts           ESP32 DevKit V1
   ┌─────────────────────────┐
   │  SDA  ──────────────────┼──────────────►  GPIO21  (I2C data)
   │  SCL  ──────────────────┼──────────────►  GPIO22  (I2C clock)
   │  3V3  ──────────────────┼──────────────►  3V3     (ESP32's own rail —
   │  GND  ──────────────────┼──────────────►  GND      NOT the shield's)
   │  INT  ──── leave off for now; later: GPIO35 + external 4.7 kΩ pull-up
   └─────────────────────────┘
```

This is the ESP32's default `Wire` pin pair (GPIO21/22) — usable now because
LCD_D2/D3 moved to GPIO16/17 (§2) to free them, since the nRF link moved off
those pins (§6). `Wire.begin()` is still called with
`PIN_I2C_SDA`/`PIN_I2C_SCL` explicit rather than relying on the implicit
default, so [include/pins.h](include/pins.h) stays the one place pin numbers
are written down.

Pull-ups: most breakouts carry the 4.7 kΩ pair on board; Stage 4a's I2C scan
proves it. If the scan finds nothing, add 4.7 kΩ from SDA and SCL to 3V3.

Addresses: MAX30102 = 0x57 (fixed), MAX30205 = 0x48 (A0–A2 low). Scan at
100 kHz first, then 400 kHz.

---

## 6. nRF52840 link — 3 wires

Cross-wired UART. **TX goes to RX**, and the grounds must be common or the
line has no reference and you get garbage bytes.

```
   ESP32 (3.3 V)                    XIAO nRF52840 + Wio-SX1262 (3.3 V)
   ┌────────────┐                         ┌────────────────────┐
   │  GPIO32    │──────────────────────►  │  D7  RX  (P1.12)   │
   │  (TX2)     │                         │                    │
   │  GPIO33    │  ◄──────────────────────│  D6  TX  (P1.11)   │
   │  (RX2)     │                         │                    │
   │  GND       │─────────────────────────│  GND               │
   └────────────┘                         └────────────────────┘
                    115200 8N1
```

On the 30-pin DOIT board, `D32` and `D33` sit next to each other on the
header row that starts at `EN`, between `D35` and `D25`.

**D6/D7 are the only XIAO pins left.** The Wio-SX1262 takes D1–D5 (IRQ,
NRESET, BUSY, NSS, RXEN) and D8–D10 (SPI), so `uart0` — whose default pinctrl
is exactly P1.11/P1.12 — lands on the two that remain. Nothing had to be
moved to make this fit.

Both parts are 3.3 V logic — no level shifter. If the nRF has its own supply,
connect **grounds only**, not power.

**The nRF stays wired while you flash.** The link used to sit on TX0/RX0
(GPIO1/3), which this DevKit hard-wires to its USB-serial bridge chip. The
nRF then fought that chip for GPIO3: uploads failed with "The serial TX path
seems to be down" unless the nRF was unplugged. The nRF also radioed the
ESP32's boot log out as messages. On GPIO32/33 neither happens, and the
`Serial` monitor on USB is a clean debug console again.

The ESP32 enables GPIO33's internal pull-up, so RX idles high when the nRF
is unplugged. The node only sends lines that start with `+SEND,`
([PLAN.md](PLAN.md) §3.1), so noise on the wire during an ESP32 reset never
reaches the air.

✅ **The nRF console now lives on `uart0`, not USB CDC** — `uart_dev` is
`DT_CHOSEN(zephyr_console)` ([reference/nrf.cpp:82](reference/nrf.cpp#L82)), so the
console *is* this link, and the ESP32 cannot act as a USB host. The board
overlay and its `.conf` in the Zephyr sample carry the change; see
[PLAN.md](PLAN.md) §3.3 for the exact settings and §3.4 for the per-set
flashing procedure.

> The nRF therefore no longer enumerates a serial port on a PC. That is
> expected, not a fault — the only window onto it is the ESP32, or a USB-TTL
> adapter clipped onto D6/D7. UF2 flashing is unaffected, because that USB
> belongs to the bootloader rather than to this application.

**Verify before trusting the link** (Stage 4, PLAN.md §5): a line typed on
the ESP32 appears on air at the far node (**TX**), the nRF's `+RX` lines land
in the ESP32 inbox (**RX**), 100 round-trip pings come back in order with
zero loss, then a 10-minute soak with zero framing errors. The `t4x_linkdiag`
environment re-runs this battery on demand.

**Reading the link indicator (PLAN.md §4.1a):** the UI reports one of three
states, not a simple up/down, because a silent UART and a healthy UART with
no peer in range look identical unless something distinguishes them:

| State | Wiring implication |
|---|---|
| **LINK UP** | TX/RX/GND all good, and a second mesh node is in range |
| **SEARCHING FOR NETWORK** | TX/RX/GND are wired correctly — the nRF's `hello` beacons are getting through — there is just no peer to talk to yet. **Not a wiring fault**, do not re-check the wires for this. |
| **LINK DOWN** | the UART itself is silent — this is the wiring fault. Recheck the TX↔RX cross, GND, and that the nRF console has been moved off USB CDC (PLAN.md §3.3) |

A single unpaired device will sit in **SEARCHING FOR NETWORK** indefinitely.
That is correct operation, not a symptom to chase.

---

## 7. Future expansion — NEO-6M GPS (reserved, not wired)

Not part of the current build. GPIO34 is held in reserve so nothing else
claims it first. (This reservation was GPIO32/33 until the nRF link needed
them, §6.)

```
   NEO-6M GPS module                       ESP32 DevKit V1
   ┌─────────────────────────┐
   │  TX  ────────────────────┼──────────────►  GPIO34  (RX, UART1)
   │  RX  ──── leave off — no output pin is left to drive it
   │  VCC ────────────────────┼──────────────►  3V3
   │  GND ────────────────────┼──────────────►  GND
   └─────────────────────────┘
                    9600 8N1 (module default)
```

One wire is enough. The module streams NMEA sentences on its own from power-up, and
those carry the position fix. Its RX input only matters for reconfiguring
the module, which the default settings don't need. GPIO34 is input-only,
which is fine for a receive line. It has no pull-up, but NMEA sentences
carry a checksum, so noise with no module fitted is rejected. It gets its own
peripheral, UART1, untouched by anything else in the project:

```cpp
Serial1.begin(9600, SERIAL_8N1, /*rx=*/34, /*tx=*/-1);
```

Like every other GPIO, once this module is actually wired its pin belongs in
[include/pins.h](include/pins.h) as `PIN_GPS_RX`, not hardcoded at the call
site.

---

## 8. Build order

Wire only what the current stage needs — that is the whole point of the
staged workflow in [PLAN.md](PLAN.md) §5.

| Stage | Add | Running total |
|---|---|---|
| **1a** identify controller | power + all 13 LCD lines | 15 wires |
| **1b** display bring-up | *nothing* | 15 wires |
| **2** touch | *nothing* — shares LCD lines | 15 wires |
| **3** touch in LVGL | *nothing* — no input hardware at all | 15 wires |
| **4** nRF link | TX, RX, GND | 18 wires |
| **4a** health bring-up | SDA, SCL, 3V3, GND (+1 INT later) | 19 wires |

```bash
pio run -e t1a_lcdid -t upload && pio device monitor   # then set the driver
pio run -e t1_display -t upload
```

---

## 9. If it doesn't work

| Symptom | Cause | Fix |
|---|---|---|
| Stage 1a reads `0xFF` everywhere | Shield unpowered — 3.3 V fed to its `5V` pin | Feed `5V` from `VIN`; verify ~3.3 V on the shield's `3V3` pin |
| Stage 1a reads `0xFF`, power verified | Shield has 5 V input dividers | Feed `3V3` from ESP32 `3V3`, leave `5V` off (§1) |
| Stage 1a reads `0x0000` everywhere | Data line shorted to GND, or `CS`/`RS` swapped | Recheck `A2`→GPIO25 and `A3`→GPIO26 |
| Stage 1a returns unstable garbage | `D8`/`D9` wired as if they were D0/D1 | They carry LCD_D0/D1 — see §2 |
| ID found, but white screen in 1b | Wrong `*_DRIVER` flag | Use exactly what 1a printed |
| Image fills only the left ~75%, stale strip down the right edge, bottom rows missing | **ILI9342 driven as an ILI9341.** Native geometry differs (320×240 vs 240×320) but everything else matches, so it initialises and passes throughput happily | `-DILI9342_DRIVER=1`. **This build needs it.** Note portrait then becomes `TFT_ROTATION 1`, not 0 |
| Reds show as blue | Panel is RGB, TFT_eSPI defaults to BGR | Add `-DTFT_RGB_ORDER=1`. **This build needs it** — confirmed: without it the red SOS screen renders blue. The value is compared as a literal, so it must be `1`, not a `TFT_RGB` symbol (which does not exist and evaluates to 0 = BGR) |
| Whole image inverted | Panel variant | Add `-DTFT_INVERSION_ON` |
| Regular vertical stripes in the gradient | One data line not making contact | Stripe spacing names the bit: 2 px = D1, 16 px = D4 |
| Image shifted or an edge missing | Wrong driver variant or panel size | Recheck the 1a verdict |
| Random reboots, `ESP_RST_BROWNOUT` | Backlight + ESP32 exceeding the USB port | Power from a supply, not a laptop hub |
| I2C scan finds no devices at all | Missing pull-ups, or powered from the shield's own 3V3 regulator | Use the ESP32's `3V3` pin; add 4.7 kΩ from SDA/SCL to 3V3 (§5) |
| I2C scan finds 0x57 but `REV_ID` ≠ 0x15 | Knockoff or different chip behind the MAX30102 silkscreen | Trust the scan, not the label; treat unknown parts as unsupported |
| MAX30205 reads a fixed or out-of-range value | Stuck temperature register, or address collision | Check A0–A2 are low (0x48); rescan (§5) |
| Won't enter download mode | Something holding GPIO5 or GPIO15 low at boot | Both are strapping pins; unplug the shield and retry |
| Link indicator stuck on `LINK DOWN` | UART wiring fault, or nRF console still on USB CDC | Recheck TX↔RX cross and common GND (§6); confirm the nRF build moved the console off USB CDC (PLAN.md §3.3) |
| Link indicator stuck on `SEARCHING FOR NETWORK` | Wiring is fine — no peer node in range | Not a wiring fault; bring a second mesh node in range (§6) |
