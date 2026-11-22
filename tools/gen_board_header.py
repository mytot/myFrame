#!/usr/bin/env python3
"""
gen_board_header.py — turn devices/<board>/device.json into a C header.

Run at CMake configure time so the declarative board description is the single
source of truth. The generated header is written into the build directory and
never committed.

    gen_board_header.py --device devices/foo/device.json --out build/foo/frame_board.h
"""
from __future__ import annotations

import argparse
import json
import os
import sys

VALID_CHIPS = {"esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6", "esp32p4"}
VALID_PROFILES = {"balanced", "lowmem", "raster8", "direct", "zerocopy"}
VALID_TOUCH = {"none", "gt911"}
VALID_AUDIO = {"none", "i2s_dac", "es8311"}

TIMING_KEYS = (
    "hsync_pulse_width", "hsync_back_porch", "hsync_front_porch",
    "vsync_pulse_width", "vsync_back_porch", "vsync_front_porch",
)


def fail(msg: str) -> None:
    print(f"gen_board_header: error: {msg}", file=sys.stderr)
    sys.exit(1)


def as_int(v, name: str, lo=None, hi=None) -> int:
    if isinstance(v, bool) or not isinstance(v, int):
        try:
            v = int(str(v), 0)
        except (TypeError, ValueError):
            fail(f"{name}: expected an integer, got {v!r}")
    if lo is not None and v < lo:
        fail(f"{name}: {v} is below the minimum of {lo}")
    if hi is not None and v > hi:
        fail(f"{name}: {v} is above the maximum of {hi}")
    return v


def as_gpio(v, name: str) -> int:
    """GPIO numbers; -1 means 'not connected'. Accepts the string "-1"."""
    if isinstance(v, str):
        s = v.strip().lower()
        if s in ("-1", "none", "nc", "null"):
            return -1
    n = as_int(v, name, -1, 48)
    return n


def as_str(v, name: str, maxlen: int) -> str:
    if not isinstance(v, str):
        fail(f"{name}: expected a string, got {v!r}")
    if not v:
        fail(f"{name}: must not be empty")
    if len(v) > maxlen:
        fail(f"{name}: longer than {maxlen} characters")
    if any(c in v for c in '"\\\n\r'):
        fail(f"{name}: contains a character that cannot be emitted into a C string")
    return v


def parse_i2c_addr(v, name: str) -> int:
    if isinstance(v, str):
        s = v.strip().lower().replace("0x", "")
        try:
            return int(s, 16)
        except ValueError:
            fail(f"{name}: {v!r} is not a hex I2C address")
    return as_int(v, name, 0x08, 0x77)


