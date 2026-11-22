# myFrame — Launcher (Home Screen)

> Requirement: *"Device first loads launcher/home where all installed apps will be shown with app store;
> they can open and browse the app store to install new or update the existing ones."*

---

## 1. Role

The launcher **is** the home screen. It is the first thing the user sees after boot, the thing every
app returns to, and the only global surface in the system. It has exactly three jobs:

1. **Show every installed app** — installed native apps, installed managed (WASM) apps, and the
   built-in apps (Settings, Logs).
2. **Surface the app store** — a permanent, always-visible entry that opens the store.
3. **Show update state at a glance** — a badge on the store tile and on each app with an available
   update, so the user does not have to open anything to know an update exists.

The launcher is a built-in app (`frame_apps_builtin/launcher`) built on the same `frame_ui` and
`frame_app` APIs as everything else. It is not a special mode, and it holds no privilege that a
third-party app could not also hold — except that it is never uninstalled.

---

## 2. Layout

```
┌──────────────────────────────────────────────────────────────────────┐
│  ☰  myFrame            ⟳ 3 updates           Wi-Fi ▂▄▆   14:32  ▮▮▮ │  ← status bar
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│   │          │  │          │  │          │  │          │             │
│   │  ⊞ Store │  │ ⚙ Settings│  │ ☰ Logs  │  │ 🕐 Clock │             │
│   │          │  │          │  │          │  │     ●    │             │
│   │  Browse  │  │          │  │          │  │  1.2.0   │             │
│   └──────────┘  └──────────┘  └──────────┘  └──────────┘             │
│                                                                      │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│   │ 🌤 Weather│  │ 📝 Notes │  │          │  │          │             │
│   │     ●    │  │          │  │          │  │          │             │
│   └──────────┘  └──────────┘  └──────────┘  └──────────┘             │
│                                                                      │
│                      ● ● ○ ○                        ← page dots       │
└──────────────────────────────────────────────────────────────────────┘
```

### Zones

| Zone | Behaviour |
|------|-----------|
| **Status bar** (top, 32 px) | Device name, update count, Wi-Fi state, clock, brightness. Tapping it opens the quick-settings sheet (brightness slider, Wi-Fi toggle, sleep). |
| **Grid** | 4 columns × N rows at 800×480. Tiles are 160×140 px with 16 px gutters — large enough to hit with a fingertip, small enough to fit 8 apps per page. |
| **Page dots** (bottom) | One dot per page; current page filled. Hidden when there is only one page. |

### Tile anatomy

```
┌────────────────┐
│     ┌────┐     │   icon 64×64, RGB565, cached
│     │ ⚙ │     │
│     └────┘     │
│    Settings    │   name, ellipsised at 1 line
│      1.0.0     │   version, muted colour
│         ●      │   update badge (only when an update exists)
└────────────────┘
```

- **Update badge** — a 12 px dot in the accent colour, top-right of the icon, plus a number if
  more than one. Rendered from the locally cached catalog, so it works offline.
- **Store tile is pinned** to position 0, page 0, and never participates in reorder.
- **Built-ins** (Settings, Logs) are pinned to positions 1 and 2.
- **Installed apps** follow, ordered by last-launched time (most recent first), then alphabetically.

---

## 3. Interactions

| Gesture | Result |
|---------|--------|
| **Tap** | Launch the app (native: splash + reboot-switch; managed: instant) |
| **Long-press (400 ms)** | Context menu: *Open · App info · Uninstall* (uninstall disabled for built-ins and the store) |
| **Horizontal drag (> 80 px)** | Page between launcher pages, with a 220 ms eased transition |
| **Vertical drag down from the top** | Quick-settings sheet (brightness, Wi-Fi, sleep, about) |
| **Swipe up from the bottom edge** | Home — always returns to the launcher, from anywhere |
| **Two-finger tap** | Toggle edit mode (reorder / uninstall) — optional, off by default in v1 |

The **home gesture is global**, registered by `frame_input`, and works from inside any app
including native apps. That is what makes the "app → launcher" round trip feel like a phone rather
than a kiosk with a reboot button.

---

## 4. Boot sequence to launcher

Target: **≤ 3 s from power-on to an interactive launcher** (acceptance A3).

```
t=0.00   reset
t=0.35   2nd-stage bootloader done
t=0.60   app_main, PSRAM @120 MHz up, board init
t=0.85   panel + LVGL port up
t=0.95   first frame: brand splash drawn            ← device looks alive here
t=1.30   NVS + FatFS mounted, settings loaded
t=1.45   app registry read, launcher grid built
t=1.60   launcher visible and interactive           ← target
         (Wi-Fi connects in the background, icon cache warms lazily)
```

Two rules make this achievable:

- **Nothing on the boot path blocks on the network.** Wi-Fi connect, catalog refresh and NTP all
  happen on the net task after the launcher is already on screen.
- **Icons load lazily.** The grid renders with placeholder tiles, then decodes icons in the LVGL
  task at idle priority. A slow or missing icon never delays the launcher.

---

## 5. App store as a launcher surface

The store is a first-class launcher destination, not a separate flow.

```
Launcher  ──tap Store──►  Store: Browse
                            │  tabs: Installed │ Updates │ All
                            │
                            ├─ Installed → Open · Update · Uninstall
                            ├─ Updates   → "Update all" + per-app rows
                            └─ All        → catalog grid, search, categories
                                        │
                                        └─ Detail → Install / Update
                                                      │
                                                      └─ Progress sheet (%, ETA, cancel)
```

