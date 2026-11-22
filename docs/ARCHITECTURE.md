# myFrame — Architecture

---

## 1. The one-paragraph version

FrameOS is a single ESP-IDF application. At build time a **board definition** is selected,
which turns into a generated header plus a handful of `board.c` symbols. At runtime the firmware
brings up the display and boots straight into a **launcher** — the home screen, which shows every
installed app and carries the **app store** as a first-class tile. Apps are either **built into the
image** (Launcher, Settings, Logs) or **installed from a store** as signed packages. Installed
native apps are separate firmware images living in dedicated OTA slots; the launcher switches to
them by changing the boot partition and restarting. Managed apps are WebAssembly modules loaded into
an in-process interpreter and hot-swapped without a reboot. Both models ship in v1.

```
┌──────────────────────────────────────────────────────────────────┐
│  APPS                                                             │
│  launcher │ settings │ logs │ store │ clock(wasm) │ <installed>   │
├──────────────────────────────────────────────────────────────────┤
│  Frame SDK  (libframe — stable, versioned C ABI)                  │
│  ui │ storage │ net │ audio │ input │ assets │ kv │ log           │
├──────────────────────────────────────────────────────────────────┤
│  FrameOS core                                                     │
│  app_manager │ package_mgr │ store_client │ ota │ lifecycle      │
├──────────────────────────────────────────────────────────────────┤
│  Device HAL  (per-board, selected at build: -DBOARD=…)            │
│  display │ touch │ backlight │ audio │ sd │ buttons │ leds         │
├──────────────────────────────────────────────────────────────────┤
│  ESP-IDF v6.1 — FreeRTOS · esp_lcd · LVGL 9 · PSA Crypto · NVS    │
│  FatFS · esp_http_client · esp_https_ota · wasm3                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 2. Layering rules

These rules are what keep the firmware *generic*. They are enforced in code review and, where
practical, by a CI check that greps for violations.

| Rule | Meaning |
|------|---------|
| **L1** | App code **never** includes a board header. It includes `<frame/frame.h>` and nothing else device-specific. |
| **L2** | Device HAL exposes a **struct of function pointers** (`frame_board_t`), not a scatter of globals. Core code receives it; it does not link to it directly. |
| **L3** | Core components never call `esp_lcd_*`, `driver/*` or `nvs_*` directly — always through a HAL or a service. This is what makes host-side unit tests possible. |
| **L4** | Only `frame_store` may write to flash partitions. Only `frame_ota` may change the boot partition. Nothing else gets those handles. |
| **L5** | No dynamic allocation on the LVGL thread except through LVGL's own allocator. Everything long-lived is allocated at boot. |
| **L6** | Every public header carries `FRAME_API_VERSION`. The linker emits a versioned symbol table so an app image can refuse to run against an incompatible FrameOS. |
| **L7** | **Per-pixel rendering happens in SRAM.** PSRAM holds bulk data and the final sequential flush copy — never a render target. See [Render Performance §2](RENDER_PERFORMANCE.md#2-the-fix-render-in-sram-never-in-the-psram-framebuffer). |
| **L8** | **LVGL runs only on core 1.** No task other than `lvgl` may call an `lv_*` function, and nothing on core 0 may hold a lock the render path needs. |

---

## 3. Boot flow

```
ROM bootloader
  └─ 2nd stage bootloader (stock IDF)
       └─ app image (whichever slot otadata selects)
            ├─ esp_system init, PSRAM init @120 MHz, cache init
            ├─ frame_board_init()          ← HAL: pins, I2C buses, SPI
            ├─ frame_display_init()        ← panel up, LVGL port up, splash drawn
            ├─ frame_storage_init()        ← NVS + FatFS mount
            ├─ frame_log_init()            ← ring buffer, UART sink
            ├─ frame_settings_load()       ← typed settings from NVS
            ├─ am I a launched app?  ─── yes ──→ run app entry, skip shell boot
            ├─ no → frame_net_init()       ← Wi-Fi (async), provisioning if unprovisioned
            ├─ frame_app_manager_init()    ← registry from NVS, slot table
            ├─ frame_shell_start()         ← status bar, input routing
            ├─ launcher shown  ──────────► interactive here, target ≤ 1.6 s
            └─ background: Wi-Fi up → catalog refresh → update badges
```

The "am I a launched app" check reads a `boot_handoff` blob from NVS
(`{app_id, slot, shell_slot, sdk_ver}`) written by the shell just before it calls
`esp_ota_set_boot_partition()` + `esp_restart()`.

---

## 4. Component map

| Component | Responsibility | Key deps |
|-----------|---------------|----------|
| `frame_core` | event bus, app manager, lifecycle, watchdog, crash handler | — |
| `frame_display` | panel bring-up, buffer strategy, LVGL port, vsync pacing, backlight, power | `esp_lcd`, `lvgl` |
| `frame_input` | touch (GT911), buttons, gestures → normalised `frame_input_event_t` | `driver/i2c_master` |
| `frame_ui` | theme, typography, navigation stack, reusable widgets, animations | `lvgl` |
| `frame_storage` | NVS settings (typed, schema-versioned), FatFS asset area, app registry | `nvs_flash`, `fatfs` |
| `frame_net` | Wi-Fi STA, reconnect policy, provisioning, HTTP client wrapper, mDNS | `esp_wifi`, `esp_http_client` |
| `frame_ota` | slot allocation, image write, boot partition switch, rollback, FrameOS self-update | `esp_ota_ops` |
| `frame_store` | catalog fetch + verify, install/update/uninstall, progress, dev-mode bypass | `frame_ota`, PSA Crypto |
| `frame_log` | PSRAM ring buffer, levels/tags/timestamps, UART + HTTP sinks | — |
| `frame_app` | **the public SDK**: C API + ABI version + host bindings for WASM | — |
| `frame_wasm` | wasm3 runtime, sandbox, host import table, UI-tree diffing, trap containment | `wasm3` |
| `frame_apps_builtin` | Launcher, Settings, Logs, Store (each a subdirectory) | `frame_ui`, `frame_app` |

---

## 5. Threading model

The split is driven by one fact: **ESP-IDF runs the Wi-Fi task on core 0 at priority 23.** A radio
task preempting a render mid-frame is a visible stutter, so rendering must own the other core.

```
┌──────────────────── CORE 0 ────────────────────┐   ┌──────────────────── CORE 1 ────────────────────┐
│  "I/O + application core"                       │   │  "Render core"                                 │
│                                                 │   │                                                │
│  prio 23  Wi-Fi / BT stack        (IDF default) │   │  prio  4  lvgl          all LVGL calls         │
│  prio  6  frame_audio      I2S feed             │   │  prio  3  frame_wasm     managed runtime       │
│  prio  5  frame_touch      20 Hz GT911 poll     │   │                                                │
│  prio  4  frame_app       app logic (native)    │   │  Idle → light sleep hint                      │
│  prio  3  frame_net       HTTP, DNS, prov.      │   │                                                │
│  prio  3  frame_store     download + flash write│   │                                                │
│  prio  2  frame_log       UART drain            │   │                                                │
└─────────────────────────────────────────────────┘   └────────────────────────────────────────────────┘
```

| Task | Prio | Stack | Core | Justification |
|------|------|-------|------|---------------|
| Wi-Fi / BT | 23 | IDF | 0 | IDF default. Hard real-time — the MAC must be serviced or the link drops. Kept off the render core. |
| `lvgl` | 4 | 10 KB | **1** | Owns all LVGL calls; gets core 1 to itself so a frame is never preempted by radio or I/O. |
| `frame_app` | 4 | 12 KB | **0** | App logic is usually waiting on timers or network. Keeping it off core 1 means a busy app cannot starve render. |
| `frame_wasm` | 3 | 8 KB | **1** | Interpreters are CPU-bound; at prio 3 the renderer always wins. |
| `frame_net` | 3 | 6 KB | **0** | Blocking I/O belongs on the I/O core. |
| `frame_store` | 3 | 8 KB | **0** | Flash writes stall XIP; must never share a core with render. |
| `frame_touch` | 5 | 3 KB | **0** | I²C polling. Explicitly **not** in the LVGL task (a known cause of RGB-panel tearing). |
| `frame_audio` | 6 | 4 KB | **0** | Underrun = audible glitch; keep it away from the render core. |
| `frame_log` | 2 | 3 KB | **0** | Sinks must not delay anything. |
| vsync / bounce ISR | 23 | — | — | Only signals a semaphore. |

`CONFIG_FREERTOS_UNICORE` must stay **off**; a startup assertion verifies every task's core affinity
against this table (acceptance A25). Full rationale, the rejected alternatives, and the
synchronisation rules are in [Render Performance §4](RENDER_PERFORMANCE.md#4-two-cores-properly-scheduled).

---

## 6. Memory map

ESP32-S3 on the 8048S070: **512 KB internal SRAM**, **8 MB octal PSRAM @ 120 MHz**, **16 MB flash**.

The organising rule: **hot working set in SRAM, bulk data in PSRAM, static data in flash.** Nothing in
the per-pixel path touches PSRAM except the final sequential flush copy.

| Pool | Item | Size | Notes |
|------|------|------|-------|
| Internal SRAM | FreeRTOS heap / Wi-Fi / BT / system | ~150 KB | Non-negotiable |
| | LVGL draw buffers (2 × 800×16×2 B) | 51 KB | Per-pixel RMW must be cache-resident |
| | Panel bounce buffers (2 × 800×8×2 B) | 26 KB | EDMA must not read PSRAM |
| | Font glyph cache (hot glyphs) | 24 KB | Glyph blits are per-pixel reads |
| | LVGL object tree + styles + heap | 32 KB | Pointer-chasing, latency-sensitive |
| | Task stacks (9 tasks) | ~56 KB | Table above |
| | Driver / HAL state, misc | ~20 KB | — |
| | **Display subtotal** | **~77 KB** | Meets A4 (≤120 KB) |
| | **SRAM total** | **~359 KB** | **~153 KB headroom** |
| PSRAM | Framebuffers ×2 (800 × 480 × 2 B) | 1.5 MB | `balanced` profile |
| | LVGL heap (large objects) | 1.5 MB | `LV_MEM_SIZE` |
| | Image cache (decoded RGB565) | 768 KB | `LV_CACHE_DEF_SIZE` |
| | Pre-rendered app surfaces | 1.0 MB | Surface cache for static chrome |
| | Log ring buffer | 256 KB | ~2 000 lines at 128 B |
| | Download / staging buffer | 384 KB | Freed between operations |
| | wasm3 linear memory + heap | 384 KB | Only while a managed app is resident |
| | **PSRAM total** | **~5.8 MB** | **~2.2 MB headroom** |

`CONFIG_SPIRAM_USE_MALLOC=y` with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` so small hot
allocations stay in SRAM and only large blocks go to PSRAM.

**Bandwidth, not just capacity, is the constraint** — the panel consumes ~32 MB/s of PSRAM
continuously, and the render path is designed to add ~0 when the screen is static. See
[Render Performance §1](RENDER_PERFORMANCE.md#1-the-arithmetic-nobody-can-argue-with).

---

## 7. Flash layout (16 MB, 8048S070 default)

```
# Name       Type   SubType   Offset      Size       Notes
nvs          data   nvs       0x9000      0x6000     settings, app registry, boot_handoff
otadata      data   ota       0xF000      0x2000     boot selection + rollback state
phy_init     data   phy       0x11000     0x1000
shell_a      app    ota_0     0x20000     0x300000   FrameOS A   (3.0 MB)
shell_b      app    ota_1     0x320000    0x300000   FrameOS B   (3.0 MB)  ← self-OTA target
app_0        app    ota_2     0x620000    0x180000   native app   (1.5 MB)
app_1        app    ota_3     0x7A0000    0x180000   native app   (1.5 MB)
app_2        app    ota_4     0x920000    0x180000   native app   (1.5 MB)
app_3        app    ota_5     0xAA0000    0x180000   native app   (1.5 MB)
storage      data   fat       0xC20000    0x3D0000   icons, assets, download staging (3.9 MB)
                                                       ends 0xFF0000 — 64 KB spare
```

Slot count and size are **Kconfig knobs per device**
(`CONFIG_FRAME_APP_SLOT_COUNT`, `CONFIG_FRAME_APP_SLOT_SIZE`), so a smaller board can ship 2 × 512 KB.

Why real OTA subtypes rather than a custom partition type: the stock bootloader already implements
"pick the slot with the highest sequence number, roll back if the new image never reports healthy".
Reusing it means **app crash → automatic fallback to the shell** for free.

---

## 8. App execution models

| | **Native** | **Managed / WASM** |
|---|---|---|
| What it is | Standalone ESP-IDF image linked against `libframe` | `.wasm` module + manifest, run by wasm3 inside FrameOS |
| Switch cost | Reboot, ~300–500 ms | Instant, no reboot (~5–20 ms) |
| Performance | Native | ~10–40× slower (interpreted) |
| Isolation | Process-level — a hard fault cannot corrupt the shell | Interpreter-level — traps are contained, memory is bounded |
| Memory | Owns the whole device | Bounded heap (64–512 KB) |
| UI access | Full LVGL | Retained-mode UI tree via host imports |
| Crash → shell | Bootloader rollback | Runtime trap → app marked failed, shell survives |
| Best for | Games, dashboards, media, anything performance-sensitive | Clocks, weather, notes, small utilities |

**Both ship in v1.** The launcher shows them side by side; the only user-visible difference is
launch latency.

Rejected alternative: mapping an ELF into the instruction address space with
`esp_mmu_map(ESP_MMU_VADDR_EXEC)`. Possible on S3, but it demands position-independent code, breaks
ISR registration, and gives no rollback story. Documented as future work only.

---

## 9. Error handling & recovery

| Failure | Behaviour |
|---------|-----------|
| App image fails validation at install | Rejected before any slot is written; logged; UI shows the reason |
| App image fails validation at boot | Bootloader rolls back to the previously valid slot (the shell) |
| App crashes after launch | `esp_ota_mark_app_valid_cancel_rollback()` is never called → next boot reverts to shell |
| FrameOS crashes | Watchdog reset; `boot_handoff` is cleared; boots into shell; consecutive-failure counter triggers **safe mode** (no auto-launch, Wi-Fi off, log level raised) |
| Power loss mid-install | Slot left in an erased/partial state. Registry entry is only committed *after* `esp_ota_end()` succeeds, so the half-written slot is invisible and reclaimed on next boot |
| Power loss mid-update | Same as above; the previous version is still intact in its own slot until the new one is committed |
| NVS corrupted | `nvs_flash_erase()` + defaults, then require re-provisioning |
| Heap exhaustion | Allocation failures return `NULL` and are handled; `frame_heap_monitor` logs the high-water mark on every transition |

---

## 10. Configuration surface

Three layers, in order of precedence:

1. **`sdkconfig.defaults.<board>`** — per-device tuning (PSRAM speed, flash size, LVGL buffers, partition table).
2. **`device.json`** — declarative board description, turned into `frame_board.h` at build time.
3. **Runtime settings (NVS)** — user-facing: brightness, theme, Wi-Fi, language, store URL.

`device.json` example (abridged):

```json
{
  "id": "sunton_esp32_8048s070",
  "revision": "1.3",
  "chip": "esp32s3",
  "display": {
    "interface": "rgb565", "width": 800, "height": 480,
    "pclk_hz": 16000000, "panel": "EK9716/ST7262",
    "profile": "balanced"
  },
  "touch":  { "controller": "gt911", "i2c_addr": "0x5D" },
  "audio":  { "codec": "pcm5101", "verify": true },
  "flash_mb": 16, "psram_mb": 8, "psram_speed": "120m"
}
```