def check_pins_unique(pins: dict[str, int], board: str) -> None:
    seen: dict[int, str] = {}
    for name, num in pins.items():
        if num < 0:
            continue
        if num in seen:
            fail(f"{board}: GPIO {num} is assigned to both {seen[num]} and {name}")
        seen[num] = name


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True, help="path to device.json")
    ap.add_argument("--out", required=True, help="output header path")
    args = ap.parse_args()

    try:
        with open(args.device, "r", encoding="utf-8") as fh:
            dev = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read {args.device}: {exc}")

    board_id = as_str(dev.get("id"), "id", 48)
    revision = as_str(dev.get("revision", "1.0"), "revision", 16)
    chip = as_str(dev.get("chip"), "chip", 16).lower()
    if chip not in VALID_CHIPS:
        fail(f"chip: {chip!r} is not one of {sorted(VALID_CHIPS)}")

    flash_mb = as_int(dev.get("flash_mb"), "flash_mb", 1, 1024)
    psram_mb = as_int(dev.get("psram_mb"), "psram_mb", 0, 1024)

    disp = dev.get("display")
    if not isinstance(disp, dict):
        fail("display: section is required")
    if str(disp.get("interface", "")).lower() not in ("rgb565", "rgb"):
        fail("display.interface: only 'rgb565' is implemented in v1")

    width = as_int(disp.get("width"), "display.width", 1, 4096)
    height = as_int(disp.get("height"), "display.height", 1, 4096)
    pclk = as_int(disp.get("pclk_hz"), "display.pclk_hz", 1_000_000, 100_000_000)
    profile = str(disp.get("profile", "balanced")).lower()
    if profile not in VALID_PROFILES:
        fail(f"display.profile: {profile!r} is not one of {sorted(VALID_PROFILES)}")

    timings = disp.get("timings", {})
    if not isinstance(timings, dict):
        fail("display.timings: expected an object")
    for key in TIMING_KEYS:
        if key not in timings:
            fail(f"display.timings.{key}: missing")

    # Derived refresh rate: pclk / (h_total * v_total).
    h_total = width + sum(as_int(timings[k], f"timings.{k}", 0, 4096)
                          for k in ("hsync_pulse_width", "hsync_back_porch", "hsync_front_porch"))
    v_total = height + sum(as_int(timings[k], f"timings.{k}", 0, 4096)
                           for k in ("vsync_pulse_width", "vsync_back_porch", "vsync_front_porch"))
    if h_total <= 0 or v_total <= 0:
        fail("display: total timing is zero")
    refresh_hz = pclk // (h_total * v_total)
    if refresh_hz < 20:
        print(f"gen_board_header: warning: refresh rate is {refresh_hz} Hz (< 20)", file=sys.stderr)

    pins: dict[str, int] = {}
    for name in ("pclk_gpio", "de_gpio", "vsync_gpio", "hsync_gpio", "backlight_gpio"):
        pins[name] = as_gpio(disp.get(name, -1), f"display.{name}")

    data = disp.get("data_gpio", [])
    if not isinstance(data, list) or len(data) not in (16, 8):
        fail("display.data_gpio: expected a list of 16 (RGB565) or 8 pins")
    data_pins = [as_gpio(p, f"display.data_gpio[{i}]") for i, p in enumerate(data)]
    for i, p in enumerate(data_pins):
        pins[f"data_gpio[{i}]"] = p

    bl_freq = as_int(disp.get("backlight_freq_hz", 5000), "display.backlight_freq_hz", 100, 100_000)
    bl_pct = as_int(disp.get("backlight_default_pct", 80), "display.backlight_default_pct", 0, 100)
    bl_active_high = bool(disp.get("backlight_active_high", True))

    touch = dev.get("touch", {"controller": "none"})
    if not isinstance(touch, dict):
        fail("touch: expected an object")
    touch_ctrl = str(touch.get("controller", "none")).lower()
    if touch_ctrl not in VALID_TOUCH:
        fail(f"touch.controller: {touch_ctrl!r} is not one of {sorted(VALID_TOUCH)}")

    touch_pins: dict[str, int] = {}
    touch_addr = 0
    touch_poll = 20
    touch_inv_x = touch_inv_y = touch_swap = False
    touch_w, touch_h = width, height
    touch_port = 0
    if touch_ctrl != "none":
        touch_pins["touch_sda_gpio"] = as_gpio(touch.get("sda_gpio", -1), "touch.sda_gpio")
        touch_pins["touch_scl_gpio"] = as_gpio(touch.get("scl_gpio", -1), "touch.scl_gpio")
        touch_pins["touch_rst_gpio"] = as_gpio(touch.get("rst_gpio", -1), "touch.rst_gpio")
        touch_pins["touch_int_gpio"] = as_gpio(touch.get("int_gpio", -1), "touch.int_gpio")
        if touch_pins["touch_sda_gpio"] < 0 or touch_pins["touch_scl_gpio"] < 0:
            fail("touch: sda_gpio and scl_gpio are required when a controller is present")
        touch_addr = parse_i2c_addr(touch.get("i2c_addr", "0x5D"), "touch.i2c_addr")
        touch_poll = as_int(touch.get("poll_hz", 20), "touch.poll_hz", 1, 200)
        touch_inv_x = bool(touch.get("invert_x", False))
        touch_inv_y = bool(touch.get("invert_y", False))
        touch_swap = bool(touch.get("swap_xy", False))
        touch_w = as_int(touch.get("width", width), "touch.width", 1, 4096)
        touch_h = as_int(touch.get("height", height), "touch.height", 1, 4096)
        touch_port = as_int(touch.get("i2c_port", 0), "touch.i2c_port", 0, 2)

    audio = dev.get("audio", {"codec": "none"})
    if not isinstance(audio, dict):
        fail("audio: expected an object")
    audio_codec = str(audio.get("codec", "none")).lower()
    if audio_codec not in VALID_AUDIO:
        fail(f"audio.codec: {audio_codec!r} is not one of {sorted(VALID_AUDIO)}")
    audio_pins: dict[str, int] = {}
    audio_rate = 16000
    if audio_codec != "none":
        audio_pins["audio_bclk"] = as_gpio(audio.get("bclk_gpio", -1), "audio.bclk_gpio")
        audio_pins["audio_lrck"] = as_gpio(audio.get("lrck_gpio", -1), "audio.lrck_gpio")
        audio_pins["audio_dout"] = as_gpio(audio.get("dout_gpio", -1), "audio.dout_gpio")
        audio_pins["audio_mclk"] = as_gpio(audio.get("mclk_gpio", -1), "audio.mclk_gpio")
        audio_rate = as_int(audio.get("sample_rate", 16000), "audio.sample_rate", 8000, 96000)

    sd = dev.get("sdcard", {"present": False})
    if not isinstance(sd, dict):
        fail("sdcard: expected an object")
    sd_present = bool(sd.get("present", False))
    sd_pins: dict[str, int] = {}
    sd_mount = "/sdcard"
    sd_mode = "sdmmc_1bit"
    if sd_present:
        sd_pins["sd_cmd"] = as_gpio(sd.get("cmd_gpio", -1), "sdcard.cmd_gpio")
        sd_pins["sd_clk"] = as_gpio(sd.get("clk_gpio", -1), "sdcard.clk_gpio")
        sd_pins["sd_d0"] = as_gpio(sd.get("d0_gpio", -1), "sdcard.d0_gpio")
        if sd_pins["sd_cmd"] < 0 or sd_pins["sd_clk"] < 0 or sd_pins["sd_d0"] < 0:
            fail("sdcard: cmd_gpio, clk_gpio and d0_gpio are required when present")
        sd_mount = as_str(sd.get("mount_point", "/sdcard"), "sdcard.mount_point", 32)
        sd_mode = str(sd.get("mode", "sdmmc_1bit")).lower()
        if sd_mode not in ("sdmmc_1bit", "sdmmc_4bit", "spi"):
            fail(f"sdcard.mode: {sd_mode!r} is not one of sdmmc_1bit/sdmmc_4bit/spi")

    # A pin may drive only one function.
    check_pins_unique({**pins, **touch_pins, **audio_pins, **sd_pins}, board_id)

    chip_enum = "FRAME_CHIP_" + chip.upper()
    profile_enum = "FRAME_DISPLAY_PROFILE_" + profile.upper()
    touch_enum = "FRAME_TOUCH_" + ("NONE" if touch_ctrl == "none" else touch_ctrl.upper())
    audio_enum = "FRAME_AUDIO_" + ("NONE" if audio_codec == "none" else audio_codec.upper())

    L: list[str] = []
    A = L.append
    A("/*")
    A(f" * Auto-generated from devices/{board_id}/device.json by tools/gen_board_header.py.")
    A(" * DO NOT EDIT — edit the JSON and re-run the build.")
    A(" */")
    A("#pragma once")
    A("")
    A('#include "frame_core/board_types.h"')
    A("")
    A("#define FRAME_BOARD_ID           " + json.dumps(board_id))
    A("#define FRAME_BOARD_REVISION     " + json.dumps(revision))
    A(f"#define FRAME_BOARD_CHIP         {chip_enum}")
    A(f"#define FRAME_BOARD_FLASH_MB     {flash_mb}u")
    A(f"#define FRAME_BOARD_PSRAM_MB     {psram_mb}u")
    A("")
    A("/* Display */")
    A(f"#define FRAME_DISPLAY_WIDTH        {width}u")
    A(f"#define FRAME_DISPLAY_HEIGHT       {height}u")
    A(f"#define FRAME_DISPLAY_PCLK_HZ      {pclk}u")
    A(f"#define FRAME_DISPLAY_REFRESH_HZ   {refresh_hz}u")
    A(f"#define FRAME_DISPLAY_PROFILE      {profile_enum}")
    A(f"#define FRAME_DISPLAY_HSYNC_PW     {as_int(timings['hsync_pulse_width'], 'x')}u")
    A(f"#define FRAME_DISPLAY_HSYNC_BP     {as_int(timings['hsync_back_porch'], 'x')}u")
    A(f"#define FRAME_DISPLAY_HSYNC_FP     {as_int(timings['hsync_front_porch'], 'x')}u")
    A(f"#define FRAME_DISPLAY_VSYNC_PW     {as_int(timings['vsync_pulse_width'], 'x')}u")
    A(f"#define FRAME_DISPLAY_VSYNC_BP     {as_int(timings['vsync_back_porch'], 'x')}u")
    A(f"#define FRAME_DISPLAY_VSYNC_FP     {as_int(timings['vsync_front_porch'], 'x')}u")
    A(f"#define FRAME_DISPLAY_PCLK_GPIO    {pins['pclk_gpio']}")
    A(f"#define FRAME_DISPLAY_DE_GPIO      {pins['de_gpio']}")
    A(f"#define FRAME_DISPLAY_VSYNC_GPIO   {pins['vsync_gpio']}")
    A(f"#define FRAME_DISPLAY_HSYNC_GPIO   {pins['hsync_gpio']}")
    A(f"#define FRAME_DISPLAY_BACKLIGHT_GPIO {pins['backlight_gpio']}")
    A(f"#define FRAME_DISPLAY_BACKLIGHT_FREQ_HZ {bl_freq}u")
    A(f"#define FRAME_DISPLAY_BACKLIGHT_DEFAULT_PCT {bl_pct}u")
    A("#define FRAME_DISPLAY_BACKLIGHT_ACTIVE_HIGH " + ("1" if bl_active_high else "0"))
    A(f"#define FRAME_DISPLAY_DATA_WIDTH  {len(data_pins)}u")
    A("#define FRAME_DISPLAY_DATA_GPIO { " + ", ".join(str(p) for p in data_pins) + " }")
    A("")
    A("/* Touch */")
    A(f"#define FRAME_TOUCH_CONTROLLER     {touch_enum}")
    A(f"#define FRAME_TOUCH_I2C_PORT       {touch_port}")
    A(f"#define FRAME_TOUCH_I2C_ADDR       0x{touch_addr:02X}u")
    A(f"#define FRAME_TOUCH_POLL_HZ        {touch_poll}u")
    A(f"#define FRAME_TOUCH_INVERT_X       {1 if touch_inv_x else 0}")
    A(f"#define FRAME_TOUCH_INVERT_Y       {1 if touch_inv_y else 0}")
    A(f"#define FRAME_TOUCH_SWAP_XY        {1 if touch_swap else 0}")
    A(f"#define FRAME_TOUCH_WIDTH          {touch_w}u")
    A(f"#define FRAME_TOUCH_HEIGHT         {touch_h}u")
    for key in ("touch_sda_gpio", "touch_scl_gpio", "touch_rst_gpio", "touch_int_gpio"):
        if key in touch_pins:
            A(f"#define FRAME_{key.upper()}  {touch_pins[key]}")
    A("")
    A("/* Audio */")
    A(f"#define FRAME_AUDIO_CODEC          {audio_enum}")
    A(f"#define FRAME_AUDIO_SAMPLE_RATE    {audio_rate}u")
    for key in ("audio_bclk", "audio_lrck", "audio_dout", "audio_mclk"):
        if key in audio_pins:
            A(f"#define FRAME_{key.upper()}  {audio_pins[key]}")
    A("")
    A("/* microSD */")
    A("#define FRAME_SDCARD_PRESENT      " + ("1" if sd_present else "0"))
    A(f"#define FRAME_SDCARD_MOUNT        {json.dumps(sd_mount)}")
    A(f"#define FRAME_SDCARD_MODE         {json.dumps(sd_mode)}")
    for key in ("sd_cmd", "sd_clk", "sd_d0"):
        if key in sd_pins:
            A(f"#define FRAME_{key.upper()}_GPIO  {sd_pins[key]}")
    A("")

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")

    print(f"gen_board_header: {board_id} {revision} {chip} "
          f"{width}x{height}@{refresh_hz}Hz profile={profile}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