- **Three tabs, one screen.** *Installed* shows what is on the device with its version and an
  *Update* button where applicable. *Updates* is the same list filtered to apps with a newer
  version available, with an *Update all* action. *All* is the browsable catalog.
- **The launcher's "3 updates" pill** jumps straight to the *Updates* tab.
- **Install progress is a sheet, not a blocking dialog** — the user can dismiss it and keep browsing;
  the install continues on the store task and posts a toast on completion.
- **After install**, the app appears on the launcher immediately (no reboot needed to see the tile;
  a reboot only happens on first *launch* of a native app).

This ordering is what makes the "device first loads launcher → browse → install → update" loop from
the requirement feel like one continuous experience.

---

## 6. Update discovery

Update state is **cached, not fetched on demand**, so badges render instantly:

| When | Action |
|------|--------|
| Boot (background) | Load cached catalog from NVS → compute update set → draw badges |
| Launcher open | If catalog is stale (> 1 h) and Wi-Fi is up, refresh silently; update badges when it lands |
| Pull-to-refresh in Store | Force refresh, show spinner |
| Every 24 h | Background refresh if Wi-Fi is up |

`frame_app_manager` owns a `frame_app_state_t` per app:

```c
typedef struct {
    char     id[64];
    char     name[48];
    char     installed_version[16];
    char     available_version[16];   /* "" if none cached */
    uint8_t  update_available;         /* derived: semver_cmp() > 0 */
    uint8_t  kind;                     /* native | managed | builtin */
    uint8_t  slot;                     /* 0xFF for builtin */
    char     icon_path[96];
} frame_app_state_t;
```

The launcher reads this list. It never talks to the store or the catalog directly — so the badge
logic is unit-testable on the host, and so a third-party launcher (if anyone ever writes one) gets
the same data.

---

## 7. Empty and degraded states

| State | What the user sees |
|-------|--------------------|
| Fresh device, nothing installed | Grid shows Store + Settings + Logs, plus a ghost tile: *"Install your first app →"* |
| No Wi-Fi, catalog never fetched | Store tile shows *"Offline"*; installed apps still launch normally |
| Catalog signature invalid | Warning banner in Store; cached catalog used if present |
| App crashed last launch | Tile carries a small warning triangle; context menu offers *Reinstall* |
| Slot table full | Store shows *"Storage full — uninstall an app to install another"* with a link to the Installed tab |

Every one of these is reachable from the launcher, so the device never dead-ends.

---

## 8. Rendering performance

The launcher is the screen the user sees most, so it is held to a strict budget — and it is subject
to the same render contract as every app. Full arithmetic in
[Render Performance](RENDER_PERFORMANCE.md).

| Metric | Target | Why |
|--------|--------|-----|
| First launcher frame after boot | ≤ 1.6 s | See §4 |
| Idle cost (static, no updates) | **0 swaps/s** | `dirty_px == 0` → no copy, no swap, no PSRAM traffic beyond panel scanout |
| Badge update (one tile) | ≤ 3 ms | A single 160×140 tile is ~45 KB of PSRAM traffic |
| Page swipe | **20 Hz**, ≤ 50 ms/frame | A page swipe is a full-screen transition — see the note below |
| Cold icon decode | ≤ 8 ms each, off the critical path | Decoded lazily at idle priority |
| Launcher RAM (tiles + icons) | ≤ 300 KB PSRAM | |

**Why page swipe is 20 Hz and not 30.** A full-screen 800×480 transition is ~24 ms of render + copy
on this hardware, which does not fit twice into a 33 ms frame. Rather than ship a stuttering 30 Hz
swipe, the launcher declares `FRAME_RENDER_CLASS_FULL` (20 Hz) for paging and makes the 220 ms eased
animation look deliberate at that rate. Everything else on the launcher — badges, toasts, tile
presses — is small-area and runs at 30 Hz without effort.

**The launcher's own rules, which are also the template for apps:**

- **Idle means idle.** Nothing animates on the launcher when the user is not touching it. The clock
  in the status bar updates once a minute and invalidates only its own label.
- **Icon cache in PSRAM**, keyed by `icon_path + mtime`. Decoded RGB565 bitmaps at 64×64 = 8 KB each;
  200 apps would be 1.6 MB, so the cache is capped at 1 MB with LRU eviction.
- **Only visible tiles are created.** Off-page tiles are created on demand as the user pages; this
  keeps the LVGL object tree small (LVGL redraw cost scales with object count, not pixel count).
- **Icons load lazily.** The grid renders with placeholder tiles, then decodes icons in the LVGL task
  at idle priority. A slow or missing icon never delays the launcher.
- **Reordering is a data operation**, not a UI one: the grid is rebuilt from the sorted registry,
  never mutated in place.
- **The update pill is a single label** updated by a dirty check, not a full status-bar redraw.

---

## 9. Acceptance criteria for the launcher

| ID | Criterion |
|----|-----------|
| L1 | Boot lands on the launcher in ≤ 3 s, interactive |
| L2 | Every installed app is visible on the launcher, with icon, name and version |
| L3 | The store is reachable in one tap from the launcher, always |
| L4 | Apps with a pending update show a badge without the store being opened |
| L5 | Tapping a native app reaches a usable app in ≤ 1.5 s |
| L6 | Tapping a managed (WASM) app reaches a usable app in ≤ 500 ms, with no reboot |
| L7 | Swipe-up-from-bottom returns to the launcher from any app, including native apps |
| L8 | Uninstalling an app removes its tile and reclaims its slot |
| L9 | Launcher survives a full page of 100 apps without frame drops while paging |
