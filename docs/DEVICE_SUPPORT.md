# myFrame — Device Support

---

## 1. The problem

"Support multiple ESP32 devices" usually decays into `#ifdef BOARD_X` sprinkled through the code.
That is not support, it is a fork. myFrame's rule is: **the firmware image is identical across
devices; only a board description changes.**

Concretely, adding a board must require:

1. A new `devices/<id>/` directory (copied from `devices/_template/`)
2. One line in `devices/index.json`

…nothing else. No edits to `components/`, no edits to `main/`. This is acceptance criterion **A2**,
and it is verified by actually adding a second board during Phase 8.

---

## 2. What a board definition contains

```
devices/sunton_esp32_8048s070/
├── device.json            # declarative: identity, capabilities, limits
├── board.h                # pin map + constants (pure data)
├── board.c                # frame_board_init(): buses, GPIOs, panel, touch
├── display.c              # panel timings + buffer profile for this panel
├── touch.c                # touch controller bring-up (delegates to frame_input)
├── audio.c                # optional codec / I2S
├── sdkconfig.defaults     # PSRAM speed, flash size, LVGL tuning
├── partitions.csv         # flash layout for this device
└── README.md              # photos, pinout, revision notes, gotchas
```

`device.json` is consumed by the build script, which emits `frame_board.h` with the resolved
constants. `board.c` is compiled into the image and produces the single `frame_board_t` instance
that the rest of the system consumes.

```c
typedef struct {
    const char *id;
    const char *revision;
    frame_chip_t chip;
    frame_flash_geom_t flash;
    frame_psram_geom_t psram;

    const frame_display_desc_t *display;   // timings, pins, buffer profile
    const frame_touch_desc_t   *touch;     // controller, bus, addr, axes mapping
    const frame_audio_desc_t   *audio;     // may be NULL

    esp_err_t (*init)(void);               // buses, GPIO, panel, touch
    void      (*backlight_set)(uint8_t pct);
    void      (*enter_sleep)(void);
    void      (*exit_sleep)(void);
} frame_board_t;

extern const frame_board_t *frame_board(void);
```

Every board must provide this symbol. There is exactly one board linked into any image.

---

## 3. Build flow

```bash
# list boards
python3 tools/build.py --list

# build the shell for one board
python3 tools/build.py --board sunton_esp32_8048s070

# build + flash + open a serial monitor
python3 tools/build.py --board sunton_esp32_8048s070 --flash --monitor

# build a standalone app package instead of the shell
python3 tools/build.py --board sunton_esp32_8048s070 --app clock --release

# raw ESP-IDF, if you prefer
idf.py -DBOARD=sunton_esp32_8048s070 -B build/sunton_esp32_8048s070 build
```

`tools/build.py` does four things:

1. Reads `devices/index.json`, resolves the board id, validates `device.json`.
2. Selects `sdkconfig.defaults.<board>` and `partitions.csv` via CMake variables.
3. Generates `frame_board.h` from `device.json`.
4. Invokes `idf.py` with a per-board build directory (so boards can be built in parallel).

CI runs the matrix over every entry in `devices/index.json`.

---

## 4. Capability negotiation

Not every board has touch, audio, or an SD slot. Apps declare what they need in their manifest;
the store filters incompatible apps before the user ever sees them.

```json
// app manifest
{
  "requires": {
    "chip":     ["esp32s3"],
    "display":  { "min_width": 480, "min_height": 320, "touch": true },
    "psram_mb": 4,
    "audio":    false
  }
}
```

At install time the device re-checks every field against its own `device.json` — the store's
filter is a UX convenience, not a security boundary.

---

## 5. Target: Sunton ESP32-8048S070

The first supported board. A cheap, widely available 7" dev board — and a genuinely good test of
the architecture, because its RGB panel consumes nearly every GPIO.

> **Variant confirmed:** `-C` — capacitive touch via GT911 on I²C at `0x5D`. The board does **not**
> route the GT911 INT line, so the input layer polls at 20 Hz rather than using interrupts.

### 5.1 Identity

