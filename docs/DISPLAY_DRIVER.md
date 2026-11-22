# myFrame — Display Driver (`frame_display`)

> Requirement: *"For display you should implement a driver which will be resource efficient."*
>
> The quantitative reasoning — PSRAM bandwidth arithmetic, the two-core schedule, and the app render
> budget — lives in [Render Performance](RENDER_PERFORMANCE.md). This document covers the driver
> itself: API, buffer strategy, pacing, touch and power.

---

## 1. What "resource efficient" has to mean here

On the 8048S070 the numbers are unforgiving:

- Framebuffer at 800 × 480 × 16 bpp = **768 000 bytes**. Internal SRAM is **512 KB total**.
  → The framebuffer **cannot** live in SRAM. It must live in PSRAM.
- PSRAM is a shared bus. The LCD_CAM EDMA engine pulls pixels out of it continuously —
  **~32 MB/s at 16 MHz PCLK, unconditionally** — while the CPU wants it for everything else.
  Realistic PSRAM throughput is ~70–85 MB/s, so the panel alone takes ~40 % of it.
- Wi-Fi is the other big consumer, and it also wants PSRAM for its buffers.
- FreeRTOS, the LVGL task, touch polling and the store's flash writes all compete for the same SRAM.

So the driver is not "a wrapper that calls `esp_lcd_new_rgb_panel`". The driver is **a set of
decisions about where bytes live and when they move** — plus a clean API so those decisions are
tunable per board.

### The four levers

