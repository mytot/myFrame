# myFrame — Framebuffer Memory: Why the CYD Trick Doesn't Transfer, and What Does

> **Question raised:** *"Why can't we build a similar raster driver for the display which can save
> memory?"* — referencing
> [`chipguyhere/ESP32-CYD-Resistive_display`](https://github.com/chipguyhere/ESP32-CYD-Resistive_display),
> whose `RasterBouncingBalls` demo renders with **~23 KB** of extra memory instead of a framebuffer.
>
> **Short answer:** that demo saves memory because its **panel has GRAM**, not because of a clever
> driver. The 8048S070's panel has no GRAM. But there *is* a real, Espressif-documented way to halve
> the framebuffer on our board — it just costs CPU instead of memory. Full analysis below.

---

## 1. What the CYD library actually does

`RasterBouncingBalls` uses what the library calls **beam chasing**:

> *"The balls skip LVGL: each strip of them is drawn just before it's sent. Only the controls at the
> bottom are LVGL, and they wait their turn behind the balls. It uses about 23 KB of extra memory."*

Mechanically: render a horizontal strip into a small buffer → push it over SPI → render the next
strip → push it. **No full framebuffer exists anywhere.** The pixels are generated in the same
order and at the same rate the panel consumes them.

This works on the CYD because of **three properties that the 8048S070 does not have**:

| Property | CYD (ST7789) | 8048S070 (RGB panel) |
|----------|--------------|----------------------|
| **Panel has its own GRAM** | ✅ Yes — the panel refreshes itself | ❌ **No — it's dumb glass** |
| **Pixel push is on demand** | ✅ SPI writes whenever the MCU likes | ❌ Continuous DMA, panel sets the pace |
| **Per-pixel time budget** | **~48–96 cycles** (40–80 MHz SPI) | **~15 cycles** (16 MHz PCLK) |
| **Resolution** | 240×320 = 76 800 px | 800×480 = 384 000 px (**5× more**) |
| **MCU framebuffer required** | ❌ No | ✅ **Yes** |

The per-pixel budget is the killer. On the CYD, SPI at 40–80 MHz moves 2.5–5 Mpx/s, so each pixel
may take **200–400 ns = 48–96 cycles**. On the 8048S070 the LCD_CAM DMA consumes **16 Mpx/s**, so
each pixel may take **62.5 ns = 15 cycles**. Beam chasing needs to *render* a pixel in that budget —
LVGL's software renderer needs 30–80 cycles per pixel for text with alpha. It cannot be done.

**So: a beam-chasing LVGL driver on the 8048S070 is not possible.** Not a tuning issue — a factor-of-3
shortfall in the fundamental budget.

---

## 2. But the underlying idea does transfer — and Espressif documents it

ESP-IDF has a mode for exactly this. From the **v6.1 RGB LCD documentation**, *Bounce Buffer Only*:

> *"This mode is similar to the Bounce Buffer with Single PSRAM Frame Buffer, but there is no PSRAM
> frame buffer initialized by the LCD driver. Instead, the user supplies a callback function that is
> responsible for filling the bounce buffers. **As this driver does not care where the written pixels
> come from, this allows for the callback doing e.g., on-the-fly conversion from a smaller, 8-bit-per-
> pixel PSRAM frame buffer to a 16-bit LCD, or even procedurally generated frame-buffer-less
> graphics.**"*

That is Espressif explicitly blessing the technique the question is asking about. The API:

```c
/* no_fb mode: driver allocates NO framebuffer. You fill every bounce buffer yourself. */
esp_lcd_rgb_panel_config_t cfg = {
    .no_fb = 1,
    .bounce_buffer_size_px = 800 * 8,     /* 2 × 12.8 KB in SRAM */
    /* ... */
};
esp_lcd_rgb_panel_register_event_callbacks(panel, &(esp_lcd_rgb_panel_event_callbacks_t){
    .on_bounce_empty = my_bounce_fill,
}, NULL);

/* Called from ISR context, at panel rate. Must fill len_bytes fully. */
bool my_bounce_fill(esp_lcd_panel_handle_t panel, void *bounce_buf,
                    int pos_px, int len_bytes, void *user_ctx);
```

`pos_px` tells you where you are in the frame, so the callback knows which source rows to read.
**This is a raster driver** — the question is only what you can afford to do inside it.

---

## 3. What fits in a 15-cycle pixel budget

Measured at 240 MHz, 62.5 ns per pixel, 800 px per line, 51.25 µs per line.

| Operation in the bounce callback | Cycles/px | Fits 15? | CPU load at 39 Hz |
|----------------------------------|-----------|----------|-------------------|
| Plain 16-bit copy from PSRAM | ~2–3 | ✅ | ~5 % |
| 8-bit → 16-bit LUT expansion | ~4–6 | ✅ | ~10–15 % |
| 32-bit batched LUT expansion | ~2.5–3.5 | ✅ | ~6–9 % |
| RGB565 → RGB565 blend | ~5–8 | ⚠️ | ~12–20 % |
| 2× nearest-neighbour upscale | ~3–5 | ✅ | ~8–12 % |
| **Text with per-pixel alpha (LVGL)** | **~30–80** | ❌ | — |
| **Any LVGL widget rendering** | **~40–150** | ❌ | — |

**The line is clear:** the bounce callback can do *format conversion and scaling* — cheap, uniform
work on a contiguous buffer. It cannot do *scene rendering* — that is irregular, cache-hostile, and
3–10× over budget.

**This reframes the question correctly.** A raster driver is not a replacement for the framebuffer on
this panel. It is a **format converter on the way out** — and that is still useful.

---

## 4. The real option space

All figures for 800×480 @ 39 Hz. "Panel traffic" is the continuous PSRAM read the panel performs —
the fixed cost identified in [Render Performance §1.2](RENDER_PERFORMANCE.md#12-the-panel-takes-a-fixed-cut-forever).

| Profile | Framebuffer | Panel PSRAM traffic | Extra CPU | Colour | Tearing | Viable? |
|---------|-------------|---------------------|-----------|--------|---------|---------|
| **`balanced`** *(current default)* | 2 × 16-bit = **1.5 MB** | **32.0 MB/s** | 0 | 65 536 | none | ✅ Default |
| **`lowmem`** | 1 × 16-bit = **0.77 MB** | 32.0 MB/s | 0 | 65 536 | some, full-screen only | ✅ Already planned |
| **`raster8`** — 8-bit fb + LUT | 2 × 8-bit = **0.77 MB** | **16.0 MB/s** | **~6–15 % of a core** | 256 | none | ⚠️ Needs measurement |
| **`scaled`** — 400×240 fb, 2× upscale | 2 × 16-bit = **0.38 MB** | **7.5 MB/s** | ~8–12 % of a core | 65 536 | none, but blocky | ⚠️ Bad for text |
| **`direct`** | 2 × 16-bit = 1.5 MB | 32.0 MB/s | 0 (no copy) | 65 536 | none | ⚠️ Video only |
| **`beam`** — no fb at all | **0** | 0 | **~100 % of a core** | 65 536 | none | ❌ Not for LVGL |

### 4.1 `raster8` — the interesting one

Store the framebuffer as **8-bit palette indices** in PSRAM, expand to RGB565 in the bounce callback
via a 256-entry LUT.

| | `balanced` | `raster8` | Delta |
|---|---|---|---|
| Framebuffer memory (2 fbs) | 1.5 MB | **0.77 MB** | **−768 KB** |
| Panel PSRAM traffic | 32.0 MB/s | **16.0 MB/s** | **−16 MB/s** |
| Flush-copy traffic per full update | 768 KB | **384 KB** | **−384 KB** |
| Extra CPU | 0 | ~6–15 % of a core | +6–15 % |
| Colours | 65 536 | 256 | −65 280 |

**This is a genuinely good trade for bandwidth** — it halves the panel's unconditional 32 MB/s cut,
taking PSRAM contention from ~38 % down to ~19 %. It is the single biggest bandwidth lever available
after PSRAM clock speed.

**And a genuinely bad trade for CPU.** Two problems:

1. **The callback runs in ISR context.** A 6–15 % ISR load that recurs every 410 µs adds latency to
   *every* interrupt in the system — including the Wi-Fi MAC. This is the same class of problem as
   doing I²C in the render loop.
2. **Quantisation costs CPU twice.** LVGL renders 16-bit into SRAM; the flush must quantise to 8-bit
   (a ~1–2 cycle/px pass); the bounce callback then expands back. Two passes for one saving.

**Verdict:** implement it as an **opt-in profile**, measured, not assumed. It is the right answer for
a 4 MB PSRAM board where memory is genuinely scarce, and the right answer on the 8048S070 *if* Phase 1
profiling shows PSRAM bandwidth still saturating after the SRAM-render fix.

### 4.2 `beam` — not viable for us, worth stating why

A true framebuffer-less driver (procedural pixels, no stored image) is possible and the ESP-IDF docs
say so. But it requires generating each pixel in ~15 cycles, which rules out any LVGL content. Its
only legitimate uses here are a boot splash and a full-screen video decode path — neither of which
justifies a second render pipeline.

### 4.3 `scaled` — rejected for a UI

Rendering at 400×240 and upscaling 2× gives the same bandwidth win as `raster8` with no colour loss
and lower CPU. But 2× nearest-neighbour makes 16 px text look like a 1990s LCD. For a device whose
primary content is text (Settings, Logs, App Store), that is disqualifying. Revisit only if a game
or dashboard app specifically opts in.

---

## 5. The thing that actually matters: memory is not the binding constraint

It is worth saying plainly, because the framebuffer *looks* like the biggest allocation:

| Resource | `balanced` uses | Available | Headroom |
|----------|-----------------|-----------|----------|
| **PSRAM capacity** | 5.8 MB | 8 MB | **2.2 MB (28 %)** |
| **PSRAM bandwidth** | 32 MB/s (panel) + render | ~85 MB/s | ~50 MB/s |
| **Internal SRAM** | 359 KB | 512 KB | **153 KB (30 %)** |

**We are not short of PSRAM capacity.** The 1.5 MB framebuffer is 19 % of it. The problem identified
in the previous review round was **bandwidth**, not capacity — and that is fixed by rendering into
SRAM ([Render Performance §2](RENDER_PERFORMANCE.md#2-the-fix-render-in-sram-never-in-the-psram-framebuffer)),
which costs *more* memory, not less.

So the correct priority order for saving memory is **not** the framebuffer:

| # | Saving | Amount | Cost | Verdict |
|---|--------|--------|------|---------|
| 1 | **Stream image assets instead of caching them all** | up to **768 KB** | Decode on demand, slight scroll cost | ✅ Do this |
| 2 | **Single framebuffer (`lowmem`)** | **768 KB** | Tearing during full-screen animation only | ✅ Ship as a profile |
| 3 | Cap the LVGL heap at what the current screen needs | ~500 KB | Occasional allocation failure on heavy screens | ⚠️ Tune |
| 4 | `raster8` 8-bit framebuffer | 768 KB | 256 colours + ~6–15 % ISR load | ⚠️ Opt-in only |
| 5 | `scaled` half-resolution | 1.1 MB | Text looks bad | ❌ No |

Items 1 and 2 together free **1.5 MB** — as much as the entire `raster8` saving — with no colour loss
and no ISR load.

---

## 6. A real bug that affects our default profile

Worth flagging, because `balanced` uses bounce buffers: **esp-idf issue #19070 / IDFGH-18263**.

> *"esp_lcd RGB (bounce-buffer mode): bounce buffer selection by `bb_eof_count % 2` is never
> re-synchronised when `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` — one lost EOF corrupts the picture
> permanently."*

If a single DMA end-of-frame interrupt is coalesced, the driver's bounce-buffer parity inverts and
every subsequent refill lands in the buffer being scanned — permanent corruption. The reported
real-world trigger is `esp_wifi_start()` (PHY init) racing the panel.

**Implication for Phase 1:** this must be tested explicitly, with Wi-Fi starting while the panel is
running. If it reproduces on v6.1.1, the options are:

1. Use `no_fb` and select the buffer from the GDMA descriptor address (the workaround in the issue).
2. Drop bounce buffers (`bounce_buffer_size_px = 0`) and let EDMA read PSRAM directly — trades some
   PSRAM efficiency for correctness.
3. Pin the panel's core and start Wi-Fi from the other core (the issue notes this avoids it).

This is a **Phase 1 exit criterion**, not a Phase 8 cleanup item — it corrupts the picture.

---

## 7. Recommendation

1. **Keep `balanced` as the default.** 2 × 16-bit framebuffers in PSRAM. We have 2.2 MB of headroom
   and no reason to spend CPU to save memory we do not need.
2. **Ship `lowmem` as a supported profile** — single framebuffer, 768 KB saved, already planned.
3. **Implement `raster8` as an opt-in profile in Phase 8**, gated on measurement. It is the correct
   answer for a 4 MB PSRAM board and a legitimate lever if bandwidth still saturates. Do **not** make
   it the default: the ISR cost is real and the colour loss is visible.
4. **Do not implement `beam`.** It cannot render LVGL content and adds a second pipeline for no gain.
5. **Do not implement `scaled`** for general UI use.
6. **Verify the bounce-buffer parity bug** before anything else in Phase 1 (§6).
7. **Add the image-asset streaming item** to Phase 8 — it saves as much as `raster8` for free.

### Profile table, updated

| Profile | Framebuffers | LVGL mode | SRAM | PSRAM | Panel traffic | Status |
|---------|-------------|-----------|------|-------|---------------|--------|
| `balanced` | 2 × 16-bit | `PARTIAL` | ~77 KB | 1.5 MB | 32 MB/s | **Default** |
| `lowmem` | 1 × 16-bit | `PARTIAL` | ~55 KB | 0.77 MB | 32 MB/s | Supported |
| `raster8` | 2 × 8-bit + LUT | `PARTIAL` + quantise | ~80 KB | 0.77 MB | **16 MB/s** | **Opt-in, Phase 8** |
| `direct` | 2 × 16-bit | `DIRECT` | ~30 KB | 1.5 MB | 32 MB/s | Video only |
| `scaled` | 2 × 16-bit @ 400×240 | `PARTIAL` + upscale | ~60 KB | 0.38 MB | 7.5 MB/s | Rejected |
| `beam` | none | n/a | ~30 KB | 0 | 0 | Rejected |
| `zerocopy` | none | n/a | ~30 KB | 0 | 0 | ≤480×320 only |

---

## 8. Acceptance criteria added by this document

| ID | Criterion |
|----|-----------|
| **A26** | The **bounce-buffer parity bug** (IDFGH-18263) is tested explicitly on v6.1.1 with Wi-Fi starting during panel operation; the result and any workaround are recorded |
| **A27** | `raster8` is measured, not assumed: if implemented, the ISR load is ≤ 15 % of a core and PSRAM panel traffic is ≤ 16 MB/s |
| **A28** | Image assets are **streamed, not fully cached**; the decoded-image cache is capped at 768 KB and evicts LRU |
| **A29** | On a 4 MB PSRAM board, the `lowmem` profile boots and runs the launcher within the same SRAM budget as `balanced` |
