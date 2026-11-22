# myFrame — Render Performance & Bandwidth Budget

> **Why this document exists.** The draft plan put LVGL in `DIRECT` render mode writing straight into
> a PSRAM framebuffer. That is the wrong default on ESP32-S3: it turns every UI update into random
> read-modify-write traffic against the slowest bus in the system. This document replaces it with a
> measured, budgeted design and a concrete two-core schedule.

---

## 1. The arithmetic nobody can argue with

### 1.1 PSRAM is the bottleneck, and it is shared

| Quantity | Value | Source |
|----------|-------|--------|
| Octal PSRAM clock | 120 MHz (`CONFIG_SPIRAM_SPEED_120M`) | S3 max |
| Peak bus width | 8 bit | octal |
| **Peak theoretical bandwidth** | **120 MB/s** | 8 × 120 MHz |
| **Realistic sequential throughput** | **~70–85 MB/s** | command + address + turnaround overhead |
| **Realistic random access** | **far worse** | per-transaction latency dominates |

### 1.2 The panel takes a fixed cut, forever

The 8048S070's panel is a dumb RGB panel with no GRAM. It scans continuously. Whatever the CPU is
doing, the LCD_CAM EDMA engine is reading the framebuffer.

```
800 × 480 px × 2 B          = 768 000 B per frame
× 39 Hz (16 MHz PCLK)       = 32.0 MB/s
× 29 Hz (12 MHz PCLK)       = 23.0 MB/s
× 49 Hz (20 MHz PCLK)       = 40.0 MB/s
```

**At the default 16 MHz PCLK the panel consumes ~32 MB/s — roughly 40 % of realistic PSRAM
bandwidth — and this is unconditional.** A static clock face costs the same as a video.

This is the single most important number in the project. Everything below follows from it.

### 1.3 What that leaves, and what a full-screen update costs

Budget at 16 MHz PCLK: **~85 − 32 = ~53 MB/s for the CPU**, before Wi-Fi.

A full-screen 800×480 update in RGB565 is 768 KB. Rendered in SRAM, then copied to PSRAM:

| Step | Cost | Notes |
|------|------|-------|
| LVGL renders into SRAM (blend-heavy) | ~8–13 ms | 384 000 px × ~5–8 cycles |
| Copy SRAM → PSRAM (768 KB @ ~60 MB/s) | ~12.8 ms | sequential; see the write-allocate caveat in §6.3 |
| **Total** | **~21–26 ms** | **≈ 38–47 fps with zero other work** |

At a 30 Hz budget (33 ms) a single full-screen update leaves **7–12 ms for everything else** —
Wi-Fi, touch, app logic, and the next frame's render. **Full-screen 30 Hz animation is not viable
on this hardware. Full-screen 20 Hz is marginal.**

### 1.4 What a *small* update costs — the number that actually matters

| Update area | Bytes | Render | Copy | Total | Fits 30 Hz? |
|-------------|-------|--------|------|-------|-------------|
| 100 × 100 | 20 KB | ~0.3 ms | 0.3 ms | **0.6 ms** | ✅ easily |
| 200 × 200 | 80 KB | ~1.2 ms | 1.3 ms | **2.5 ms** | ✅ yes |
| 400 × 300 | 240 KB | ~3.6 ms | 4.0 ms | **7.6 ms** | ✅ yes |
| 800 × 480 (full) | 768 KB | ~11 ms | 12.8 ms | **23.8 ms** | ⚠️ only alone |

**The conclusion is not "make it faster". It is "make the invalidated area small."** A UI that only
redraws what changed costs 40× less than one that redraws the screen. That is a design constraint on
every app, so it is written into the SDK contract in §7.

---

## 2. The fix: render in SRAM, never in the PSRAM framebuffer

### 2.1 What was wrong

`LV_DISPLAY_RENDER_MODE_DIRECT` + two full-size PSRAM framebuffers is the textbook "tear-free"
setup, and it is correct for a 320×240 SPI panel with a GRAM. On an 800×480 RGB panel it is wrong,
because LVGL's software renderer does **read-modify-write per pixel**:

```
blend:  dst = src*α + dst*(1-α)     →  1 read + 1 write per pixel
glyph:  read glyph atlas + read dst + write dst  →  3 PSRAM transactions
```

Worse, those accesses are **scattered** — different widgets, glyph atlases, image sources — so they
miss the (small, order-16–32 KB) data cache almost every time. A full-screen DIRECT render is
roughly 2–4× the PSRAM traffic of the same pixels copied sequentially, at far worse efficiency.

