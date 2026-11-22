# Sunton ESP32-8048S070C

7" 800×480 IPS, ESP32-S3-WROOM-1 N16R8, capacitive GT911 touch.

## Identity

| Property | Value |
|----------|-------|
| Module | ESP32-S3-WROOM-1 **N16R8** |
| Flash | 16 MB QIO @ 80 MHz |
| PSRAM | 8 MB octal @ **120 MHz** |
| Display | 7.0" 800×480 IPS, parallel RGB565, EK9716/ST7262 class |
| Touch | Goodix GT911, I²C `0x5D` |
| Backlight | GPIO 2, LEDC PWM |
| microSD | CMD 11 / CLK 12 / D0 13 (1-bit) — **verify on hardware** |

## Pin map

### RGB data bus

| Signal | GPIO | | Signal | GPIO | | Signal | GPIO |
|--------|------|--|--------|------|--|--------|------|
| B0 | 15 | | G0 | 9 | | R0 | 14 |
| B1 | 7 | | G1 | 46 | | R1 | 21 |
| B2 | 6 | | G2 | 3 | | R2 | 47 |
| B3 | 5 | | G3 | 8 | | R3 | 48 |
| B4 | 4 | | G4 | 16 | | R4 | 45 |
| | | | G5 | 1 | | | |

### Control

| Signal | GPIO | Notes |
|--------|------|-------|
| PCLK | 42 | 16 MHz → ~39 Hz |
| DE | 41 | |
| VSYNC | 40 | |
| HSYNC | 39 | |
| Backlight | 2 | LEDC, 5 kHz, active-high |
| Touch SDA | 19 | I²C |
| Touch SCL | 20 | I²C |
| Touch RST | 38 | |
| Touch INT | — | **not routed** → polling at 20 Hz |
| SD CMD | 11 | verify |
| SD CLK | 12 | verify |
| SD D0 | 13 | verify |

## Gotchas that cost an afternoon

- **The panel needs no init sequence.** No SPI, no command table.
- **GT911 address is `0x5D`, not `0x14`.** Copy-pasted configs often use the
  post-reset alternate address and fail silently.
- **Touch INT is not routed.** Poll; do not burn a GPIO on an interrupt.
- **The RGB bus consumes almost every GPIO.** Two pins are genuinely free.
- **Bounce-buffer parity bug:** esp-idf #19070 / IDFGH-18263 can corrupt the
  picture permanently after a lost EOF, with `esp_wifi_start()` as the reported
  trigger. See docs/FRAMEBUFFER_MEMORY.md §6.

## Unverified hardware items

Recorded in `device.json` under `verify`, and resolved on first contact with
the board. None of them block the build.

| # | Item | Default assumed |
|---|------|-----------------|
| H1 | SD pinout | CMD 11 / CLK 12 / D0 13 |
| H2 | Audio path | none (disabled in v1) |
| H3 | Panel max stable PCLK | 16 MHz |
| H4 | Revision v1.1 vs v1.3 | v1.3 timings |
| H5 | Backlight polarity | active-high |
| H6 | SD bus width | 1-bit |
