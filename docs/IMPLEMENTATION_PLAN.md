# myFrame — Implementation Plan

> **Status:** Draft v2 — decisions from review incorporated. **No code is written until this plan is confirmed.**
> **Stack:** ESP-IDF **v6.1.1** · ESP32-S3 · LVGL 9.x · FreeRTOS (IDF builtin)
> **Target board:** Sunton **ESP32-8048S070C** (capacitive GT911)

---

## 1. Goal

Build a **generic ESP32 firmware platform** ("FrameOS") that:

1. Runs on **multiple ESP32 devices**, each described entirely by a board definition in this repo.
2. Ships first-class support for the **Sunton ESP32-8048S070C** (7" 800×480 RGB, ESP32-S3 N16R8).
3. Builds a **single, generic firmware image** for every supported device — device differences are data, not code forks.
4. **Boots into a launcher** that is the home screen: every installed app is shown there, and the
   **app store is a first-class tile** on it.
5. Provides an **app store** where the user browses, installs new apps, and updates existing ones —
   all from the launcher, no reflash.
6. Supports **two app models in v1**: native (reboot-switched firmware images) and managed
   (WebAssembly, hot-loaded with no reboot).
7. Ships with built-in **Logs** and **Settings** apps.
8. Uses a **resource-efficient display driver** written in-repo rather than a copy-pasted vendor example.

Non-goals for v1: Android/iOS companion app, cloud account system, DRM/paid apps, multi-user.

---

## 2. Guiding decisions

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | **ESP-IDF v6.1.1** (v6.0 minimum) | v6 is current stable; Picolibc default, PSA Crypto, legacy drivers removed, warnings-as-errors. Pinning avoids a later migration. |
| D2 | **LVGL 9.x** as the UI toolkit | Mature, PSRAM-friendly, permissive licence, active maintenance, huge widget set. Building a custom renderer is not a good use of time. |
| D3 | **Two app execution models, both in v1** — *native* (separate firmware image, reboot-switched) and *managed* (WebAssembly, in-process) | Native gives full performance and perfect isolation; WASM gives true hot-loading and a sandbox. Neither alone covers every app. See [App Store](APP_STORE.md) and [App SDK](APP_SDK.md). |
| D4 | **Native apps live in real OTA slots**, not custom partition types | Stock bootloader + `esp_ota_set_boot_partition()` already do image validation, A/B sequencing and rollback. Forking the bootloader is a permanent maintenance tax. |
| D5 | **Render in SRAM, copy sequentially to PSRAM** — never render into the PSRAM framebuffer | LVGL in `DIRECT` mode does scattered read-modify-write against the slowest bus in the system. Rendering into SRAM and copying the dirty rect makes PSRAM traffic proportional to the invalidated area, not the screen. This is the single biggest performance decision in the project — see [Render Performance](RENDER_PERFORMANCE.md). |
| D5b | **Display driver is a thin, in-repo wrapper over `esp_lcd`** with four memory profiles | No reason to reimplement the LCD_CAM peripheral; every reason to own the buffer strategy, pacing and power behaviour. |
| D6 | **All packages signed (Ed25519 via PSA Crypto)**, with an explicit *dev mode* switch | Signing is nearly free to add now and painful to retrofit. Dev mode keeps the inner loop fast. |
| D7 | **Board = data** (`device.json` + `board.c`), selected at build time | Keeps "generic firmware" honest: no `#if BOARD_XXX` in app code. |
| D8 | **WASM apps use a retained-mode UI tree API**, not raw LVGL pointers | Pointer-level ABI is unstable and unsafe across a sandbox boundary. A declarative tree is small, stable, and diffable. |
| D9 | **The launcher is the home screen** — boot lands on it, it shows every installed app, and the app store is a first-class tile | This is the user's stated requirement and it simplifies everything else: no separate home flow, no separate store entry point. |
| D10 | **Dev store first, static catalog second** | Iterate on the package format and install flow against a local server before committing to a hosting topology. The device-side client is identical either way. |

---

## 3. Scope by phase

Estimates assume one engineer, hardware on desk.

| Phase | Scope | Est. | Exit criteria |
|-------|-------|------|---------------|
| **0. Scaffolding** | ESP-IDF v6.1.1 project, device registry, build script, CI build matrix, Kconfig skeleton | 0.5–1 d | `idf.py -DBOARD=sunton_esp32_8048s070 build` produces a bootable image; CI green for 1 board |
| **1. Device + display** | 8048S070 board definition, `frame_display` (4 profiles, `balanced` default), `frame_input` (GT911), backlight PWM, **two-core schedule**, **render benchmark harness** | 3–4 d | LVGL hello world renders, touch tracks a finger, no tearing in a 10 000-row scroll at 30 fps, **and the three measurements in Render Performance §6 are recorded** |
| **2. FrameOS core** | Logger ring buffer, settings store, event bus, app manager, lifecycle, crash recovery | 2–3 d | A crashing app returns to a working shell; settings survive reboot |
| **3. Launcher + shell UX** | Home screen, app grid, paging, status bar, update badges, global home gesture | 1.5–2 d | Boot lands on an interactive launcher in ≤ 3 s |
| **4. Built-in apps** | Settings, Logs | 2 d | Wi-Fi connect, brightness change, log filter + export all work end-to-end |
| **5. App SDK + packaging** | `frame_app` C API, native app template, `pack.py` + signing, slot manager, boot handoff | 2–3 d | A native sample app installs, launches, and exits back to the launcher |
| **6. Store: client + dev server** | Catalog fetch/verify, install/update/uninstall, store UI (Installed / Updates / All), `devstore.py`, device HTTP API | 3–4 d | Browse → install → launch → update → uninstall over LAN, surviving a power cut mid-install |
| **7. Managed (WASM) runtime** | wasm3 integration, retained UI tree API, host imports, sample WASM app, hot load/unload | 3–4 d | A WASM app installs and runs with no reboot, and is contained when it traps |
| **8. Hardening** | FrameOS self-OTA (A/B + rollback), memory/power tuning, profiling, static catalog hosting, docs, release artefacts | 2–3 d | 24 h soak stable; memory high-water mark documented; release pipeline produces signed images |

**v1 total: roughly 3–4 weeks.** Both app models are in scope. Phase 7 is the only phase that can
be cut without affecting the launcher → store → install → update loop.

---

## 4. Deliverables

### Firmware
- `components/frame_*` — 10 focused components (see [Architecture](ARCHITECTURE.md#4-component-map))
- `components/frame_apps_builtin/launcher/` — the home screen (see [Launcher](LAUNCHER.md))
- `devices/sunton_esp32_8048s070/` — complete board definition
- `devices/_template/` — copy-and-fill template for the next board
- `apps/` — sample native app + sample managed (WASM) app

### Tooling
- `tools/build.py` — `--board <id> [--app <id>] [--release]`
- `tools/pack.py` — builds a signed `.fpack` from a built app; `--verify` runs the device's verifier on the host
- `tools/catalog.py` — builds + signs `catalog.json`
- `tools/devstore.py` — **zero-dependency local store server**; serves the catalog, the packages, and a browser UI for browsing/installing/updating from a laptop

### Device-side HTTP API (dev store)
- `GET  /api/apps`            — list installed apps with versions and update flags
- `POST /api/apps/install`    — upload a `.fpack`, verify, install (multipart)
- `POST /api/apps/<id>/update`— fetch + install the newest catalog version
- `POST /api/apps/<id>/uninstall`
- `GET  /api/catalog`         — current catalog, verified
- `POST /api/catalog/refresh` — force a re-fetch

### CI
- Build matrix over every device in `devices/index.json`
- Signed release artefacts on tag
- Host-side unit tests for pure-logic components (slot allocator, package verifier, semver compare)

### Repository layout

```
myFrame/
├── CMakeLists.txt              # single ESP-IDF project; BOARD selected via -DBOARD=
├── sdkconfig.defaults          # cross-board defaults
├── docs/                       # this documentation set
│
├── devices/
│   ├── index.json              # the board registry — one line per supported board
│   ├── _template/              # copy-and-fill for a new board
│   └── sunton_esp32_8048s070/  # the first supported board
│       ├── device.json         # declarative identity + capabilities
│       ├── board.h / board.c   # pin map + frame_board_init()
│       ├── display.c           # panel timings, buffer profile
│       ├── touch.c             # GT911 bring-up
│       ├── audio.c             # optional
│       ├── sdkconfig.defaults  # PSRAM 120 MHz, flash 16 MB, LVGL tuning
│       ├── partitions.csv      # 16 MB layout
│       └── README.md           # pinout, photos, revision notes
│
├── components/
│   ├── frame_core/             # event bus, app manager, lifecycle, watchdog
│   ├── frame_display/          # the resource-efficient display driver
│   ├── frame_input/            # touch, buttons, gestures
│   ├── frame_ui/               # theme, nav stack, widgets
│   ├── frame_storage/          # NVS settings, FatFS assets, app registry
│   ├── frame_net/              # Wi-Fi, provisioning, HTTP
│   ├── frame_ota/              # slots, image write, boot switch, rollback
│   ├── frame_store/            # catalog, install/update/uninstall, dev API
│   ├── frame_log/              # ring buffer logger
│   ├── frame_app/              # the public SDK + ABI
│   ├── frame_wasm/             # wasm3 runtime + host imports
│   └── frame_apps_builtin/
│       ├── launcher/           # the home screen
│       ├── store/              # browse / install / update
│       ├── settings/
│       └── logs/
│
├── apps/
│   ├── helloworld_native/      # sample native app
│   └── clock_wasm/             # sample managed app
│
├── tools/
│   ├── build.py                # --board --app --release
│   ├── pack.py                 # build + sign .fpack; --verify
│   ├── catalog.py              # build + sign catalog.json
│   └── devstore.py             # local store server + browser UI
│
└── .github/workflows/          # build matrix over devices/, signed releases
```

---

## 5. Acceptance criteria (v1)

| ID | Criterion |
|----|-----------|
| A1 | `idf.py -DBOARD=<any supported board> build` succeeds for every board in the registry with no source edits |
| A2 | Adding a board requires only a new `devices/<id>/` directory + one line in `devices/index.json` |
| A3 | Boot to launcher in **≤ 3 s** on the 8048S070 |
| A4 | Display subsystem uses **≤ 120 KB internal SRAM** and **≤ 1.6 MB PSRAM** on the default profile |
| A5 | No visible tearing while scrolling a long list on the default profile |
| A6 | Settings changes persist across reboot; Wi-Fi reconnects automatically |
| A7 | Logs view shows ≥ 2 000 entries with live tail, level/tag filter, and export to SD + HTTP |
| A8 | A signed native app installs, launches, and exits to the launcher in **≤ 1.5 s** end-to-end |
| A9 | An app update replaces the app without touching FrameOS; a failed update leaves the old version running |
| A10 | Power loss at *any* point during install/update never bricks the device |
| A11 | Uninstall reclaims its slot and its asset directory |
| A12 | FrameOS self-OTA with rollback: a bad FrameOS build reverts to the previous one |
| A13 | **Launcher is the home screen** — boot lands on it in ≤ 3 s, every installed app is visible with icon + name + version, and the store is reachable in one tap |
| A14 | Apps with a pending update show a **badge on the launcher** without the store being opened |
| A15 | A **managed (WASM) app** installs and launches with **no reboot** in ≤ 500 ms |
| A16 | A trapped or looping WASM app is **contained** — the shell survives, the app is marked failed |
| A17 | The **dev store** works over LAN from a laptop browser: browse, install, update, uninstall |
| A18 | Global home gesture (swipe up from bottom) returns to the launcher from inside any app, including native ones |
| **A19** | A `light`-class app animating a **200×200 region sustains 30 fps** with zero dropped frames over 5 minutes |
| **A20** | A 200×200 widget update costs **≤ 3 ms** end-to-end; PSRAM traffic is proportional to the invalidated area, not the screen size |
| **A21** | A `medium`-class app scrolling a **10 000-row virtual list sustains 30 fps** with **core 1 load < 60 %** |
| **A22** | Wi-Fi sustains **≥ 5 MB/s** download while the launcher animates, with **< 1 dropped frame per second** |
| **A23** | A 1.5 MB native app install never blocks the UI for more than **50 ms** |
| **A24** | The three hardware measurements in Render Performance §6 are **recorded with real numbers** before Phase 1 is signed off |
| **A25** | `CONFIG_FREERTOS_UNICORE` is **n** and a startup assertion verifies every task's core affinity against the schedule |
| **A26** | The **bounce-buffer parity bug** (esp-idf #19070 / IDFGH-18263) is tested on v6.1.1 with Wi-Fi starting during panel operation; result and workaround recorded. **Phase 1 blocker** |
| **A27** | If `raster8` is implemented: ISR load ≤ 15 % of a core, panel PSRAM traffic ≤ 16 MB/s, measured not assumed |
| **A28** | Image assets are **streamed, not fully cached**; decoded-image cache capped at 768 KB with LRU eviction |
| **A29** | On a 4 MB PSRAM board, `lowmem` boots and runs the launcher within the same SRAM budget as `balanced` |

---

## 6. Resolved decisions

These were open questions in the draft; they are now settled and the plan above reflects them.

| # | Question | Decision | Consequence |
|---|----------|----------|-------------|
| Q1 | Which app model ships first? | **Both native and managed in v1.** | Phase 7 is in scope, not deferred. Launcher shows both kinds side by side with no visible distinction beyond launch latency. |
| Q2 | Where does the store live in v1? | **Dev store first** — a local HTTP server plus a device-side HTTP API. | Phase 6 delivers `tools/devstore.py`; the static HTTPS catalog becomes a Phase 8 follow-up because the device client is identical either way. |
| Q3 | Package signing from day one? | **Yes, with a dev bypass.** | Ed25519 over canonical(manifest) ‖ payload, via PSA Crypto. `CONFIG_FRAME_STORE_ALLOW_UNSIGNED` for local iteration only; `--release` refuses to build with it enabled. |
| Q4 | Which 8048S070 variant? | **`-C`, capacitive GT911.** | Input layer is I²C polling at 20 Hz. INT pin is not routed on this board, so no interrupt path is needed. |

### Still open (non-blocking)

These have sensible defaults and will not hold up Phase 0. They get resolved in Phase 1 with the
hardware on the desk — see [Device Support §6](DEVICE_SUPPORT.md#6-open-hardware-questions-verify-in-phase-1-day-one).

| # | Question | Default |
|---|----------|---------|
| H1 | SD card pinout on this revision | CMD 11 / CLK 12 / D0 13, SDMMC 1-bit |
| H2 | Audio path (DAC vs codec) | I²S DAC, optional, not on the v1 critical path |
| H3 | Panel max stable PCLK | 16 MHz (~39 Hz); try 20–24 MHz in Phase 1 |
| H4 | Revision v1.1 vs v1.3 differences | v1.3 timings, v1.1 as a `revision` variant |
| H5 | Backlight polarity | Active-high, 5 kHz PWM |
| H6 | SD slot 1-bit vs 4-bit | 1-bit |

---

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| **PSRAM bandwidth contention** — 800×480 RGB scanout + Wi-Fi + LVGL render all hit the same 8 MB PSRAM | **High** | **High** | Panel takes ~32 MB/s of ~85 MB/s unconditionally. Mitigations, in order: PSRAM at 120 MHz; **render in SRAM and copy the dirty rect sequentially** (traffic ∝ invalidated area, not screen); bounce buffers in SRAM so EDMA never reads PSRAM; skip empty flushes; idle refresh throttle. **Benchmarked on day one of Phase 1** — see [Render Performance](RENDER_PERFORMANCE.md) |
| **LVGL's own scroll optimisation copies the framebuffer** | High | Medium | In DIRECT mode LVGL blits from the PSRAM framebuffer when scrolling, which is read+write of the scrolled area. `balanced`/`lowmem` use PARTIAL mode where this does not apply; scroll regions are bounded by `frame_ui_virtual_list()` |
| **Render starved by Wi-Fi on the same core** | Medium | High | Two-core split: Wi-Fi on core 0 (IDF default, prio 23), LVGL alone on core 1. Enforced by a startup affinity assertion (A25) |
| **Flash writes stall XIP on both cores** | High | Medium | Erase in ≤64 KB chunks and write in ≤8 KB chunks with `vTaskDelay(1)` between; installs are modal and the progress screen is static; LVGL draw path compiled to IRAM (`CONFIG_FRAME_RENDER_IN_IRAM`) |
| **Write-allocate doubles the flush-copy cost** | Medium | Medium | Measured in Phase 1 §6.2. If the 768 KB copy exceeds 25 ms, the flush path moves to a GDMA channel, which bypasses the cache |
| **Bounce-buffer parity bug corrupts the picture permanently** (esp-idf #19070 / IDFGH-18263) | **Medium** | **High** | One coalesced EOF inverts `bb_eof_count % 2` and every later refill lands in the buffer being scanned; `esp_wifi_start()` is the reported trigger. **Tested first in Phase 1** (A26). Fallbacks: `no_fb` + GDMA-descriptor refill, drop bounce buffers, or pin the panel and start Wi-Fi from the other core |
| **Memory pressure is misdiagnosed as a framebuffer problem** | Low | Low | PSRAM has **2.2 MB headroom (28 %)**. The bigger free wins are streaming image assets (768 KB) and `lowmem` single-fb (768 KB) — together as much as the whole `raster8` saving, with no colour loss. See [Framebuffer Memory §5](FRAMEBUFFER_MEMORY.md#5-the-thing-that-actually-matters-memory-is-not-the-binding-constraint) |
| **Native app switch feels slow** (reboot ~300–500 ms) | Medium | Medium | Splash frame drawn *before* restart so the transition reads as intentional; keep bootloader lean; measure and publish the number |
| **8048S070 revision differences** break the panel or touch | Medium | High | `revision` field in `device.json`; both v1.1 and v1.3 timings encoded; verify on real hardware in Phase 1 |
| **ESP-IDF v6 migration surprises** (Picolibc, warnings-as-errors, removed legacy drivers) | Medium | Medium | Pin v6.1.1; no legacy `driver/*.h` usage at all; enable `-Werror` from day one so it never accumulates |
| **WASM interpreter too slow for real UIs** | Medium | Medium | Now in v1, so it gets a **hard gate in Phase 7**: a scrolling-list benchmark must hit ≥ 20 fps before WASM is used for anything but simple dashboards. The retained-tree UI model keeps per-frame work on the native side, which is most of the mitigation. |
| **wasm3 memory footprint** crowds out LVGL / the log buffer | Medium | Medium | 512 KB hard cap on the WASM heap; `lowmem` display profile auto-selected when a managed app is resident; heap high-water logged on every app transition |
| **Flash layout too tight** as apps grow | Low | Medium | Slot count/size are Kconfig knobs per device; 16 MB gives ~4 MB of headroom over the base layout |
| **Bricking via a bad app image** | Low | High | Never write the running slot; stock `esp_ota_*` validation; rollback on failed health check; shell slot is never written by the app store |

---

## 8. Out of scope (explicit)

- Paid apps, licensing, DRM
- Multi-user / lock screen security
- Native Android/iOS companion app (the device's own web UI covers setup in v1)
- Bluetooth audio, media playback
- ESP32-P4 / MIPI-DSI support (architecture allows it; no board definition in v1)
- Over-the-air updates of the *bootloader* partition

---

## 9. Definition of done (v1)

- [ ] All 18 acceptance criteria met on real 8048S070 **-C** hardware
- [ ] `docs/` matches the implementation (not aspirational)
- [ ] A second board can be added from `_template` in under an hour — proven by actually adding one
- [ ] CI builds every board on every push; releases are signed and reproducible
- [ ] 24-hour soak test with app install/update cycles (native and WASM), no leak, no crash
- [ ] The dev store flow works from a clean laptop: clone, `python3 tools/devstore.py`, browse, install