### 2.2 The corrected pipeline

```
   LVGL renders ONLY the invalidated rect
              │
              ▼
   ┌─────────────────────────┐
   │  SRAM draw buffer       │   800 × 16 px × 2 B = 25.6 KB  (×2 = 51.2 KB)
   │  read-modify-write here │   cache-resident, ~1 cycle/byte
   └───────────┬─────────────┘
               │  one tight sequential memcpy
               ▼
   ┌─────────────────────────┐
   │  PSRAM back framebuffer │   800 × 480 × 2 B = 768 KB  (×2 = 1.5 MB)
   └───────────┬─────────────┘
               │  vsync-paced swap (tear-free)
               ▼
   ┌─────────────────────────┐
   │  SRAM bounce buffers    │   800 × 8 px × 2 B = 25.6 KB  (×2)
   │  EDMA reads from SRAM   │   free — SRAM is fast
   └───────────┬─────────────┘
               ▼
            panel (39 Hz)
```

**Three properties, all of them load-bearing:**

1. **PSRAM traffic is proportional to the invalidated area, not the screen.** A 100×100 widget
   update moves 20 KB of PSRAM traffic instead of 768 KB — a 38× reduction.
2. **The traffic that remains is sequential.** A burst copy hits PSRAM at its best; scattered RMW
   hits it at its worst.
3. **EDMA reads SRAM, not PSRAM.** The bounce buffers mean the panel's continuous scanout does not
   add random-access pressure to the PSRAM bus at all.

### 2.3 What this costs

~51 KB of SRAM for draw buffers and ~26 KB for bounce buffers — about 77 KB of the 512 KB budget.
That is the price of not fighting the PSRAM bus, and it is cheap. Full budget in §3.

### 2.4 The profiles, revised

| Profile | Framebuffers | Render mode | SRAM | PSRAM | When |
|---------|-------------|-------------|------|-------|------|
| **`balanced`** *(default)* | 2 in PSRAM | `PARTIAL`, SRAM buffers | ~77 KB | 1.5 MB | 8048S070 and any board with ≥ 4 MB PSRAM |
| **`lowmem`** | 1 in PSRAM | `PARTIAL`, smaller SRAM buffers | ~55 KB | 0.77 MB | ≤ 2 MB PSRAM, or while a WASM app is resident |
| **`direct`** | 2 in PSRAM | `DIRECT` | ~30 KB | 1.5 MB | **Only** for a verified full-screen video case; must be justified in the app manifest |
| **`zerocopy`** | none | `no_fb` + bounce fill | ~30 KB | 0 | ≤ 480×320 panels only; not reachable on this board |

`direct` is no longer the default. It stays in the code because a full-screen slideshow or a video
player genuinely benefits from rendering once into the framebuffer with no copy — but it must be an
explicit, opt-in choice, and the store refuses a `direct` app on a board that has not declared
support for it.

---

## 3. Memory tiers

The rule that makes everything else work: **hot working set in SRAM, bulk data in PSRAM, static
data in flash.** Nothing in the per-pixel path touches PSRAM except the final sequential copy.

### 3.1 Internal SRAM (512 KB total — the scarce resource)

| Item | Size | Tier | Why here |
|------|------|------|----------|
| FreeRTOS heap / system / Wi-Fi & BT static | ~150 KB | SRAM | Non-negotiable |
| LVGL draw buffers (2 × 800×16×2 B) | 51.2 KB | SRAM | Per-pixel RMW — must be cache-resident |
| Panel bounce buffers (2 × 800×8×2 B) | 25.6 KB | SRAM | EDMA must not read PSRAM |
| Font glyph cache (hot glyphs, 2–3 fonts) | 24 KB | SRAM | Glyph blits are per-pixel reads |
| LVGL heap (object tree, styles, small allocs) | 32 KB | SRAM | Pointer-chasing, latency-sensitive |
| Task stacks (9 tasks) | ~56 KB | SRAM | — |
| Driver / HAL state, misc | ~20 KB | SRAM | — |
| **Total** | **~359 KB** | | **~153 KB headroom** |

**Display subsystem subtotal: ~77 KB** (draw + bounce), comfortably inside the ≤120 KB target.

### 3.2 PSRAM (8 MB — bulk only)