| Lever | Effect |
|-------|--------|
| **Render in SRAM, copy sequentially** | PSRAM traffic ∝ invalidated area, not screen size. The single biggest win — see [Render Performance §2](RENDER_PERFORMANCE.md#2-the-fix-render-in-sram-never-in-the-psram-framebuffer). |
| **Bounce buffers in SRAM** | EDMA reads SRAM at full speed; panel scanout adds no PSRAM pressure. |
| **Dirty-rectangle rendering** | Only invalidated regions are redrawn; a static screen costs ~0 CPU and ~0 PSRAM. |
| **Vsync-paced presentation** | Never write into the buffer being scanned → no tearing, no wasted frames. |

---

## 2. Buffer profiles

Boards differ. A 2 MB PSRAM board cannot afford two full framebuffers; an 8 MB board can. The
profile is a `device.json` field, not a compile-time `#if`.

| Profile | Framebuffers | LVGL mode | SRAM | PSRAM | Panel traffic | Use |
|---------|-------------|-----------|------|-------|---------------|-----|
| **`balanced`** *(default)* | 2 × 16-bit in PSRAM | `PARTIAL` | ~77 KB | 1.5 MB | 32 MB/s | 8048S070; any board with ≥ 4 MB PSRAM |
| **`lowmem`** | 1 × 16-bit in PSRAM | `PARTIAL` | ~55 KB | 0.77 MB | 32 MB/s | ≤ 2 MB PSRAM, or while a WASM app is resident |
| **`raster8`** | 2 × 8-bit + LUT | `PARTIAL` + quantise | ~80 KB | 0.77 MB | **16 MB/s** | Opt-in, Phase 8 — see [Framebuffer Memory](FRAMEBUFFER_MEMORY.md) |
| **`direct`** | 2 × 16-bit in PSRAM | `DIRECT` | ~30 KB | 1.5 MB | 32 MB/s | Full-screen video only; explicit opt-in |
| **`zerocopy`** | none | `no_fb` + bounce fill | ~30 KB | 0 | 0 | ≤ 480×320 panels; not reachable on this board |

### 2.1 `balanced` — the default

```
num_fbs                 = 2
flags.fb_in_psram       = 1
bounce_buffer_size_px   = 800 * 8          (2 × 12.8 KB in SRAM)
LVGL render mode        = LV_DISPLAY_RENDER_MODE_PARTIAL
LVGL draw buffers       = 2 × (800 × 16 × 2 B) = 51.2 KB in SRAM
```

- **PSRAM:** 2 × 768 000 B = **1.5 MB**
- **SRAM:** 51.2 KB (draw) + 25.6 KB (bounce) = **~77 KB**

LVGL renders only the invalidated rectangle into an SRAM buffer. `frame_display_flush()` then
performs **one sequential `memcpy`** into the back framebuffer, and the swap is paced by vsync.

This is the profile that makes small updates cheap: a 100×100 widget update moves 20 KB of PSRAM
traffic, not 768 KB.

### 2.2 `lowmem` — small-PSRAM boards

```
num_fbs                 = 1
flags.fb_in_psram       = 1
bounce_buffer_size_px   = 800 * 16         (2 × 25.6 KB in SRAM)
LVGL render mode        = LV_DISPLAY_RENDER_MODE_PARTIAL
LVGL draw buffers       = 2 × (800 × 8 × 2 B) = 25.6 KB in SRAM
```

- **PSRAM:** **0.77 MB**
- **SRAM:** 25.6 KB + 25.6 KB = **~51 KB**

Single framebuffer means LVGL renders into SRAM and copies into the live buffer — tearing is possible
during a full-screen animation, but invisible for the dirty-rect updates that dominate real use.

### 2.3 `direct` — explicit opt-in only

LVGL renders straight into a PSRAM framebuffer, no copy. Cheaper for genuinely full-screen content,
much more expensive for everything else because the renderer does scattered read-modify-write against
PSRAM. The store refuses a `direct` app on a board that has not declared support for it.

### 2.4 `raster8` — the memory-saving raster profile (opt-in)

The one place a genuine raster driver earns its keep on this board. Framebuffer stored as
**8-bit palette indices** in PSRAM, expanded to RGB565 in the `on_bounce_empty` callback through a
256-entry LUT.

```
framebuffer:  800 × 480 × 1 B = 384 KB  (×2 = 768 KB, vs 1.5 MB)
panel read:   384 KB × 39 Hz = 16.0 MB/s   (vs 32.0 MB/s)
```

Halves both the framebuffer memory and the panel's unconditional PSRAM traffic. Costs ~6–15 % of a
core in ISR context and limits the UI to 256 colours. Full trade-off analysis, the per-operation
cycle budget, and why it is **not** the default: [Framebuffer Memory §3–§4](FRAMEBUFFER_MEMORY.md).

### 2.5 `zerocopy` — documented, not used

`no_fb = 1` with `on_bounce_empty` filling bounce buffers straight from the LVGL partial buffer.
~30 KB of RAM total, but the CPU must produce scanlines at the panel's rate — only viable at low
resolution. Kept in the code for a future small board; **not** the 8048S070 path.

---

## 3. Driver API

Deliberately narrow. Nothing outside `frame_display` touches `esp_lcd`.

```c
typedef enum {
    FRAME_DISPLAY_PROFILE_BALANCED,
    FRAME_DISPLAY_PROFILE_LOWMEM,
    FRAME_DISPLAY_PROFILE_DIRECT,
    FRAME_DISPLAY_PROFILE_ZEROCOPY,
} frame_display_profile_t;

typedef struct {
    uint16_t width, height;
    frame_display_profile_t profile;
    uint32_t pclk_hz;
    uint32_t refresh_hz;           /* derived from the timing block */
    bool     has_touch;
} frame_display_info_t;

esp_err_t frame_display_init(const frame_display_desc_t *desc);
esp_err_t frame_display_get_info(frame_display_info_t *out);

/* Backlight, 0..100, gamma-corrected, ramped to avoid a visible step. */
esp_err_t frame_display_set_backlight(uint8_t percent);
uint8_t   frame_display_get_backlight(void);

/* Idle policy: slow the LVGL refresh timer, drop to the brightness floor. */
esp_err_t frame_display_set_idle(bool idle);

/* Diagnostics — surfaced in Settings → Developer and GET /api/debug/render. */
typedef struct {
    uint32_t frames_rendered;
    uint32_t frames_skipped;      /* flush requested but nothing was dirty  */
    uint32_t frames_dropped;      /* exceeded the frame budget               */
    uint32_t vsync_missed;
    uint32_t dirty_px;            /* invalidated pixels in the last frame    */
    uint32_t render_us, copy_us, frame_us_last, frame_us_max;
    size_t   sram_bytes, psram_bytes;
    uint32_t core0_load_pct, core1_load_pct;
} frame_display_stats_t;

void frame_display_get_stats(frame_display_stats_t *out);
```

---

## 4. PSRAM bandwidth — the short version

Full treatment in [Render Performance §1](RENDER_PERFORMANCE.md#1-the-arithmetic-nobody-can-argue-with).

| Consumer | Bandwidth at 16 MHz PCLK | Share of ~85 MB/s |
|----------|--------------------------|-------------------|
| LCD_EDMA scanout, 16 bpp | 16 M × 2 B = **32.0 MB/s** | ~38 % |
| LVGL render into SRAM | **0 MB/s** | 0 % |
| Flush copy (only when dirty) | ∝ invalidated area | 0 % when static |
| Wi-Fi (RX/TX) | 2–10 MB/s under load | ~10 % |
| **Left for everything else** | | **~50 %** |

**The key design consequence:** because rendering happens in SRAM, the panel's 32 MB/s is a *fixed*
cost and the render path adds *nothing* unless pixels actually change. A static screen leaves the
entire remaining PSRAM bandwidth to Wi-Fi.

**Why 120 MHz PSRAM is non-negotiable:** at 80 MHz the octal PSRAM delivers only ~40 MB/s effective —
the panel alone would consume ~80 % of it and the device would stutter under any network load.

> A dumb RGB panel scans continuously. There is no "stop refreshing" the way there is with a
> GRAM-backed SPI panel. Everything above is about *not adding* traffic, not about removing the
> panel's baseline.

---

## 5. LVGL configuration for this target

```c
#define LV_COLOR_DEPTH                 16     /* RGB565, matches the panel        */
#define LV_USE_STDLIB_MALLOC           LV_STDLIB_BUILTIN
#define LV_MEM_SIZE                    (1536 * 1024)   /* heap, backed by PSRAM   */
#define LV_CACHE_DEF_SIZE              (768 * 1024)    /* decoded image cache     */
#define LV_DRAW_SW_DRAW_UNIT_CNT       1               /* see note                */
#define LV_DEF_REFR_PERIOD             16              /* 60 Hz intent, panel 39  */
#define LV_USE_LOG                     0               /* route via frame_log     */
#define LV_USE_ASSERT_NULL             1
#define LV_USE_ASSERT_MALLOC           1
```

- **One draw unit.** Two units would help if rendering were CPU-bound; here it is PSRAM-bound, and a
  second unit thrashes the small data cache. Also, LVGL's multi-draw-unit threads cannot be pinned
  to a core, so they would break the two-core schedule in
  [Render Performance §4](RENDER_PERFORMANCE.md#4-two-cores-properly-scheduled). Revisit in Phase 8
  with a custom pinned draw pool if profiling justifies it.
- **1.5 MB heap, 768 KB image cache.** Fits the PSRAM budget in Render Performance §3.2.
- **`PARTIAL` render mode is the default** — that is what keeps LVGL's per-pixel work in SRAM.

---

## 6. Rendering pipeline

```
        ┌──────────── LVGL task (CORE 1, prio 4) ─────────────┐
        │                                                       │
 input ─► lv_timer_handler()                                    │
        │   └─ lv_obj_invalidate_area()   (dirty rects merge)  │
        │   └─ lv_display_refr_timer()                          │
        │        └─ render ONLY the dirty rect                  │
        │           into an SRAM draw buffer  (25.6 KB)         │
        │        └─ flush_cb → frame_display_flush()            │
        └──────────────────────┬────────────────────────────────┘
                               │
                    dirty_px == 0 ?
                       │              │
                      yes             no
                       │              │
                  skip entirely   1. esp_cache_msync(WRITE_BACK) on the SRAM buffer
                       │         2. memcpy SRAM → PSRAM back fb   (sequential)
                       │         3. wait vsync semaphore
                       │         4. esp_lcd_panel_draw_bitmap() → swap fb pointer
                       │         5. lv_display_flush_ready()
                       │              │
                       └──────────────┤
                                      ▼
                          LCD_CAM / EDMA scans out
                          via SRAM bounce buffers
                                      │
                                 panel (39 Hz)
```

**The flush contract.** Getting any of these wrong produces bugs that look like hardware faults:

1. `esp_cache_msync(MSYNC_TYPE_WRITE_BACK)` before the copy — the CPU wrote through the data cache,
   and the copy must see the bytes that are actually in SRAM.
2. Wait for the vsync semaphore (posted from `esp_lcd_rgb_panel_register_event_callbacks`).
3. `esp_lcd_panel_draw_bitmap(panel, 0, 0, w, h)` rotates to the next framebuffer.
4. `lv_display_flush_ready()` — **after** the swap is queued, never before.

Skipping (1) gives intermittent sparkle artefacts. Getting (4) wrong hard-locks the LVGL render loop.

**Skip the whole thing when nothing is dirty.** `dirty_px == 0` → no copy, no swap, no PSRAM traffic.
This is what makes a static screen free.

---

## 7. Touch

GT911 on I²C (SDA 19 / SCL 20, addr `0x5D`, RST 38, **INT not connected**).

- **Polled at 20 Hz** from `frame_touch` on **core 0**.
- **Never read during the LVGL render cycle.** A known cause of visible tearing on RGB panels: an
  I²C transaction stalls the bus while the panel is mid-scan.
- Events are queued into a lock-free SPSC ring and drained by the LVGL task at frame start.
- Driver handles: coordinate transform (panel orientation may be 180° from natural), 5-point
  tracking, palm rejection (drop contacts > 12 mm), and a 10 ms synthetic debounce.
- Gestures derived in `frame_input`, not in the touch driver: swipe (launcher paging), long-press
  (context menu), edge-swipe-up (home).

```c
typedef struct {
    uint8_t  id;          /* tracking id                          */
    uint16_t x, y;        /* panel coordinates, already transformed */
    uint16_t pressure;    /* 0..255, normalised                   */
} frame_touch_point_t;

typedef struct {
    frame_input_type_t   type;    /* DOWN / MOVE / UP / CANCEL */
    frame_touch_point_t  point;
    uint32_t             timestamp_us;
} frame_input_event_t;

esp_err_t frame_touch_init(const frame_touch_desc_t *desc);
esp_err_t frame_touch_read(frame_touch_point_t *pts, uint8_t max, uint8_t *count);
```

---

## 8. Backlight & power

- **LEDC PWM** on GPIO 2. 5 kHz carrier — high enough to be invisible, low enough to avoid EMI on
  the RGB bus.
- **Gamma-corrected brightness** (`L = (x/100)^2.2`) so 20 % looks like 20 %, not "nearly off".
- **Ramped transitions** (~120 ms, 8 steps) so brightness changes do not snap.
- **Idle ladder:** 100 % → 60 % after 60 s → 10 % after 180 s → panel off after 600 s. Any touch
  returns to 100 % instantly.
- **`frame_display_set_idle()`** also throttles the LVGL refresh timer (16 ms → 100 ms) when nothing
  is dirty — see [Render Performance §8](RENDER_PERFORMANCE.md#8-mitigation-ladder).

---

## 9. Verification plan (Phase 1)

| Test | Pass condition |
|------|----------------|
| Solid-colour fill | Uniform colour, no banding, correct orientation |
| LVGL hello world | Text renders sharply, no colour swap (BGR/RGB) |
| Scrolling benchmark (10 000-row virtual list, 60 s) | 30 fps, core 1 < 60 %, no visible tearing |
| Touch calibration grid | Tap lands within 3 px of the crosshair in all 9 zones |
| **Dirty-rect check** | Static clock: `dirty_px` < 5 000 per frame; PSRAM traffic back to baseline |
| **Wi-Fi + render** | 5 MB/s download during a launcher animation, < 1 dropped frame/s |
| **PSRAM throughput** | ≥ 55 MB/s sequential measured; recorded in Render Performance §6 |
| **Copy cost** | 768 KB SRAM→PSRAM in ≤ 25 ms; recorded in Render Performance §6 |
| Memory audit | `frame_display_get_stats()` reports ≤ 120 KB SRAM, ≤ 1.6 MB PSRAM |
| Frame pacing | `vsync_missed` < 1 % over a 5-minute interactive session |
| **Bounce-buffer parity** | Wi-Fi started *while* the panel runs, 500 cycles, no permanent corruption — see the IDFGH-18263 note below |
| Brightness ramp | No visible step between 99 % and 1 % |

> **Known upstream defect (Phase 1 blocker):** esp-idf issue #19070 / IDFGH-18263 — in bounce-buffer
> mode the driver picks which bounce buffer to refill from `bb_eof_count % 2`, and that parity is
> never re-synchronised. One coalesced EOF inverts it permanently and every subsequent refill lands
> in the buffer being scanned. Reported real-world trigger: `esp_wifi_start()` (PHY init) racing the
> panel. Because `balanced` and `lowmem` both use bounce buffers, this must be tested before anything
> else. Fallbacks: `no_fb` + GDMA-descriptor-based refill, or drop bounce buffers entirely. Detail in
> [Framebuffer Memory §6](FRAMEBUFFER_MEMORY.md#6-a-real-bug-that-affects-our-default-profile).

---

## 10. Deliberate non-features

- **No SPI panel support in v1.** The board abstraction allows it (`display.interface` in
  `device.json`) and `frame_display` keeps the flush contract generic, but only the RGB path is
  implemented. Adding SPI later means adding a flush implementation, not an architecture change.
- **No partial-refresh GRAM tricks.** The 8048S070's panel has no GRAM; nothing to exploit.
- **No DMA2D/PPA blitter.** ESP-IDF 6.1 added accelerated bitmap drawing for RGB LCD panels via
  DMA2D/PPA. That peripheral exists on ESP32-P4, **not** on the ESP32-S3, so it is unavailable on
  this board. It becomes an automatic win when a P4-class board is added.
- **No multi-draw-unit LVGL in v1.** Threads cannot be core-pinned, so it would break the schedule.
  Revisit in Phase 8 only if profiling shows core 1 saturating.
