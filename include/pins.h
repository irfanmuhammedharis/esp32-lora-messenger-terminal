#pragma once
//
// Wiring for the LoRa Messenger Terminal. Every GPIO number in the project is
// written here and nowhere else.
//
// Board:   ESP32 DevKit V1, 30-pin DOIT, WROOM-32
// Display: 2.4" TFT LCD shield, Arduino Uno form factor, 8-bit parallel bus,
//          4-wire resistive touch with NO touch controller
//
// Physical header positions are in WIRING.md.
//
// ── The pin budget ──────────────────────────────────────────────────────────
// The parallel shield needs 13 GPIOs (8 data + 5 control) against ~16 usable
// bidirectional pins on this board, so the assignment below is not arbitrary:
//
//   * TFT_eSPI's ESP32 parallel driver writes the data bus through the GPIO
//     register in one shot, which only reaches pins 0-31. Every LCD pin here
//     is therefore < 32, and that is a hard constraint, not a preference.
//   * The touch panel shares four LCD lines (see below). Two of them are read
//     as analog voltages, so those two must land on ADC-capable pins.
//   * Nothing else needs a pin. Input is the touch sheet, already wired
//     inside the shield onto lines the LCD uses, so the entire user
//     interface costs zero additional GPIO.
//
// Avoided on purpose:
//   GPIO 6-11   SPI flash, and not on the 30-pin header anyway
//   GPIO 12     MTDI strapping - high at boot sets the flash to 1.8V
//   GPIO 2      onboard LED fights an input pull-up; strapping pin
//
// GPIO 1/3 (TX0/RX0) are left alone too: on this DevKit they are hard-wired
// to the onboard USB-serial bridge chip that flashing and the Serial monitor
// use. The nRF link sat there for a while and blocked both, so it moved to
// GPIO32/33 (see the UART section below).
//
// Still free: GPIO 2, 36, 39 (36/39 input-only). GPIO34 now carries the
// NEO-6M GPS - see the GPS section below.

// ── Display: 8-bit parallel data bus ────────────────────────────────────────
// NOTE the crossover: the shield's D8/D9 header pins carry LCD_D0/LCD_D1,
// while its D2..D7 pins carry LCD_D2..LCD_D7. Miswiring these two is the most
// common reason a shield shows noise instead of an image.
#define PIN_LCD_D0     13   // shield pin D8   (also touch XP)
#define PIN_LCD_D1     14   // shield pin D9   (also touch YM)
#define PIN_LCD_D2     16   // shield pin D2  - freed when the nRF link moved
                             // off UART2's default pins 16/17 (see below)
#define PIN_LCD_D3     17   // shield pin D3  - see PIN_LCD_D2 note
#define PIN_LCD_D4     23   // shield pin D4
#define PIN_LCD_D5     18   // shield pin D5
#define PIN_LCD_D6     19   // shield pin D6
#define PIN_LCD_D7      5   // shield pin D7

// ── Display: control lines ──────────────────────────────────────────────────
#define PIN_LCD_RD      4   // shield pin A0
#define PIN_LCD_WR     27   // shield pin A1
#define PIN_LCD_RS     25   // shield pin A2  - register select / DC
#define PIN_LCD_CS     26   // shield pin A3
#define PIN_LCD_RST    15   // shield pin A4

// The shield's backlight is hardwired to its own 3.3V rail. There is no
// control pin on the Uno headers, so brightness is fixed and no GPIO is
// spent on it.

// ── Touch: 4-wire resistive, no controller, shares the LCD bus ──────────────
// X is read by driving XP/XM and sampling YP; Y by driving YP/YM and sampling
// XM. So PIN_TOUCH_YP and PIN_TOUCH_XM must both be ADC-capable - GPIO26 is
// ADC2_CH9 and GPIO25 is ADC2_CH8. ADC2 is unusable while WiFi is active,
// which is fine here because this device never brings WiFi up.
//
// Because these are LCD lines, every touch read leaves the bus in the wrong
// state and the display driver has to be re-initialised for the next draw.
// Stage 2 owns that save/restore dance.
#define PIN_TOUCH_XP   PIN_LCD_D0   // GPIO13, digital drive
#define PIN_TOUCH_XM   PIN_LCD_RS   // GPIO25, ADC2_CH8  - sampled for Y
#define PIN_TOUCH_YP   PIN_LCD_CS   // GPIO26, ADC2_CH9  - sampled for X
#define PIN_TOUCH_YM   PIN_LCD_D1   // GPIO14, digital drive

// ── Input ───────────────────────────────────────────────────────────────────
// There is none beyond the touch panel above. The four push buttons that used
// to sit on GPIO32-35 were removed: GPIO34/35 are input-only with no internal
// pull-up, and left floating one of them registered an 800 ms long press by
// itself and fired a panic SOS. See PLAN.md section 5.
//
// Free as a result: GPIO32, 33, 34, 35 (32/33 fully bidirectional). GPIO35 is
// still spoken for below (PIN_MAX30102_INT), GPIO32/33 now carry the nRF
// link (UART section below), and GPIO34 the GPS (next section).