| Item | Size | Access pattern |
|------|------|----------------|
| Framebuffers × 2 | 1.5 MB | Sequential, via the flush copy |
| LVGL heap (large objects, images) | 1.5 MB | Mixed, but off the per-pixel path |
| Image cache (decoded RGB565) | 768 KB | Sequential during blit |
| Pre-rendered app surfaces | 1.0 MB | Sequential during blit |
| Log ring buffer | 256 KB | Sequential |
| Download / staging buffer | 384 KB | Sequential, freed between operations |
| wasm3 linear memory + heap | 384 KB | Sequential-ish, bounded |
| **Total** | **~5.8 MB** | **~2.2 MB headroom** |

### 3.3 Flash (16 MB, XIP)

Code, static assets, font files, icons. Read-only and mapped through the instruction cache. The one
critical rule: **writes to flash contend with instruction fetch** — see §5.

---

## 4. Two cores, properly scheduled

ESP32-S3 is 2 × Xtensa LX7 @ 240 MHz. The naive split — "core 0 for LVGL, core 1 for everything
else" — is actively harmful, because **ESP-IDF runs the Wi-Fi task on core 0 at priority 23**. A
radio task preempting a render mid-frame is a visible stutter, and it happens constantly.

### 4.1 The assignment

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

| Task | Core | Prio | Stack | Justification |
|------|------|------|-------|---------------|
| **Wi-Fi / BT** | 0 | 23 | IDF | IDF default. Hard real-time — the MAC must be serviced or the link drops. It stays on core 0 so it can never preempt rendering. |
| **lvgl** | 1 | 4 | 10 KB | Gets core 1 to itself. A frame is never preempted by radio or I/O. |
| **frame_app** (native app logic) | 0 | 4 | 12 KB | App logic is usually waiting on timers or network. Keeping it off core 1 means a busy app cannot starve render. |
| **frame_wasm** | 1 | 3 | 8 KB | Interpreters are CPU-bound; it belongs near render. At prio 3 the renderer always wins. |
| **frame_net** | 0 | 3 | 6 KB | Blocking I/O belongs on the I/O core. |
| **frame_store** (download + flash) | 0 | 3 | 8 KB | Must never share a core with render — see §5. |
| **frame_touch** | 0 | 5 | 3 KB | I²C polling. Explicitly **not** in the LVGL task (a known cause of RGB-panel tearing). |
| **frame_audio** | 0 | 6 | 4 KB | Underrun = audible glitch; keep it away from the render core. |
| **frame_log** | 0 | 2 | 3 KB | Sinks, must not delay anything. |

### 4.2 Why this split and not the other way round

| Alternative | Why it is rejected |
|-------------|--------------------|
| LVGL on core 0 | Wi-Fi at prio 23 preempts mid-frame → stutter under any network load. This is the exact failure mode we are designing out. |
| Everything on core 1, Wi-Fi alone on core 0 | App logic and flash writes would then compete with render. Worse. |
| LVGL split across both cores (2 draw units) | LVGL's multi-draw-unit support does not expose thread-to-core pinning; the OS schedules the render threads wherever it likes, which defeats the purpose. Revisit in Phase 8 with a custom draw-unit thread pool if profiling demands it. |
| Unicore (`CONFIG_FREERTOS_UNICORE`) | Strictly worse. Must remain **off**. |

### 4.3 Synchronization rules

| Rule | Detail |
|------|--------|
| **R1** | All LVGL calls happen on the lvgl task. Anything else uses `lv_async_call()` or `frame_ui_dispatch()`. |
| **R2** | No mutex is held across a blocking operation. In particular, `frame_store` never holds the registry lock while writing flash. |
| **R3** | `frame_touch` posts events to a lock-free SPSC ring; the LVGL task drains it at frame start. |
| **R4** | The framebuffer swap is gated by a binary semaphore posted from the panel's vsync ISR. Nothing else in the system touches that semaphore. |
| **R5** | `CONFIG_FREERTOS_HZ = 1000` so `vTaskDelay(1)` is a real 1 ms — required for the flash-write pacing in §5. |

### 4.4 Measured, not assumed

`frame_render_stats_t` is exposed on a debug overlay (`CONFIG_FRAME_DEBUG_OVERLAY=y`) and via
`GET /api/debug/render`:

```c
typedef struct {
    uint32_t frame_us_last;        /* last frame: render + copy + swap       */
    uint32_t frame_us_max;         /* worst frame since boot                */
    uint32_t render_us;            /* LVGL render time only                 */
    uint32_t copy_us;              /* SRAM → PSRAM flush copy only          */
    uint32_t dirty_px;             /* invalidated pixels in the last frame  */
    uint32_t frames_dropped;       /* frames exceeding the budget           */
    uint32_t vsync_missed;
    size_t   sram_bytes, psram_bytes;
    uint32_t core0_load_pct;       /* idle-task watermark, sampled 1 Hz     */
    uint32_t core1_load_pct;
} frame_render_stats_t;
```

