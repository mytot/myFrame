# myFrame — App Store

> Requirement: *"Design app store and how app can be added dynamically to esp32 along with future updates."*

---

## 1. The core constraint, stated honestly

An ESP32-S3 **cannot dynamically link native code at runtime** in any safe, supported way:

- Code must execute from IRAM (512 KB, shared with everything) or from flash mapped into the
  instruction cache.
- PSRAM on S3 is **data-only** — it cannot hold executable code.
- `esp_mmu_map(ESP_MMU_VADDR_EXEC)` can map flash into the instruction address space, but it demands
  position-independent code, gives you no relocation support and no rollback, and breaks ISR
  registration. It is a research project, not a product feature.

So "dynamically add an app" has to mean one of two things, and myFrame supports both:

| | **Native app** | **Managed app (WASM)** |
|---|---|---|
| Mechanism | Separate firmware image → switch boot partition → restart | `.wasm` module → load into wasm3 → run in-process |
| "Dynamic"? | Yes — installed, replaced and removed at runtime, no reflash | Yes — and hot-swapped without a reboot |
| Cost to switch | ~300–500 ms restart | ~5–20 ms |
| Performance | Native | Interpreted, ~10–40× slower |
| Isolation | Process-level | Interpreter sandbox |

**Both ship in v1.** The package format, catalog, signing and store UI are identical for both kinds —
the only difference is what `payload.bin` contains and what happens at launch.

The launcher is the front door to all of it: boot lands on the launcher, every installed app is a
tile there, and the store is a pinned tile. See [Launcher](LAUNCHER.md).

---

## 2. Package format — `.fpack`

A ZIP container. ZIP is chosen over a custom binary format because it is debuggable with `unzip`,
streamable, and has a tiny reader (~4 KB) already available for ESP32.

```
myframe-clock-1.2.0.fpack
├── manifest.json        # required — the contract
├── payload.bin          # required — native: ESP app image; managed: .wasm module
├── icon.png             # optional — 128×128, shown in the launcher
├── signature.bin        # required — Ed25519 over canonical(manifest) ‖ payload
└── assets/…             # optional — extracted to /apps/<id>/
```

### `manifest.json`

```json
{
  "schema": 1,
  "id": "dev.frame.clock",
  "name": "Clock",
  "version": "1.2.0",
  "kind": "native",
  "sdk": "^1.0.0",

  "requires": {
    "chip":      ["esp32s3"],
    "psram_mb":  4,
    "display":   { "min_width": 480, "min_height": 320, "touch": true },
    "audio":     false
  },

  "permissions": ["net", "storage:app"],

  "entry":  "payload.bin",
  "size":   421376,
  "sha256": "9f2c…",

  "description":    "A large analogue clock with world-clock support.",
  "author":         "Frame Labs",
  "homepage":       "https://example.com/clock",
  "release_notes":  "Fixes DST handling for southern-hemisphere time zones."
}
```

Field rules:

- `id` — reverse-DNS, immutable, primary key in the registry.
- `version` — strict semver (`MAJOR.MINOR.PATCH`), compared numerically, never lexically.
- `kind` — `native` | `managed`. Decides the execution path.
- `sdk` — semver range checked against `FRAME_API_VERSION`. A mismatch is a hard install failure.
- `requires` — re-validated on-device. The store's filtering is UX, not security.
- `permissions` — declared intent; enforced for managed apps, advisory for native (native apps run
  with full privileges by construction — this is stated plainly rather than pretended otherwise).

### Canonicalisation

`manifest.json` is re-serialised with sorted keys, no insignificant whitespace and UTF-8 before
signing, so that a signature taken on a laptop is reproducible on the device. Getting this wrong
is the classic "works on my machine" of package signing.

---

## 3. Trust model

Three keys, three jobs. Keys never live in the repo except the public halves.