// ── UART1 from the NEO-6M GPS (receive only) ────────────────────────────────
// The pin reserved for this since the buttons came out; nothing else moved to
// make room. A GPS only needs its NMEA output wire to report position, so one
// input-only pin is enough: GpsReceiver::begin() runs
// `Serial1.begin(9600, SERIAL_8N1, PIN_GPS_RX, -1)`. GPIO34 has no internal
// pull-up, so it floats while no module is fitted - harmless, because every
// NMEA sentence carries a checksum and lib/Gps drops any that fails it.
// Configuring the module (ESP32 -> GPS RX) would need an output pin, and none
// is left; the module's power-up defaults are all this needs.
#define PIN_GPS_RX      34   // ESP32 RX  <-  NEO-6M TX

// ── UART2 link to the nRF52840 (cross-wired, plus a common ground) ──────────
// GPIO32/33, not TX0/RX0: those are hard-wired to the USB-serial bridge, and
// with the nRF on them uploads failed and the nRF radioed out the boot log.
// Pins >= 32 are fine here. The <32 rule is the LCD driver's (one 32-bit GPIO
// register write), while a UART reaches any pin through the GPIO matrix.
//
// RX is on 33 rather than input-only 34 because 33 has an internal pull-up
// (LoraLink::begin() enables it). With the nRF unplugged the line idles high
// instead of floating, so noise can't pose as nRF traffic - any complete
// line counts as proof the link is alive.
#define PIN_NRF_TX      32   // ESP32 TX  ->  nRF RX (XIAO D7)
#define PIN_NRF_RX      33   // ESP32 RX  <-  nRF TX (XIAO D6)

// ── I2C health bus: MAX30102 (HR/SpO2) + MAX30205 (body temp) ──────────────
// Wired to the ESP32's default Wire pins, GPIO21/22 - previously avoided
// because they doubled as LCD_D2/D3 (see above; the LCD lines moved to
// 16/17 to free these for I2C). Wire.begin() is still called with these
// pins explicit rather than relying on the implicit default, so pins.h
// stays the single source of truth.
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define PIN_MAX30102_INT 35   // optional; input-only pin, needs an external
                              // pull-up - the first cut polls the FIFO instead

// ── Guard against pins.h and platformio.ini drifting apart ──────────────────
#ifdef TFT_CS
static_assert(TFT_CS  == PIN_LCD_CS,  "TFT_CS build flag != PIN_LCD_CS");
static_assert(TFT_DC  == PIN_LCD_RS,  "TFT_DC build flag != PIN_LCD_RS");
static_assert(TFT_RST == PIN_LCD_RST, "TFT_RST build flag != PIN_LCD_RST");
static_assert(TFT_WR  == PIN_LCD_WR,  "TFT_WR build flag != PIN_LCD_WR");
static_assert(TFT_RD  == PIN_LCD_RD,  "TFT_RD build flag != PIN_LCD_RD");
static_assert(TFT_D0  == PIN_LCD_D0,  "TFT_D0 build flag != PIN_LCD_D0");
static_assert(TFT_D1  == PIN_LCD_D1,  "TFT_D1 build flag != PIN_LCD_D1");
static_assert(TFT_D2  == PIN_LCD_D2,  "TFT_D2 build flag != PIN_LCD_D2");
static_assert(TFT_D3  == PIN_LCD_D3,  "TFT_D3 build flag != PIN_LCD_D3");
static_assert(TFT_D4  == PIN_LCD_D4,  "TFT_D4 build flag != PIN_LCD_D4");
static_assert(TFT_D5  == PIN_LCD_D5,  "TFT_D5 build flag != PIN_LCD_D5");
static_assert(TFT_D6  == PIN_LCD_D6,  "TFT_D6 build flag != PIN_LCD_D6");
static_assert(TFT_D7  == PIN_LCD_D7,  "TFT_D7 build flag != PIN_LCD_D7");
#endif

// The parallel driver writes all 8 data lines with a single 32-bit GPIO
// register store, which cannot reach pins 32 and above.
static_assert(PIN_LCD_D0 < 32 && PIN_LCD_D1 < 32 && PIN_LCD_D2 < 32 &&
              PIN_LCD_D3 < 32 && PIN_LCD_D4 < 32 && PIN_LCD_D5 < 32 &&
              PIN_LCD_D6 < 32 && PIN_LCD_D7 < 32,
              "LCD data pins must all be GPIO < 32 for the parallel driver");
static_assert(PIN_LCD_WR < 32 && PIN_LCD_RD < 32 && PIN_LCD_RS < 32 &&
              PIN_LCD_CS < 32 && PIN_LCD_RST < 32,
              "LCD control pins must all be GPIO < 32");