| Property | Value |
|----------|-------|
| Board | Sunton ESP32-8048S070 (7" RGB panel) |
| Variants | `-N` (no touch), `-R` (resistive), `-C` (capacitive GT911) — **target confirmed: `-C`** |
| Module | ESP32-S3-WROOM-1 **N16R8** |
| CPU | Xtensa LX7 dual-core @ 240 MHz |
| Internal SRAM | 512 KB |
| Flash | 16 MB (QIO) |
| PSRAM | 8 MB octal (OPI) — **run at 120 MHz** |
| Display | 7.0" 800×480 IPS, parallel RGB565 |
| Panel controller | EK9716 / ST7262 class — *dumb RGB, no init sequence, no GRAM* |
| Touch | Goodix GT911, I²C @ 0x5D, polling |
| Backlight | GPIO 2, PWM via LEDC |
| Extras | microSD, speaker, USB-C (CH340 UART) |

### 5.2 Pin map

**RGB data bus (16-bit RGB565)**

| Signal | GPIO | | Signal | GPIO | | Signal | GPIO |
|--------|------|--|--------|------|--|--------|------|
| R0 | 14 | | G0 | 9 | | B0 | 15 |
| R1 | 21 | | G1 | 46 | | B1 | 7 |
| R2 | 47 | | G2 | 3 | | B2 | 6 |
| R3 | 48 | | G3 | 8 | | B3 | 5 |
| R4 | 45 | | G4 | 16 | | B4 | 4 |
| | | | G5 | 1 | | | |

**Control & touch**

| Signal | GPIO | Notes |
|--------|------|-------|
| PCLK | 42 | Pixel clock |
| DE | 41 | Data enable |
| VSYNC | 40 | |
| HSYNC | 39 | |
| Backlight | 2 | LEDC PWM |
| Touch SDA | 19 | I²C |
| Touch SCL | 20 | I²C |
| Touch RST | 38 | |
| Touch INT | — | **Not connected on this board** → driver must poll |

### 5.3 Panel timing

```
pclk_hz            16 000 000
hsync_pulse_width           4
hsync_back_porch            8
hsync_front_porch           8
vsync_pulse_width           4
vsync_back_porch            8
vsync_front_porch           8
```

→ 820 × 500 total, **≈ 39 Hz** refresh. Acceptable for a static dashboard; raising PCLK to
20–24 MHz gets ~50–60 Hz but costs PSRAM bandwidth (see [Display Driver](DISPLAY_DRIVER.md#4-psram-bandwidth-is-the-real-budget)).
The value is a per-board constant so it can be tuned without touching code.

### 5.4 Notes that will save you an afternoon

- **The panel needs no init sequence.** No SPI, no command table. Power, timing and DE are enough.
- **GT911 address is `0x5D`, not `0x14`.** Many copy-pasted configs use `0x14` (the alternate
  address after a reset strap) and fail silently.
- **Touch INT is not routed.** Plan for polling. Do not burn a GPIO on an interrupt you cannot read.
- **The RGB bus eats almost every GPIO.** Free pins after display + touch + SD: roughly two.
  Anything else must live behind I²C on the shared connector.
- **Revision differences (v1.1 vs v1.3) are real** — panel timings and some pin assignments differ.
  `device.json` carries a `revision` field; ship both, verify on hardware.

---

## 6. Open hardware questions (verify in Phase 1, day one)

These are marked in `device.json` with `"verify": true` and validated by a hardware checklist
before Phase 1 is considered done.

| # | Question | Why it matters | Default assumed |
|---|----------|----------------|-----------------|
| H1 | **SD card pinout** on this exact revision | Needed for offline install / asset storage | CMD 11, CLK 12, D0 13 (SDMMC 1-bit) — **must confirm** |
| H2 | **Audio path** — PCM5101 I²S DAC, or ES8311/ES8388 + I²S codec? | Codec needs I²C config + MCLK; DAC needs only I²S | I²S DAC, optional |
| H3 | **Panel max PCLK** and whether 800×480 @ 20 MHz is stable | Determines achievable refresh rate and PSRAM budget | 16 MHz |
| H4 | **Revision v1.1 vs v1.3** differences | Different init timing on some units | v1.3 |
| H5 | **Backlight polarity and max PWM frequency** | Wrong polarity = black screen with a lit panel | Active-high, 5 kHz |
| H6 | Whether the SD slot is 1-bit or 4-bit capable | 4-bit is faster for app installs | 1-bit |

---

## 7. Adding the next board

`devices/_template/` is a working board definition with every pin marked `TODO`.
The checklist for a new board:

- [ ] Copy `_template` → `devices/<id>/`
- [ ] Fill `device.json` (identity, chip, flash/PSRAM sizes, display descriptor)
- [ ] Fill `board.h` pin map
- [ ] Replace `display.c` timings with the panel's datasheet values
- [ ] Choose a buffer profile (`balanced` / `lowmem` / `direct`) in `device.json`
- [ ] Choose `partitions.csv` appropriate to the flash size
- [ ] Add the board to `devices/index.json`
- [ ] Build, flash, run the display smoke test
- [ ] Write the `README.md` with pinout photos and gotchas

Target: **under an hour**, assuming the panel datasheet is in hand.