| Key | Signs | Stored where | Rotation |
|-----|-------|--------------|----------|
| **Store root** | the catalog index | device NVS (pinned at first use), overridable in dev mode | manual, via a signed catalog update |
| **Publisher** | an individual `.fpack` | embedded in the catalog entry, which the store root endorses | by publishing a new catalog |
| **System** | FrameOS self-OTA images | `sdkconfig` + NVS, never writable by the app store | requires a signed FrameOS update |

Chain: `store_root` signs the catalog → catalog carries `publisher.pub` for each app →
`publisher` signs the package. A compromised publisher key cannot forge a catalog entry; a
compromised store key cannot sign a FrameOS update.

All verification uses **Ed25519 via PSA Crypto** (`psa_verify_hash()`), which is the supported path
in ESP-IDF v6's MbedTLS 4.x / PSA stack — no hand-rolled crypto.

### Dev mode

`CONFIG_FRAME_STORE_ALLOW_UNSIGNED=y` accepts unsigned packages **and** disables the network
catalog (local/sideloading only). It is `n` by default, and the build script refuses to produce a
`--release` artefact with it enabled. There is no way to ship an unsigned build by accident.

---

## 4. Catalog

A JSON document. In v1 it is served by the **dev store** (a local HTTP server); the static-hosted
variant is the same file dropped on GitHub Pages or S3 with no server-side logic at all.

```json
{
  "schema": 1,
  "generated_at": "2026-11-22T10:00:00Z",
  "apps": [
    {
      "manifest":      { "...the full manifest.json, inline..." },
      "download_url":  "https://store.example.com/pkgs/myframe-clock-1.2.0.fpack",
      "size":          421376,
      "sha256":        "9f2c…",
      "publisher_key": "ed25519:MCow…",
      "signature":     "base64…"
    }
  ]
}
```

`catalog.sig` is an Ed25519 signature over `catalog.json` with the store root key.

Device behaviour:
- Fetch on: app-store open, launcher refresh (pull-to-refresh), and every 24 h in the background.
- `If-None-Match` / ETag to make re-fetch free.
- Last-known-good catalog cached in NVS, so the store still opens offline.
- Catalog size capped at 256 KB; pagination via `?since=<cursor>` if it ever grows.

### Local / offline mode

Drop `.fpack` files into `/sdcard/apps/` (or upload over the device's HTTP API at
`POST /api/apps/install`). The package registry scanner picks them up on the next store refresh.
This is also the escape hatch when the network is down or the store key needs replacing.

---

## 4a. Dev store (v1 default)

Shipping the store from a laptop is how the format and the install flow get debugged quickly.
`tools/devstore.py` is a single-file Python server with no third-party dependencies.

```
laptop                                    device
┌───────────────────────────┐             ┌─────────────────────────────┐
│ tools/devstore.py :8080   │             │ FrameOS                     │
│                           │◄── GET ─────│ GET /catalog.json           │
│  • serves catalog.json    │             │ GET /pkgs/*.fpack           │
│  • serves .fpack files    │────────────►│                             │
│  • serves a browser UI    │             │ POST /api/apps/install      │
│    for drag-drop install  │◄── POST ────│ POST /api/catalog/refresh   │
│  • /sign                  │             │                             │
└───────────────────────────┘             └─────────────────────────────┘
```

Two directions, both useful:

| Direction | When |
|-----------|------|
| **Device → dev store** (device is the HTTP client) | Normal case: the device's store app points at `http://<laptop>:8080` and browses/installs |
| **Laptop → device** (laptop drives the device's HTTP API) | Debugging: curl/Postman installs a package directly, no UI interaction needed |

The dev store also exposes `POST /sign` so `pack.py` can sign against a key that never leaves the
laptop, and it can be run with `--allow-unsigned` to mirror the device's dev mode.

**Promotion path:** once the format is stable, the same `catalog.json` + `.fpack` files are pushed
to a static host and the only change is the store URL in settings. Nothing in the device client
differs — that is deliberate, so the hosting decision can be made late.

### Device HTTP API

```
GET  /api/apps                     installed apps + version + update flag
POST /api/apps/install             multipart .fpack upload → verify → install
POST /api/apps/<id>/update         install newest catalog version
POST /api/apps/<id>/uninstall
GET  /api/catalog                  verified catalog, as the device sees it
POST /api/catalog/refresh          force re-fetch
GET  /api/logs?level=…&tag=…       log ring buffer
POST /api/launcher/open/<id>       launch an app (test automation)
```

The API is enabled by `CONFIG_FRAME_DEV_API` (default **y** in debug builds, **n** in release)
and is authenticated with a per-device token printed on the Settings → Developer screen. It is a
development surface, not a public one.

---

## 5. Install flow (native)

```
  user taps Install
        │
        ▼
  1. manifest checks        schema, sdk range, requires, permissions disclosed
        │
        ▼
  2. reserve a slot         free slot → else evict LRU uninstallable app
        │
        ▼
  3. download               stream to SD if present, else 512 KB PSRAM window
        │                   progress callback → UI
        ▼
  4. verify signature       Ed25519 over canonical(manifest) ‖ payload
        │                   + sha256(payload) == manifest.sha256
        ▼
  5. esp_ota_begin(slot)    erases the slot region
        │
        ▼
  6. esp_ota_write()        chunked from the download stream
        │
        ▼
  7. esp_ota_end()          validates the ESP image header, hash and segment table
        │
        ▼
  8. commit registry        nvs_set_blob("apps/<id>") — ONLY after step 7 succeeds
        │
        ▼
  9. extract icon + assets  → /apps/<id>/
        │
        ▼
  10. done                  app appears in launcher
```

**The atomicity rule:** the registry entry is the single source of truth, and it is written *last*.
A power cut anywhere in steps 3–8 leaves an erased or half-written slot that the registry knows
nothing about — so it is simply reclaimed on the next boot. The app never appears half-installed.

### Slot allocation

| State | Meaning |
|-------|---------|
| `FREE` | erased, available |
| `RESERVED` | claimed by an in-flight install (with an expiry timestamp) |
| `VALID` | holds a committed image, referenced by the registry |
| `STALE` | referenced by no registry entry — reclaimable |

`frame_ota` owns this table in NVS and runs a sweep at boot that demotes anything orphaned to
`STALE` → `FREE`. `RESERVED` entries older than 10 minutes are also reclaimed, which is how a
power cut mid-install self-heals.

### Launch

```
  shell writes boot_handoff = {app_id, slot, shell_slot, sdk_ver}
  shell draws a "launching <app>" frame   ← hides the restart
  esp_ota_set_boot_partition(app_slot)
  esp_restart()
        ...
  bootloader loads the app image
  app reads boot_handoff → confirms it was launched, runs
  user presses Home → frame_exit_to_shell()
        → esp_ota_set_boot_partition(shell_slot); esp_restart()
```

**Why the splash matters:** the restart is unavoidable (~300–500 ms: bootloader + PSRAM init +
LVGL init). Drawing a deliberate transition frame turns "the device froze for a moment" into
"the app is opening". Measured in Phase 5; target ≤1.5 s end-to-end (acceptance A8).

### Why the shell is never at risk

The store can only ever write to `app_*` slots. `shell_a` / `shell_b` are written exclusively by
`frame_ota`'s self-update path, which requires the separate **system** key. A malicious or buggy
app therefore cannot brick the device — worst case, the bootloader rolls back to the shell.

### Install flow (managed / WASM)

Shorter, because there is no slot and no restart:

```
  1–4.  same manifest checks, download, signature verify
  5.    extract payload.wasm + assets → /apps/<id>/
  6.    commit registry entry  (nvs_set_blob)
  7.    launcher tile appears immediately
```

- **No flash partition involved** — the module lives in the FAT filesystem, which is what makes it
  instantly replaceable and instantly removable.
- **On launch:** `frame_wasm` mmaps the module, instantiates it against a bounded heap
  (default 384 KB, capped at 512 KB), and registers its host imports. Target ≤ 500 ms from tap to
  first rendered frame (acceptance A15).
- **On exit:** the module is unloaded and its heap returned. Nothing persists between launches
  except what the app wrote to `kv`.
- **Failure containment:** a trap, an out-of-memory, or an over-long call (per-app watchdog) marks
  the app failed and returns to the launcher. The shell process is untouched (acceptance A16).
- **Updates are a file swap** — write the new `.wasm` to a temp path, then `rename()` over the old
  one. A FAT rename is atomic enough that a power cut leaves either the old or the new file, never
  a hybrid.

---

## 6. Updates

```
  catalog shows Clock 1.2.0 → 1.3.0 available
        │
        ▼
  1. pick target slot     a slot that is not the one currently holding 1.2.0
        │
        ▼
  2–8. install 1.3.0     exactly as above, into the new slot
        │
        ▼
  9. registry swap       single NVS write: {id → new slot, new version}
        │                old slot becomes STALE
        ▼
 10. old slot freed      on next boot sweep
```

- **The old version keeps running** until the registry write commits. If the update fails at any
  point, the app is untouched.
- **Rollback:** the new image boots in `PENDING_VERIFY` state. The app calls
  `frame_app_mark_healthy()` after ~10 s of successful operation. If it never does — because it
  crashed, hung, or panicked — the bootloader reverts to the previous valid image on the next boot,
  and FrameOS marks the app as "needs attention" in the launcher.
- **Anti-rollback (optional, per publisher):** the registry stores the highest version ever
  installed for an `id`. A package with a lower version is refused. Prevents downgrade attacks.

### Delta updates

Full packages in v1. Delta (bsdiff / heatshrink) is a Phase 8+ optimisation — worth it only once
packages routinely exceed ~1 MB, and only if the device's typical uplink is slow. The manifest
already carries `size`, so the UI can show "1.3.0 (410 KB)" vs "delta (38 KB)" without a format
change.

---

## 7. Uninstall

```
  1. erase the slot               esp_ota_erase_partition()
  2. delete registry entry        nvs_erase_key("apps/<id>")
  3. delete assets                recursive rmdir /apps/<id>/
  4. free the slot                slot table → FREE
```

Order matters: erase flash *before* dropping the registry entry, so an interrupted uninstall leaves
a `STALE` slot (reclaimed) rather than a registry entry pointing at erased flash (broken app).

---

## 8. FrameOS self-update

Separate path, separate key, standard IDF flow:

- `shell_a` / `shell_b` A/B slots.
- `esp_https_ota()` downloads, validates the **system** signature, writes the inactive slot.
- New image boots, runs a health check (display alive, LVGL rendering, NVS readable, 30 s uptime),
  then calls `esp_ota_mark_app_valid_cancel_rollback()`.
- Health check fails → watchdog → bootloader reverts to the previous slot.

Rollback is what makes shipping self-update safe; it is not optional.

---

## 9. Store UI

The store is reached in one tap from the launcher and is organised as **three tabs on one screen**,
so "what do I have", "what needs updating" and "what can I get" are all one swipe apart.

```
┌──────────────────────────────────────────────────────────────────┐
│  ‹ Store                      ⟳                      🔍           │
├──────────────────────────────────────────────────────────────────┤
│   Installed    │    Updates ●3    │    All                         │
├──────────────────────────────────────────────────────────────────┤
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ [icon] Clock            1.2.0 → 1.3.0      [ Update ]      │  │
│  │        dev.frame.clock · 410 KB                             │  │
│  ├────────────────────────────────────────────────────────────┤  │
│  │ [icon] Weather          2.0.0 → 2.1.1      [ Update ]      │  │
│  │        dev.frame.weather · 1.1 MB                           │  │
│  ├────────────────────────────────────────────────────────────┤  │
│  │ [icon] Notes            1.0.0               [ Open ]       │  │
│  │        dev.frame.notes · 210 KB                             │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│              [ Update all (3) ]                                  │
└──────────────────────────────────────────────────────────────────┘
```

| Tab | Contents | Primary action |
|-----|----------|----------------|
| **Installed** | Every installed app, its version, and an *Update* button where a newer version is cached | Open / Update / Uninstall |
| **Updates** | Filtered to apps with a newer version available | *Update all* + per-app rows |
| **All** | The browsable catalog, searchable, filterable by category and by "works on this device" | Install |

**Detail screen:** icon, name, publisher, description, permissions (plain-language), download size,
release notes, version history, and Install / Update / Open / Uninstall as appropriate.

**Install progress is a sheet, not a blocking dialog.** The user can dismiss it and keep browsing;
the install continues on the store task and posts a toast on completion.

**After install, the app appears on the launcher immediately** — no reboot is needed to see the
tile. A reboot only happens on the first *launch* of a native app, and never at all for a managed app.

The store is itself a built-in app (`frame_apps_builtin/store`), built on `frame_ui` — not a special
mode, and it holds no privilege that a third-party app could not also hold.

### Launcher integration

The launcher's status bar shows an update pill (`⟳ 3`) computed from the cached catalog. Tapping it
deep-links into the store's *Updates* tab. Individual app tiles carry a dot when an update exists,
so the user learns about updates without ever opening the store. Full detail in
[Launcher §5](LAUNCHER.md#5-app-store-as-a-launcher-surface).

---

## 10. Failure modes

| Scenario | Behaviour |
|----------|-----------|
| Network drops mid-download | Partial file discarded; slot released back to `FREE`; UI shows "download interrupted, retry?" |
| Signature invalid | Rejected at step 4, before any flash write. Logged with the key fingerprint. |
| `esp_ota_end()` fails (corrupt image) | Slot erased, registry untouched, app never appears |
| Slot table full | Evict the least-recently-launched uninstallable app, after asking the user |
| Disk full on SD | Falls back to the PSRAM streaming window; large packages may be refused with a clear message |
| Catalog signature invalid | Catalog rejected; cached last-known-good used; UI shows a warning banner |
| Clock skew breaks TLS | Device refuses HTTPS until NTP sync; shows "setting time…" rather than a scary TLS error |
| App crashes on launch | Bootloader rollback to shell; launcher shows "Clock crashed last time — Reinstall / Remove" |
| WASM module traps mid-run | App marked failed, module unloaded, heap returned; launcher shows a warning triangle on the tile |
| WASM module exceeds its heap | Allocation returns `NULL` inside the sandbox; app sees an allocation failure like any other; shell unaffected |
| WASM app hangs | Per-app watchdog (default 10 s without a yield) terminates the module and returns to the launcher |
| Dev store unreachable | Store tile shows "Offline"; installed apps keep working; cached catalog still browsable |
| Device HTTP API token lost | Regenerate from Settings → Developer; the token is printed to the serial console at boot |

---

## 11. Tooling

```bash
# build an app into a signed .fpack
python3 tools/pack.py --app apps/clock --out dist/clock-1.2.0.fpack \
                      --key ~/.myframe/publisher.pem

# build + sign the catalog from a directory of .fpack files
python3 tools/catalog.py --pkgs dist/ --out store/catalog.json \
                         --key ~/.myframe/store_root.pem

# serve it locally for development
python3 tools/devstore.py --root store/ --port 8080

# verify a package the way the device does (same code path, host build)
python3 tools/pack.py --verify dist/clock-1.2.0.fpack --pubkey store/publisher.pub
```

`pack.py --verify` links the *same* verifier source as the firmware, so a package that verifies on
the host is guaranteed to verify on the device.
