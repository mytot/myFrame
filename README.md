# myFrame

Self-hosted, app-store-driven firmware for the Sunton **ESP32-8048S070C** (ESP32-S3-WROOM-1 N16R8, 7″ 800×480 RGB, GT911) — and any future board described by a `device.json`.

**ESP-IDF v6.1.1 · LVGL 9 · wasm3 · Ed25519**

## What it is

*FrameOS* — the shell that boots on power-on — plus an app store. Apps come in two flavours:

* **Managed (WASM)** — a `.wasm` module hot-loaded into wasm3 in ~10 ms, no reboot. Pure UI: the app describes a tree of nodes (`frame_app.h`) and the shell renders it with LVGL.
* **Native** — a normal ESP-IDF app image written to its own OTA slot and booted. Full access to the SDK; a reboot to enter and a reboot to leave.

Both are delivered as signed `.fpack` ZIPs from the same catalog, verified by the same Ed25519 code on the laptop and on the device.

## Repo map

```
devices/index.json                 board index (one entry today)
devices/sunton_esp32_8048s070/     board definition: device.json, partitions.csv, board.c, sdkconfig.defaults
components/frame_pure/             host-testable logic: semver/json/base64url/sha256/sha512/canonical/manifest/slot_table/ed25519/zip
components/frame_{core,display,input,log,storage,net,ota,ui,shell,apps_builtin,store,wasm,app}/  ESP-IDF components
main/                              shell entry point (app_main.c)
sdk/wasm/frame_app.h               managed App SDK (the header apps include)
examples/{native_hello,wasm_hello}/  minimal native and managed apps
tools/{pack,catalog,devstore,build,build_app,frame_pkg,frame_crypto,gen_*}.py  packaging + hosting
tests/{test_pure,test_store_logic,test_crypto,test_tools}.c/.py  host tests
docs/                              architecture, render, driver, launcher, store, SDK, …
```

## Quick start

```bash
# host tests (no IDF needed)
make              # or: ./tests/run.sh ;  python3 tests/test_tools.py

# install the WASM example into a local store and serve it
python3 tools/build_app.py examples/wasm_hello/app.c --manifest examples/wasm_hello/manifest.json --out dist/clock.fpack --serve dist
python3 tools/devstore.py --pkgs dist --port 8080

# build the shell (needs ESP-IDF v6 + the dev store key)
. $IDF_PATH/export.sh
idf.py build
# or: python3 tools/build.py --release --key /safe/place/store_root.pub

# pack a native app (build it as an ESP-IDF app, then pack the binary)
idf.py -B build-native -DBOARD=sunton_esp32_8048s070 build
python3 tools/pack.py --manifest examples/native_hello/manifest.json --payload build-native/frame_native_hello.bin --out dist/counter.fpack

# verify exactly the way the device will
python3 tools/pack.py --verify dist/counter.fpack --pubkey keys/dev/publisher.pub --device-code
```

Set the device's store URL in **Settings → App store → Server** to `http://<laptop>:8080`, open **App Store** on the device and pull to refresh — or drag a `.fpack` onto the dev-store browser UI at `http://<laptop>:8080`.

## Documentation

| # | Doc | What it covers |
|---|-----|----------------|
| 01 | [Implementation Plan](docs/IMPLEMENTATION_PLAN.md) | Phases, deliverables, acceptance criteria |
| 02 | [Architecture](docs/ARCHITECTURE.md) | Layering, boot order, threading, memory map |
| 03 | [Render Performance](docs/RENDER_PERFORMANCE.md) | Why it actually renders at 39 Hz |
| 04 | [Framebuffer Memory](docs/FRAMEBUFFER_MEMORY.md) | Why the CYD trick doesn't transfer |
| 05 | [Device Support](docs/DEVICE_SUPPORT.md) | The board abstraction |
| 06 | [Display Driver](docs/DISPLAY_DRIVER.md) | Buffer profiles, flush contract |
| 07 | [Launcher](docs/LAUNCHER.md) | Home screen |
| 08 | [App Store](docs/APP_STORE.md) | Package format, signing, slots |
| 09 | [App SDK](docs/APP_SDK.md) | Native vs managed APIs |

## Docs & Flasher

**Docs:** https://mytot.github.io/myFrame/ &nbsp;·&nbsp; **Web Flasher:** https://mytot.github.io/myFrame/flasher/ — pick your board and flash the latest release from the browser (Chrome/Edge, Web Serial).

Releases are built in CI (`.github/workflows/release.yml`) per board: `myframe-<board>.factory.bin` (full flash) and `myframe-<board>.ota.bin` (OTA). Tag `v*` to publish.

## Boards

One today — the Sunton ESP32-8048S070C — described declaratively in `devices/sunton_esp32_8048s070/device.json` (pins, timings, PSRAM, touch, SD). Adding a board is a new directory plus an entry in `devices/index.json`; the firmware source is identical (see `docs/DEVICE_SUPPORT.md`).

## License

MIT — see [LICENSE](LICENSE).