**Acceptance gate:** the Phase 1 benchmark must show core 1 load < 60 % during a 200-row scroll at
30 Hz, and core 0 load < 40 % during a 5 MB/s HTTP download with a static screen. If either fails,
the split is wrong and gets fixed before any app is written.

---

## 5. Flash writes are the other silent killer

Writing to SPI flash suspends the cache that serves XIP. **Both cores stall on instruction fetch**
for the duration. A 1.5 MB app install done naively will freeze the UI for seconds.

Three defences, in order:

| # | Defence | Detail |
|---|---------|--------|
| 1 | **Erase in chunks, yield between** | `esp_partition_erase_range()` in ≤ 64 KB steps with `vTaskDelay(1)` between. `esp_ota_begin(…, OTA_SIZE_UNKNOWN, …)` erases the whole partition up front — avoid it. |
| 2 | **Writes in ≤ 8 KB chunks, yield between** | Bounds the worst-case stall to a few ms. `frame_store` paces itself from a high-water mark, not a fixed delay. |
| 3 | **Installs are modal and static** | The progress screen does not animate — it updates once a second. A stall that nobody can see costs nothing. |

Defence in depth: the LVGL draw path and the flush copy are compiled into **IRAM**
(`IRAM_ATTR`), so a flash operation cannot stall them on instruction fetch. This costs ~20 KB of
IRAM, which is why it is a Kconfig option (`CONFIG_FRAME_RENDER_IN_IRAM`, default **y** on boards
with ≥ 8 MB PSRAM).

---

## 6. Three open measurement items (Phase 1, day one)

These are the numbers the whole design rests on. They get measured before any app code is written.

### 6.1 Real PSRAM sequential throughput
Write and read a 4 MB block, timed. Expect 70–85 MB/s. **If it is below 55 MB/s**, the panel PCLK
must drop to 12 MHz (23 MB/s scanout) and the full-screen budget in §1.3 gets worse.

### 6.2 Real copy cost SRAM → PSRAM
Time a 768 KB `memcpy`. Expect ~13 ms. **If it is > 25 ms**, the CPU is paying write-allocate (the
cache reads each line before overwriting it, doubling traffic). Fallback: move the flush copy to a
GDMA channel, which bypasses the cache. Budgeted in Phase 1 as a half-day spike.

### 6.3 Cache behaviour under mixed load
Run a full-screen render while a 5 MB/s HTTP download is active and measure `frame_us_max`. This is
the number that decides whether the "no per-pixel PSRAM access" rule is sufficient or needs the GDMA
path from 6.2.

Each result is written into this document with the measured value and the date. The plan is not
considered validated until all three are filled in.

---

## 7. The render budget contract for apps

"New apps should be able to render" only holds if apps know what they are allowed to cost. So the
cost is a declared, enforced contract rather than a hope.

### 7.1 App classes

| Class | Target fps | Max invalidated area / frame | Typical |
|-------|-----------|------------------------------|---------|
| `static` | on demand | any, but only on change | Settings, About |
| `light` | 30 Hz | ≤ 15 % of screen | Clock, gauge, weather |
| `medium` | 30 Hz | ≤ 40 % of screen | Scrolling list, chart |
| `full` | 20 Hz | 100 % (transitions only) | Page transitions, slideshow |

Declared in the manifest:

```json
{
  "render": { "class": "light", "max_fps": 30, "direct": false }
}
```

### 7.2 What the SDK enforces

| Mechanism | Behaviour |
|-----------|-----------|
| **FPS cap** | `frame_ui_set_fps_cap()` clamps a class-`light` app to 30 Hz regardless of what it asks for. |
| **Invalidate accounting** | `frame_render_stats_t.dirty_px` is attributed to the running app and shown in the debug overlay. |
| **Budget warning** | An app that exceeds its declared class for > 5 consecutive frames gets a logged warning and a debug-overlay flag — not a hard failure, but visible in CI. |
| **Scroll helper** | `frame_ui_virtual_list()` materialises only visible rows. A 10 000-row list must not cost 10 000 objects. |
| **Surface cache** | `frame_ui_surface_cache()` renders a complex static region once into a PSRAM surface and blits it thereafter. |
| **`direct` opt-in** | Only honoured if the app declares `"direct": true` **and** the board profile allows it. Otherwise silently downgraded to `balanced`. |

### 7.3 Guidance that apps actually need

