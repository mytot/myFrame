# myFrame — Documentation

**myFrame** is a generic, multi-device ESP32 firmware platform: a reusable OS shell
plus an app store, with per-device display/board definitions living in the repo.

Target stack: **ESP-IDF v6.x** (v6.1.1 current stable) · **ESP32-S3** · **LVGL 9**.

| # | Document | What it covers |
|---|----------|----------------|
| 01 | [Implementation Plan](IMPLEMENTATION_PLAN.md) | Scope, phases, deliverables, acceptance criteria, risks, decisions |
| 02 | [Architecture](ARCHITECTURE.md) | Layering, runtime model, component map, boot flow, threading, memory map |
| 03 | [Render Performance](RENDER_PERFORMANCE.md) | **PSRAM bandwidth arithmetic, the render-in-SRAM design, the two-core schedule, the app render budget** |
| 04 | [Framebuffer Memory](FRAMEBUFFER_MEMORY.md) | **Can a raster driver save memory? Why the CYD trick doesn't transfer, the `raster8` option, and the bounce-buffer parity bug** |
| 05 | [Device Support](DEVICE_SUPPORT.md) | The board abstraction that makes firmware "generic", plus the Sunton 8048S070 definition |
| 06 | [Display Driver](DISPLAY_DRIVER.md) | The RGB panel driver: buffer profiles, flush contract, touch, backlight, power |
| 07 | [Launcher](LAUNCHER.md) | Home screen, app grid, update badges, boot-to-launcher budget |
| 08 | [App Store](APP_STORE.md) | Package format, signing, install/update/uninstall, slot management, dev store, rollback |
| 09 | [App SDK](APP_SDK.md) | The stable API/ABI apps are written against; native vs managed execution models |

---

## Reading order

- **Want to approve or reject the plan?** → [01 · Implementation Plan](IMPLEMENTATION_PLAN.md)
- **Want to know whether it can actually render?** → [03 · Render Performance](RENDER_PERFORMANCE.md) — read this before approving
- **Curious whether a raster driver could save memory?** → [04 · Framebuffer Memory](FRAMEBUFFER_MEMORY.md)
- **Want to know how it is built?** → 02 → 03 → 04 → 05 → 06
- **Want to know how apps get onto the device?** → 08 → 09

## Status

| Item | State |
|------|-------|
| Documentation | **Draft v2 — decisions recorded, awaiting final sign-off** |
| Firmware implementation | Not started (gated on plan approval) |

## Decisions locked in from review

| Question | Decision |
|----------|----------|
| App models | **Both native and managed (WASM) ship in v1** |
| First screen | **Launcher is the home screen**; all installed apps shown; app store is a first-class launcher entry |
| Store hosting | **Dev store first** (local HTTP), static HTTPS catalog as a fast follow |
| Package signing | **Ed25519 enforced from day one**, with a dev-mode bypass |
| Board variant | **Sunton ESP32-8048S070C** — capacitive GT911 touch |