| Do | Don't |
|----|-------|
| Invalidate only the widget that changed | `lv_obj_invalidate(lv_screen_active())` |
| Let LVGL's own animations run (they invalidate only the animated area) | Custom full-screen redraw loops |
| Use `lv_label_set_text()` (invalidates the label's rect only) | Re-creating objects to change text |
| Pre-render static chrome into a surface cache | Re-blending a blurred background every frame |
| Keep lists virtualised | Creating 500 row objects up front |
| Cap your own timers to your declared class | `vTaskDelay(1)` render loops |

### 7.4 Managed (WASM) apps and rendering

Managed apps do **not** pay an interpreter penalty for rendering. They emit a declarative UI tree;
the **native shell diffs it and drives LVGL**. So a WASM clock costs the same to render as a native
clock — the interpreter only pays for tree diffing, which is proportional to *changed nodes*, not
pixels. This is the main reason the retained-tree design was chosen over exposing LVGL pointers.

---

## 8. Mitigation ladder

Ordered by value per unit of effort. Items 1–4 are Phase 1; 5–8 are Phase 8.

| # | Mitigation | Effect | Cost |
|---|-----------|--------|------|
| 1 | **PSRAM at 120 MHz** | ~2× effective bandwidth vs 80 MHz | one Kconfig line |
| 2 | **Render in SRAM, copy sequentially** (§2) | ~2–4× less PSRAM traffic per update; traffic ∝ dirty area | 77 KB SRAM |
| 3 | **Bounce buffers in SRAM** | EDMA reads SRAM; panel scanout adds no PSRAM pressure | 26 KB SRAM |
| 4 | **Skip empty flushes** | A static screen costs zero render traffic | trivial |
| 5 | **Idle refresh throttle** | 16 ms → 100 ms LVGL timer when nothing is dirty | small |
| 6 | **Surface cache for static chrome** | Complex backgrounds rendered once | medium |
| 7 | **GDMA flush path** | Removes write-allocate penalty on the copy | medium, gated on §6.2 |
| 8 | **PCLK down-shift when idle** | 39 Hz → 29 Hz, saves ~9 MB/s when nothing changes | needs panel re-init; visible glitch risk |
| 9 | **`raster8` 8-bit framebuffer** | Panel traffic **32 → 16 MB/s**, framebuffer 1.5 → 0.77 MB | 256 colours + ~6–15 % ISR load — opt-in, see [Framebuffer Memory](FRAMEBUFFER_MEMORY.md) |

Items 1–4 together take a full-screen update from "impossible" to "23 ms", and — more importantly —
take a typical widget update from ~3 MB of PSRAM traffic to ~20 KB.

Item 9 is the only remaining lever that reduces the **panel's unconditional** 32 MB/s cut. It is
deliberately last: it is the only one that costs colour depth and ISR time.

---

## 9. What "enough headroom for new apps" means, concretely

At the default profile, with a static screen:

| Resource | Used | Available |
|----------|------|-----------|
| PSRAM bandwidth | 32 MB/s (panel) | **~53 MB/s** |
| Internal SRAM | 359 KB | **~153 KB** |
| PSRAM capacity | 5.8 MB | **~2.2 MB** |
| Core 1 (render) | ~0 % | **100 %** |
| Core 0 (I/O + app) | ~15 % | **~85 %** |

That is enough for a new app to render a 400×300 animated region at 30 Hz (7.6 ms of a 33 ms budget)
while Wi-Fi runs, with room for a second concurrent surface. It is **not** enough for full-screen
30 Hz animation, and no amount of tuning will make it so on this panel — which is why §7 makes the
budget an explicit contract rather than an implicit disappointment.

---

## 10. Acceptance criteria added by this document

| ID | Criterion |
|----|-----------|
| **A19** | A `light`-class app animating a 200×200 region sustains **30 fps** with zero dropped frames over 5 minutes |
| **A20** | A 200×200 widget update costs **≤ 3 ms** end-to-end; PSRAM traffic is proportional to the invalidated area, not the screen size |
| **A21** | A `medium`-class app scrolling a 10 000-row virtual list sustains **30 fps** with core 1 load **< 60 %** |
| **A22** | Wi-Fi sustains **≥ 5 MB/s** download while the launcher animates, with **< 1 dropped frame per second** |
| **A23** | A 1.5 MB native app install never blocks the UI for more than **50 ms** |
| **A24** | All three measurements in §6 are recorded in this document with hardware numbers before Phase 1 is signed off |
| **A25** | `CONFIG_FREERTOS_UNICORE` is **n** and the task table in §4.1 is enforced by a startup assertion that verifies core affinity |
