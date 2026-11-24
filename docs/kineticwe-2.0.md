# KineticWE 2.0 — Plan: No KDE Services, Full Control via the Kinetic Settings App

Status: Approved plan. Not yet implemented.
Scope: A fork of the Noctalia v5 shell (with the Kinetic Settings UI built in) + targeted patches to this compositor (KWin fork), producing a desktop environment that runs with zero KDE helper programs and manages all desktop settings from one settings UI (Kinetic Settings) and one config folder. Iteration 1 ships Kinetic Settings as a self-contained module inside the Noctalia fork that opens a full window; a standalone `kinetic-settings` app is the (deferred) iteration-2 extraction.

---

## Part 1 — The Plain-English Summary (for non-developers)

### What this plan is about

Right now, KineticWE is two pieces of software working together:

1. **KineticWE (the compositor)** — the engine that draws windows on your screen, handles tiling, moves and resizes things, and manages your displays, keyboard shortcuts, mouse, and workspaces.
2. **Noctalia (the shell)** — everything you see on top of that: the bar, app launcher, dock, notifications, lock screen, wallpapers, and its own settings app for shell appearance and behavior.
3. **Kinetic Settings (the settings UI)** — the settings for compositor/desktop options: displays, keyboard shortcuts, mouse and pointer, cursor, icons, effects, tiling, and more. In iteration 1 it is a module inside the Noctalia shell that opens a full window — the same way Noctalia's own settings does — so there is no separate app to maintain yet; a small `.desktop` entry (`dev.kineticwe.Settings.desktop`) wraps the shell's `kinetic-settings-open` IPC command so it also shows up in the launcher.

The catch: like most modern desktop environments, the current version quietly hires a few outside helpers that belong to the KDE project. Today there are exactly two *separate* helpers, plus one that is already built in:

- **xdg-desktop-portal-kde** — the "translator" that lets apps (like a browser sharing your screen) talk to your desktop.
- **PowerDevil** — the helper that puts your screen to sleep, suspends your computer, and dims the screen.
- **kglobalacceld** — the helper that makes keyboard shortcuts work. This one is already built *inside* the compositor, so it no longer counts as a separate helper.

The helpers work fine, but they come from KDE, and that means KineticWE is permanently tied to whatever KDE does with them — updates you don't control, features you don't want, and code you can't fully own.

### What KineticWE 2.0 does about it

1. **We keep the compositor** (it already does the heavy lifting), but we stop using KDE's helper programs.
2. **We fork Noctalia** — we make our own copy of the shell — and build a **Kinetic Settings** module into it for all compositor/desktop settings: displays, keyboard shortcuts, mouse and pointer, cursor, icons, effects, tiling, and more. Noctalia's own settings app keeps only shell-level look-and-feel options, which keeps upstream merges clean. No more needing KDE's "System Settings" for any of it.
3. **We replace the two outside helpers with our own small pieces** built right into our own code:
   - The "translator" app (portal) gets replaced with our own **`xdg-desktop-portal-kwe`**, using Noctalia's own notification system and the compositor's built-in screen-recording and screenshot features.
   - PowerDevil gets replaced with a tiny power module inside our shell that does the same things (sleep, screen-off, brightness) by talking directly to the system.
4. **We give the desktop one proper config folder.** All desktop settings live in `~/.config/kineticwe/` with sane defaults that are installed with the system. No more KDE scattering settings across a dozen different files. (KDE apps still get a small "compat" file so they keep working, but it's just an output of the Kinetic Settings app, not something you manage.)
5. **We cut the cord to KDE.** The session declares itself as "KineticWE" instead of pretending to be KDE/Plasma. Anything KDE-specific that has no replacement gets removed (for example, the rarely-used "Activities" feature).

### What you get

- **Full control.** Every setting for your desktop lives in one app — Kinetic Settings — and one config folder. You decide what it does and how it looks, no KDE "System Settings" needed.
- **No KDE helper software running.** A KineticWE session ends up with only: the compositor, the shell, and a generic freedesktop "portal" frontend (which is not KDE — it's the Linux-wide standard). The only KDE things that remain are *libraries* (like spare parts embedded inside the compositor, similar to how every program uses standard code libraries) — no KDE *programs* running in the background.
- **A clean first-run experience.** The first time a user logs in, the session seeds `~/.config/kineticwe/` from sane, documented defaults.
- **Lock screen included, for free.** Noctalia already has its own lock screen and idle settings in its own config. We use those; the compositor doesn't need any lock-screen settings of its own.
- **Your plugins, themes, and future updates keep working.** The fork keeps Noctalia's plugin system and community template catalog fully intact, and upstream shell updates can still be fetched and merged. Our additions are additive — they never break existing plugins or templates.

### What it does NOT mean

- It does not mean rewriting the window engine from scratch (that would take years).
- It does not mean KDE apps stop working — most KDE apps (Dolphin, Kate, etc.) work fine without KDE helpers; they're just apps.
- It does not mean you can't install KDE's System Settings anymore — you just won't need it for anything desktop-related.
- It does not affect people still using the current KineticWE. The 2.0 work happens on a separate branch and a separate shell repo (with Kinetic Settings built in); the current release keeps working untouched.

**Bottom line:** KineticWE 2.0 = our compositor + our fork of Noctalia with Kinetic Settings built in, all desktop config in one folder with sane defaults, running with zero KDE helper programs. You own the whole desktop.

---

## Part 2 — Current State (dev)

### 2.1 Session composition today

`scripts/start-kineticwe.sh` launches:

- `kinetic-we --xwayland` — KWin fork; embeds `KGlobalAccelD` in-process (`src/globalshortcuts.cpp:97`) and owns the `org.kde.kglobalaccel` D-Bus name itself. No external kglobalacceld daemon runs.
- `xdg-desktop-portal-kde` + `xdg-desktop-portal` — frontend is freedesktop; backend is KDE.
- `org_kde_powerdevil` + `upowerd`.
- `noctalia-kwe` (the Noctalia fork shell binary) under a supervised restart loop.
- Helpers: `kbuildsycoca6`, `dbus-update-activation-environment --systemd --all`.

Env identity today: `XDG_CURRENT_DESKTOP=KDE`, `XDG_SESSION_DESKTOP=KDE`, `KDE_SESSION_VERSION=6`, `KDE_FULL_SESSION=true`, `XDG_MENU_PREFIX=plasma-`.

### 2.2 Compositor-owned D-Bus / protocol surface (free to use — no KDE service)

All of the following are owned by the `kinetic-we` process itself; a settings app or background service can use them without any KDE daemon:

- `org.kde.KWin` (`/KWin`, `/VirtualDesktopManager`, `/Plugins`, `/InputDeviceManager`, `/InputDevice/<sysname>` — `src/dbusinterface.h`, `src/backends/libinput/connection.cpp:64`)
- `org.kde.KWin.ScreenCast` / `org.kde.KWin.Screenshot` (screencast/screenshot plugins, PipeWire-backed)
- Wayland protocols implemented by this fork: `kde-output-device-v2` + `kde-output-management-v2` (`src/wayland/outputdevice_v2.cpp`, `outputmanagement_v2.cpp`), `org_kde_plasma_virtual_desktop`, `kde_lockscreen_overlay_v1`, `idleinhibit`, DPMS, fractional scale, tablet/text-input, etc.
  - **Not implemented in this fork:** `ext-session-lock-v1`, `ext-workspace-v1` (upstream KWin has both; the fork doesn't).
- `org.freedesktop.a11y.Manager` — registered by the compositor itself (`src/a11ykeyboardmonitor.cpp:38`).
- `org.freedesktop.ScreenSaver` — implemented by the compositor (idle inhibition).
- `org.kde.KGlobalSettings` signal subscription — `src/cursor.cpp:121`; cursor theme/size is applied **at runtime** on `notifyChange` type 5 (`CursorChanged`). The signal is just a D-Bus message; any process can emit it.
- Lock-screen input blocking: compositor `LockScreenFilter` + `isLockScreenOverlay()` (`src/input.cpp:293`, `src/wayland_server.cpp:464`).
- `KScreenLocker/KsldApp` is linked (`src/CMakeLists.txt:393`) but used only incidentally. It is a library, not a service.
- `src/activities.cpp` uses `KActivities::Controller` → requires the `org.kde.ActivityManager` (kactivitymanagerd) service when activities are in use. **Planned for removal.**

### 2.3 Noctalia v5 (upstream, being forked)

- C++23 / Wayland / OpenGL ES; **no Qt, no GTK, no KDE dependencies**. Deps include sdbus-cpp, pipewire, wireplumber, polkit (own agent), pam, libsecret, libsndfile, tomlplusplus, upower (optional), ddcutil (optional), libqalculate.
- Own surfaces/services: bars, dock, launcher, control center, notification daemon, wallpaper picker, OSDs, **lock screen**, session actions (logind), clipboard history, tray, settings UI, IPC (`noctalia-kwe msg ...`).
- Config: `~/.config/noctalia-kwe/config.toml`, hot-reloaded via inotify. Upstream already ships `[lockscreen]`, `[idle.behavior.lock]` / `[idle.behavior.screen-off]` (timeout + action), `[nightlight]`, `[brightness]` (sysfs backlight + optional ddcutil), `[audio]`, power-profile hooks, and `[hooks]` for `session_locked`/`session_unlocked` etc.
- Compositor backends in `src/compositors/`: niri, hyprland, sway, dwl, labwc, mango, triad, ext_workspace, **kde** (`kwin_workspace_backend.cpp` via the `org_kde_plasma_virtual_desktop` protocol; `kwin_active_window.cpp`).
  - **Gap:** the `kde/` backend has **no output backend** and no session-lock path.
- Existing kineticwe↔noctalia bridge: `src/noctaliasettings.cpp` registers shell shortcuts through the embedded KGlobalAccel and executes `noctalia-kwe msg <args>`; the compositor reads `color-schemes/noctalia.colors` for tiling accent colors (`src/tiling/tilingcontroller.cpp`). In 2.0 the shortcut/settings part of this bridge moves into the Kinetic Settings module (Phase 4); the `noctalia-kwe msg` IPC path stays for compositor-triggered shell actions.

### 2.4 Config file wiring in the compositor

- Main config name is one constant: `KWIN_CONFIG("kwinrc")` in `src/config-kwin.h.cmake` (used via `src/main.cpp:83`).
- Other `KSharedConfig::openConfig` calls to relocate (from `src/`): `kwinrc` (14×), `kaccessrc` (6×), `kcminputrc` (2×), `kwinrulesrc` (1×), `kxkbrc` (1×), `klaunchrc` (1×), `plasmarc` (1×).
- Settings schema lives in `.kcfg` files (`src/kcms/*/*.kcfg`) — these stay as the machine-readable schema; only the file name/group mapping changes.
- Migration scripts live in `kconf_update/` (to be replaced by first-run seeding).

---

## Part 3 — KDE Service Elimination Matrix

| KDE service | Today | 2.0 disposition |
|---|---|---|
| `kglobalacceld` (daemon) | Already embedded (`KGlobalAccelD::init`, `src/globalshortcuts.cpp:97`) | **Keep embedded.** Provides the `org.kde.kglobalaccel` D-Bus API for KDE app compatibility (Spectacle etc.). No daemon process. |
| `xdg-desktop-portal-kde` | External daemon | **Replace** with our own `xdg-desktop-portal-kwe` backend — a rebranded fork of xdg-desktop-portal-kde (v6.7.4) vendored at `portal/xdg-desktop-portal-kwe/` in this repo (Phase 2). |
| `org_kde_powerdevil` + `upowerd` | External daemons | **Replace** with an in-shell power service (Phase 3). upower remains optional for the shell's battery widgets. |
| `kactivitymanagerd` | Needed only by `src/activities.cpp` | **Patch out** activities support in the compositor (Phase 6). |
| `org.kde.kappmenu` | Optional plugin (`src/appmenu.cpp`) | Left dormant; only needed for a global menu. |
| `kbuildsycoca6` | CLI cache builder | Dropped from the session once System Settings is out of the loop; KDE apps degrade to slower lookups only. |
| `kded`, `ksmserver`, `kaccess`, `khotkeys`, `polkit-kde-agent` | Not running today | Not needed (Noctalia ships its own polkit agent). |

**Remaining KDE artifacts (unavoidable — libraries only, not services):** KF6 Frameworks linked into the compositor (KConfig, KGlobalAccel, KWayland, KWindowSystem, KDecoration, ...). Not services; not touched.

---

## Part 4 — Repo & Packaging Layout

- **New fork repo: `kineticwe/noctalia`** — fork of `noctalia-dev/noctalia`. Home of the shell and the power service. Noctalia's own settings app is left untouched so upstream merges stay clean. (Must be its own repo — it is a fork of a different upstream project and cannot live on a branch of the KWin fork.)
- **This repo: `portal/xdg-desktop-portal-kwe/`** — the KineticWE portal backend, vendored here as a rebranded fork of `KDE/xdg-desktop-portal-kde` (tag v6.7.4, squash-imported with `git-subtree-dir`/`git-subtree-split` metadata; upstream remote `portal-upstream` added for future `git subtree pull`). It registers `org.freedesktop.impl.portal.desktop.kwe`, ships `kwe.portal` (`UseIn=KineticWE`) + `kineticwe-portals.conf` (`[preferred] default=kwe`), and keeps every stock interface so the KDE file dialog, Spectacle, and KDE apps keep working — the desktop name and D-Bus registration are the only user-visible identity.
- **Iteration 1 — Kinetic Settings lives inside the Noctalia fork** as a self-contained, additive-only module (a `kinetic-settings/` subtree): display, keybinds/shortcuts, icons, mouse & pointer, cursor, touchpad, keyboard layouts, fonts, effects, tiling & window rules, workspaces, tabbox & screen edges. It opens a full window the same way Noctalia's own settings surface does — no separate binary; it ships a `.desktop` entry (`dev.kineticwe.Settings.desktop`, installed to `applications/` with the `kineticwe-logo` icon) that wraps `noctalia-kwe msg kinetic-settings-open`, so it is launchable from the launcher like any app. It is reached from the shell (bar/control-center button, `noctalia-kwe msg`, or the launcher entry), reuses the shell's window/chrome infrastructure, and writes `~/.config/kineticwe/` directly. It stays one new subtree + registration points and never touches Noctalia's own settings code, so upstream merges stay clean.
- **Iteration 2 (deferred): `kineticwe/kinetic-settings`** — extract the module into a standalone app repo with its own binary and `.desktop` entry when launcher-app presence, native performance, or independent packaging is wanted. Keeping the module self-contained from day one makes extraction a cut-and-paste job.
- **This repo: new branch `kineticwe-2.0`** — compositor-only changes (config relocation, activities removal, session identity, optional ext-session-lock-v1). `master` stays untouched so current users are unaffected. COPR/AUR/Nix build from the branch when ready.
- **`docs/kineticwe-2.0.md`** — this document.
- Packaging: session packages now depend on the `noctalia` fork instead of `noctalia-git`; spec/PKGBUILD/`.SRCINFO`/nix inputs updated accordingly; `kglobalacceld`/`powerdevil`/portal-kde runtime coupling removed where applicable. Iteration 1 adds no extra package — Kinetic Settings ships inside the fork; a separate `kinetic-settings` package only appears with the standalone app in iteration 2.
- Portal packaging: RPM splits a `xdg-desktop-portal-kwe` subpackage (built as %build Phase 4 from `portal/xdg-desktop-portal-kwe`); the main package `Requires: xdg-desktop-portal-kwe` (replacing `xdg-desktop-portal-kde`); new BuildRequires `kf6-kiconthemes-devel` / `kf6-kitemviews-devel` / `kf6-kstatusnotifieritem-devel` / `kf6-kio-filewidgets-devel`. AUR PKGBUILD installs the backend into the main package, `provides xdg-desktop-portal-kwe`, `conflicts xdg-desktop-portal-kde`. Nix exposes `xdg-desktop-portal-kwe` via `nix/portal-kwe.nix` (flake attr `.#xdg-desktop-portal-kwe`); the 1.x `nix/session.nix` session keeps the KDE backend until the 2.0 session cut (Phase 5), since its identity is still `KDE`.
- Window decoration: the only window decoration is the first-party `org.kineticwe.decoration` plugin (`src/plugins/kineticdecoration`), a KDecoration3 C++ plugin compiled into the `kineticwe` package and the compiled-in default (`kineticwe`/`dark`). Its colors come from the Noctalia palette or the shared color scheme, so the titlebar recolors with the theme. Kinetic Settings → Title bars offers its four variants — **Kinetic Standard Dark** (the default), **Kinetic Standard Light**, **Kinetic Dots Dark** and **Kinetic Dots Light** — so fresh installs have real titlebar choices with nothing to download. The Aurorae decoration engine (previously built from a pinned KDE commit), its vendored Layan themes and the Plastik QML decoration it shipped are no longer built, packaged or installed; no external window-decoration engine is required.
- Default palette: KineticWE points Noctalia's `[theme] builtin` at **Origami** (`shell/noctalia/src/theme/builtin_palettes.cpp`, default in `config_types.h`) instead of upstream's Noctalia palette. It is derived from RakuOS's *Origami Paper* window scheme (`org.rakuos.OrigamiPaper`, a Nord derivative: `#101216` surfaces, `#272a2e` variants, `#81a1c1` frost-blue accent, `#bf616a` error, `#a3be8c` positive), so fresh installs match that look out of the box. Everything downstream is generated from it — the KDE color scheme (hence Breeze titlebar colors), Qt5/Qt6 color files, GTK, terminals, Firefox — and the palette stays selectable in Noctalia's Appearance settings alongside the upstream palettes.
- Theming dependencies: include **`qt5ct`** (and `qt6ct`) and **`adw-gtk3-dark`** (with `adw-gtk3` for the light theme) as runtime deps. Qt apps are themed the traditional-WM way (like Hyprland/Niri): the session exports `QT_QPA_PLATFORMTHEME=qt5ct`, and the Appearance settings page writes the Noctalia color scheme to `~/.config/qt5ct/colors/noctalia.conf` + `~/.config/qt6ct/colors/noctalia.conf` and points each tool's config at it. GTK/libadwaita apps pick up colors via Noctalia's GTK templates (which reference the adw themes). kdeglobals is also updated for KDE Framework apps. All are standalone runtime deps — no KDE/Plasma package. On Apply, live changes are pushed to running apps: `org.kde.KGlobalSettings.notifyChange(0,0)` (Qt/KDE/qt5ct) and `gsettings`/`dconf` `color-scheme` (GTK).

---

## Part 5 — Noctalia Fork: Plugins, Templates & Upstream Sync

### 5.1 Plugins — compatible, unchanged

The plugin system is **shell code**; forking does not touch it:

- Plugin locations are user-side and runtime-loaded: `~/.local/share/noctalia-kwe/plugins/` (manual/path sources) and git sources cached in `~/.local/state/noctalia-kwe/plugins/sources/`.
- `plugin.toml` manifests, Luau entry scripts (isolated VMs, off the UI thread), hot-reload, the `noctalia-kwe msg plugins ...` IPC, and the Settings → Plugins UI all ship inside the fork.
- Plugin types (widgets, launcher providers, panels, shortcuts, background services) and their settings surfaces are preserved.

**Compatibility contract (must never break):**

- IPC command set (`noctalia-kwe msg ...`) — additive only.
- Runtime API / plugin API versions (`noctalia.*` host capabilities) — keep the capability set we fork at; bump versioned layers cleanly.
- TOML schema — additive only.

**Caveats:**

- Compositor-specific plugins remain compositor-specific (a hyprland-IPC plugin won't run on kinetic-we — true today, not a fork regression). Generic plugins gain the new KDE output backend for free (additive).
- Own paths, side by side with upstream: the fork uses its own `noctalia-kwe` install paths (`/usr/share/noctalia-kwe/`), desktop ID `dev.kineticwe.Noctalia`, icon (`noctalia-kwe.svg`) and XDG dirs (`~/.config/noctalia-kwe/`, `~/.local/state/noctalia-kwe/`, `~/.local/share/noctalia-kwe/`, `~/.cache/noctalia-kwe/`), so it installs and runs side by side with upstream Noctalia. These names come from upstream's `ApplicationIdentity` (`lib/core/application_identity.h`, set in `src/app/shell_identity.h`): the fork sets `name = "noctalia-kwe"` and keeps the `NOCTALIA` environment prefix. Upstream Noctalia's dirs are not read or imported; the fork starts from its own defaults. `NOCTALIA_CONFIG_HOME`/`NOCTALIA_STATE_HOME`/`NOCTALIA_DATA_HOME` overrides still apply and resolve to `<override>/noctalia-kwe`.

### 5.2 Community templates & palettes — compatible

Templates/palettes are **network catalogs**, not code:

- Fetched from `api.noctalia.dev`, cached in `~/.local/state/noctalia-kwe/community-*/`, re-fetchable; built-in and user-defined templates are declared in config (`[theme.templates]`).
- Template post actions (`kde-color-scheme`, `firefox-theme`, etc.) only write files — no KDE service or compositor dependency.
- The `kde-color-scheme` template is an existing shell→compositor link we rely on: it writes `color-schemes/noctalia.colors`, which the compositor reads for tiling accent colors (`src/tiling/tilingcontroller.cpp`).

**Risk:** the registry is a third-party network service. If `api.noctalia.dev` changes or disappears, community templates/palettes break. Mitigation: self-host the catalog if full independence is wanted (no change to the template mechanism itself). Not a blocker for 2.0.

### 5.3 Upstream sync workflow

The fork keeps `noctalia-dev/noctalia` as an upstream remote — updates can be fetched and merged at any time. Merge health is protected by:

1. **Additive-only fork policy** — new files and extensions to the existing `kde/` backend, never rewrites of core; keeps the diff surface tiny and conflicts rare.
2. **Pin to the v5 lineage** — track a tag/branch, not `main`; a future upstream v6 is a deliberate migration decision, not something absorbed accidentally.
3. **`sync-upstream` branch + cadence** — merge upstream periodically (weekly/monthly) into a dedicated branch, resolve conflicts there, then merge into the dev branch.
4. **CI conflict check** — a job runs `git merge --no-commit upstream/main` (dry run) and flags conflicts so drift is caught early.
5. **Contribute back** — anything generic should be upstreamed (e.g., extending upstream's `kde/` backend with output support), shrinking our delta toward "sync, not merge."

**Risk:** merge quality degrades with local patches to core files; the additive policy is the mitigation. Major upstream version jumps are the one forced big merge — controlled by our pinning decision.

---

## Part 6 — Unified Config: `~/.config/kineticwe/`

### 5.1 Goal

All desktop-environment settings live in one folder with sane defaults. No scattered `*rc` files.

### 5.2 Layout

Compositor-owned settings live in `~/.config/kineticwe`. The session script (`scripts/start-kineticwe.sh`, step 1b) redirects `XDG_CONFIG_HOME` **only for the compositor process** (per-command env prefix), so the compositor natively resolves `kineticwe.kwe` / `rules.kwe` / `kglobalaccelrc` / `kwinoutputconfig.json` inside that root — no compositor source changes for those, and every `KConfigWatcher` hot-reload path keeps working. The shell, kinetic-settings, D-Bus-activated services, and **all user applications keep the real `~/.config`** — user apps never have to duplicate or relocate their dotfiles. The script publishes the real config home to the compositor as `KWE_REAL_CONFIG_HOME` so shared state (`kdeglobals`) stays a single source of truth.
The script also redirects `XDG_CACHE_HOME` to `~/.cache/kineticwe` for the compositor process so its KSycoca service cache lives in a private file — without this, a process with a different `XDG_CONFIG_HOME` that shares the same cache file (the filename hash covers `XDG_DATA_DIRS` but not `XDG_CONFIG_HOME`) would continuously invalidate the other's database (different `mimeapps.list` tracking), causing an infinite ksycoca rebuild ping-pong at ~2 s/cycle.

| File | Replaces | Contents |
|---|---|---|
| `~/.config/kineticwe/kineticwe.kwe` | `kwinrc` | All compositor settings: `[Shortcuts]`, `[Mouse]`, `[Libinput]`, `[Keyboard]`, `[TabBox]`, `[ScreenEdges]`, `[Plugins]`, `[Effect-*]`, `[Desktop]`, `[Tiling]`, `[Xwayland]`, ... |
| `~/.config/kineticwe/rules.kwe` | `kwinrulesrc` | Window rules (numbered rule groups + force/apply/forbid keys) |
| `~/.config/kineticwe/kglobalaccelrc` | `~/.config/kglobalaccelrc` | App global-shortcut storage; filename hardcoded in the KGlobalAccel library, resolved under the compositor's redirected config root. Migrated by the session script. |
| `~/.config/kineticwe/kglobalshortcutsrc` | `~/.config/kglobalshortcutsrc` | Shortcut storage of the embedded KGlobalAccelD (runs inside the compositor process, so its state follows the compositor's config root). |
| `~/.config/kineticwe/keybinds.kwe` | new in 2.0 | Portable keybinds mirror: every overridden chord of the KineticWE-owned components (`kwin`, `noctalia`, `audio`, `media-ui`), Spectacle's actions, and kinetic-settings' `applications` / `custom-commands`. Other Plasma-service and per-application components are not surfaced or mirrored. Written on Apply, replayed into the daemon at session start by `KeybindsKwe::applyToDaemon`. Never copied over `kglobalshortcutsrc` — the daemon's own loader rejects it (3-part format). |
| `~/.config/kineticwe/custom-keybinds.kwe` | new in 2.0 | Kinetic-settings register of the user-added Applications / Custom Commands actions (component + action + exec). Survives a login only if the config setup helper's `KEEP` list (`scripts/kineticwe-config-setup`) includes it; without it, the shell loses the user's Application / Custom Command keybinds on every login. |
| `~/.config/kineticwe/appearance.kwe` | new in 2.0 | Portable Appearance settings (Kinetic Settings -> Icons / Cursors). Holds `[Appearance] IconTheme`, `CursorTheme` and `CursorSize` (seeded on first run by the session script); the choices travel with the rest of `~/.config/kineticwe` when copied to a new machine. Theme **colors** are not stored here: the shell's theme service owns the generated palette (qt5ct/qt6ct color schemes, `~/.config/kdeglobals`, the GTK dark/light preference). |
| `~/.config/kineticwe/kwinoutputconfig.json` | `kwinoutputconfig.json` | Compositor output layout/scale state (regenerated on change). |
| `~/.config/kineticwe/session/` | `~/.config/session/` | Compositor session-restore state. |
| `~/.config/kineticwe/kinetic-settings.kwe` | new in 2.0 | Kinetic Settings UI state that must survive a restart: `[PointingDevices] Mouse=<sysName>` / `Touchpad=<sysName>`, the device last selected on the Mouse / Touchpad pages. Written when the picker changes; kept by the config setup helper's `KEEP` list. |
| `~/.config/kdeglobals` | (unchanged) | **Shared** theme state. One copy, in the user's real config home: the shell's theme service writes it, KDE apps read it, and the compositor opens it by absolute path (`kineticweKdeGlobalsPath()`, `src/config-kwin.h.cmake`) via `KWE_REAL_CONFIG_HOME`. No mirror. The session's config setup helper (`scripts/kineticwe-config-setup`, step 4e) seeds **Breeze Dark** here when no color scheme is configured so the default window decoration and KDE apps are dark out of the box. |
| `~/.config/noctalia-kwe/config.toml` | (unchanged) | Shell-owned UI: bars, dock, launcher, notifications, wallpaper, **lock screen**, idle actions, brightness, nightlight UI, power-profile hooks. Already hot-reloaded. Shell text size is `[accessibility] font_scale` (upstream replaced the fork's old `[shell] font_size`). |

The session's config setup helper (`scripts/kineticwe-config-setup`, run by `start-kineticwe.sh` before the compositor starts) also **prunes stale user-app entries** from the config root on startup (step 4d): earlier builds redirected the whole session's `XDG_CONFIG_HOME`, so user applications (yazi, zed, fish, kitty, ...) wrote configs inside the root; only compositor-owned files are kept now. Pruned entries are **moved** to `~/.local/state/kineticwe/pruned/<timestamp>/` (newest 5 kept), never deleted, and files the helper changes are backed up to `~/.local/state/kineticwe/backups/<timestamp>/` first. The real configs in `~/.config` are untouched duplicates.

### 5.3 Syntax — KConfig INI

Both `.kwe` files use the KConfig INI syntax the compositor reads natively. No new parser; the settings schema stays in the `.kcfg` files. Example:

```ini
# ~/.config/kineticwe/kineticwe.kwe
[Windows]
Placement=Centered
BorderSnapZone=10
WindowSnapZone=10

[Shortcuts]
Switch Desktop=Meta+1
Walk Through Windows=Alt+Tab; Meta+Tab

[Mouse]
cursorTheme=Breeze
cursorSize=24

[Libinput]
TapToClick=true
NaturalScroll=false

[TabBox]
layoutName=compact
```

```ini
# ~/.config/kineticwe/rules.kwe
[1:wmclass=spotify]
placement=Force
placementrule=2

[2:wmclass=firefox]
maximize=Force
maximizerule=2

[General]
count=2
rules=1,2
```

Notes:

- Shortcut lists use `;` as the separator; some enum values are stored as numbers. Users never see these quirks — Kinetic Settings is the editor.
- The `.kwe` extension is unknown to the KDE ecosystem, so nothing else auto-discovers or clobbers these files — intentional isolation.
- kcfg codegen, `KConfigWatcher` hot-reload, and migrations all keep working unchanged.

### 5.4 Defaults & first-run seeding

- The package ships `defaults/kineticwe.kwe` + `defaults/rules.kwe` (documented, sane values; extends the existing `KineticWE Default Binds.kksrc` pattern).
- The session script seeds `~/.config/kineticwe/` on first run if missing (copy + backup existing legacy `kwinrc`/`kwinrulesrc` if present, migrate known keys).
- `kconf_update/` scripts are retired in favor of this seeding.
- One-shot migration (`scripts/kineticwe-config-setup` step 4d.5) clears every Spectacle global-shortcut default (`_launch`, `FullScreenScreenShot`, `CurrentMonitorScreenShot`, `ActiveWindowScreenShot`, `RectangularRegionScreenShot`, `WindowUnderCursorScreenShot`, `OpenWithoutScreenshot`, `RecordRegion`, `RecordScreen`, `RecordWindow`) in the live `kglobalaccelrc` on existing installs. The .kksrc clears them in fresh installs, but on an existing machine the defaults were already written by the Spectacle package before the .kwe override could land. The migration is idempotent: it leaves a `[Migration] SpectacleRecordingUnbound=kineticwe-2.1` sentinel in the file and never re-runs at the same version. Bump the sentinel version to force a re-run. If `spectacle` is not on `PATH` the migration writes only the sentinel (no stray empty Spectacle group in users' `kglobalaccelrc`).
- Default color scheme (`scripts/kineticwe-config-setup` step 4e): until the Kinetic Settings decorations/theming section lands, the config setup helper makes **Breeze Dark** the default by merging `BreezeDark.colors` (located via `XDG_DATA_DIRS` / standard share dirs; shipped by the `breeze` dependency) into the shared `kdeglobals`. It only applies when no color scheme is configured yet (no `[Colors:*]` groups and no non-empty `ColorScheme=` entry), so it seeds new users, migrates existing users still on the bare light default, and never overwrites a scheme the user or the Noctalia theme service chose. The gate makes the step idempotent — once applied, later logins skip it; no sentinel needed. The merge preserves every non-color setting already in the user's `kdeglobals` and writes atomically (tmp + `os.replace`), mirroring Noctalia's `mergeKdeColorScheme` semantics.

### 5.5 Hot reload

- The compositor reloads config on change: per-subsystem `KConfigWatcher`s (effects, screen edges, tiling, keyboard layouts, night light, accessibility plugins) plus a general watcher on `kineticwe.kwe` and one on `rules.kwe` that nudge the debounced full reconfigure (`slotReconfigure`). Editing the files applies live within ~200 ms.
- Kinetic Settings writes the `.kwe` files and triggers a reparse via the compositor's D-Bus where a setting is runtime-only (e.g., cursor: emit the `org.kde.KGlobalSettings` `notifyChange` type 5 signal — `src/cursor.cpp` applies it live).

---

## Part 7 — Work Phases (dev)

### Phase 1 — Noctalia fork: compositor backend gaps

1. **Output backend for KDE** (missing upstream): implement a `kde` output backend consuming `kde-output-device-v2` (or `org.kde.KWin` outputs D-Bus via sdbus-cpp) so the shell sees res/refresh/scale/position/rotation/brightness and can change them at runtime.
2. **Lock screen path**: keep the current flow (`kde_lockscreen_overlay_v1` + compositor `LockScreenFilter`) — works today, Noctalia owns the UI and `[lockscreen]` config. Optional stretch: add `ext-session-lock-v1` to the compositor (upstream KWin has it) and use Noctalia's standard session-lock path.
3. Keep/verify `kwin_workspace_backend` (already uses `org_kde_plasma_virtual_desktop`).
4. Ensure the shell behaves correctly with `XDG_CURRENT_DESKTOP=KineticWE` (must not assume Plasma).

### Phase 2 — Replace `xdg-desktop-portal-kde` with `xdg-desktop-portal-kwe` (fork, not rewrite)

**Status: DONE (vendored fork at `portal/xdg-desktop-portal-kwe/`, rebrand + packaging complete; screen sharing verified live in the 2.0 session).**

Approach: fork `KDE/xdg-desktop-portal-kde` (tag v6.7.4) and rebrand rather than write a from-scratch backend. This keeps every interface working unchanged — the KDE file chooser (KFileWidget in-process), Spectacle, KDE apps — while the session still exports as KineticWE. A from-scratch backend would have to reimplement FileChooser, ScreenCast, GlobalShortcuts and more, and would break Dolphin/Spectacle dialogs.

Rebrand surface (deliberately minimal — only user-visible identity changes):

- D-Bus service name: `org.freedesktop.impl.portal.desktop.kwe` (`src/xdg-desktop-portal-kwe.cpp`)
- `.portal` file: `kwe.portal` with `DBusName=org.freedesktop.impl.portal.desktop.kwe`, `UseIn=KineticWE`
- Frontend selection: `kineticwe-portals.conf` with `[preferred] default=kwe` (mirrors `kde-portals.conf`; installed to `/usr/share/xdg-desktop-portal/`)
- Binary `xdg-desktop-portal-kwe`, D-Bus-activatable service (plain activation — the `SystemdService=` line was dropped from the template so activation works in every flow), user unit `plasma-xdg-desktop-portal-kwe.service` still installed, desktop entry `org.freedesktop.impl.portal.desktop.kwe.desktop`, notifyrc, screen-grabs dir
- Intentionally kept: `TRANSLATION_DOMAIN=xdg-desktop-portal-kde` (po/ translations load), all `XdgDesktopPortalKde*` log identifiers (`xdp-kde-*`), QML URI `org.kde.xdgdesktopportal`, `Icon=kde` in the desktop entry.

Interface mapping (all stock — verified against this compositor fork, KWin 6.7.80):

- **Screenshot** → `org.kde.KWin.ScreenShot2` ✅ (exists)
- **PickColor** → `org.kde.kwin.ColorPicker` ✅ (exists)
- **GlobalShortcuts** → `org.kde.kglobalaccel` ✅ (embedded in-process, `src/globalshortcuts.cpp:97`)
- **Notification** → `org.freedesktop.Notifications` ✅ (owned by Noctalia)
- **Inhibit / IdleInhibit** → in-process; `org.freedesktop.ScreenSaver` is compositor-owned ✅
- **Settings** → reads `kdeglobals` ✅
- **FileChooser** → in-process KFileWidget (kio) ✅
- **ScreenCast** → `zkde_screencast_unstable_v1` Wayland protocol ✅ (the portal's `screencasting.cpp` binds this protocol — no D-Bus interface involved; implemented in this compositor fork at `src/wayland/screencast_v1.cpp`, registered in `src/wayland_server.cpp:132`. Verified end-to-end with OBS.)
- **PowerProfileMonitor** → `power-profiles-daemon` D-Bus (not KDE)

Drop-in: the session script starts the freedesktop `xdg-desktop-portal` frontend (`systemctl --user start xdg-desktop-portal.service` after the activation-environment update); the frontend auto-selects the `kwe` backend by desktop name (`XDG_CURRENT_DESKTOP=KineticWE` → `kineticwe-portals.conf` → `[preferred] default=kwe`). The KDE backend is never selected in a KineticWE session.

Screen sharing (was "Step 2"): the portal uses the `zkde_screencast_unstable_v1` Wayland protocol, which this compositor already implements — no `org.kde.KWin.ScreenCast` D-Bus interface was ever missing. The actual blockers found live: (a) the kwe backend only registered ScreenCast/Screenshot/etc. when `XDG_CURRENT_DESKTOP=KDE` — hardcoded gate in `desktopportal.cpp:47`, now also accepting KineticWE; (b) a stale `xdg-desktop-portal` frontend holding a dead backend connection (restart the frontend after swapping the backend binary). Both fixed; OBS screen sharing verified end-to-end.

### Phase 3 — Replace PowerDevil with an in-shell power service

New Noctalia background service ("power manager" module — Noctalia supports background service plugins):

- **Idle timeout + screen off** → compose from Noctalia's existing `[idle.behavior.*]` (upstream already ships lock/screen-off actions) + compositor idle/DPMS
- **Suspend / hibernate / reboot / shutdown** → logind D-Bus (Noctalia already performs session actions via logind)
- **Brightness** → backlight sysfs (already in the shell's brightness widget) + optional ddcutil
- **Power profiles** → `power-profiles-daemon` D-Bus
- **Lid close / AC/battery events** → logind + optional upower signals
- **Night light** → compositor `nightlight` plugin, configured by a settings module

Remove `org_kde_powerdevil` / `upowerd` from the session script (keep upower only if the shell's battery widget needs it).

### Phase 4 — Kinetic Settings module: desktop modules

Each module = write `.kwe`/config + call compositor-owned D-Bus + emit change signals. The compositor's KCM sources in `src/kcms/` are the reference for exact key names and semantics (`options`, `tiling`, `effects`, `mouse`, `rules`, `screenedges`, `tabbox`, `virtualkeyboard`, `desktop`, `decoration`, `animations`, `xwayland`). Iteration 1 ships this as a self-contained `kinetic-settings` module inside the Noctalia fork (`kinetic-settings/` subtree) opening a full window like the shell's settings surface — it never touches Noctalia's own settings code, so upstream merges stay clean.

| Module | Config target | Runtime apply mechanism |
|---|---|---|
| Display (res/refresh/rotation/arrangement/scale/brightness) | kde-output-device-v2 management (Phase 1 backend) | compositor applies immediately |
| Shortcuts (compositor + global app) | `kineticwe.kwe` `[Shortcuts]`; `kglobalaccelrc` (component `noctalia`/`kwin`) | config reload + embedded kglobalaccel re-read |
| Cursor | `kineticwe.kwe` `[Mouse]` + `org.kde.KGlobalSettings` notifyChange(5) | runtime (`src/cursor.cpp:140`) |
| Icons | `kineticwe.kwe` + real `~/.config/kdeglobals` (shared, one copy) | app-level; shell own |
| Mouse/Touchpad | `kineticwe.kwe` `[Libinput]` | compositor reparse; `InputDevice` D-Bus for live tweaks |
| Keyboard layouts | `kineticwe.kwe` `[Keyboard]` + `org.kde.KWin` Layouts D-Bus | immediate + config |
| Fonts | `~/.config/fontconfig/conf.d/99-kineticwe.conf` | fontconfig (non-KDE) |
| Lock screen | Noctalia TOML `[lockscreen]`, `[idle.behavior.lock]` | shell-owned |
| Effects / Animations | `kineticwe.kwe` `[Plugins]` / `[Effect-*]` | `org.kde.KWin.Plugins` D-Bus |
| Tiling / Window rules | `kineticwe.kwe` tiling groups; `rules.kwe` | config reload |
| Workspaces / Virtual desktops | `kineticwe.kwe` `[Desktop]` + `VirtualDesktopManager` D-Bus | immediate |
| Tabbox / Screen edges / OSD | `kineticwe.kwe` `[TabBox]` / `[ScreenEdges]` | config reload |
| Autostart | XDG autostart dirs (see `docs/autostart-implementation.md`) | shell autostart supervisor |
| Sounds | Noctalia TOML `[audio]` (libsndfile) | shell-owned |
| XWayland | `kineticwe.kwe` xwayland group | restart only (documented) |

Once modules land, fold the existing `src/noctaliasettings.cpp` shortcut bridge into the Kinetic Settings module (keep the `noctalia-kwe msg` IPC for compositor-triggered shell actions).

### Phase 5 — Session identity & start script (hard cut)

- `XDG_CURRENT_DESKTOP=KineticWE`, `XDG_SESSION_DESKTOP=KineticWE`, matching `.portal` file so the freedesktop portal frontend loads `xdg-desktop-portal-kwe` and can never pick a KDE one.
- Remove `KDE_SESSION_VERSION`, `KDE_FULL_SESSION`, `XDG_MENU_PREFIX=plasma-` exports; audit fallout (kbuildsycoca menus, `kde-open`, desktop-file identity).
- Provide own `xdg-open` behavior via the OpenURI portal in the shell.
- Rewrite `scripts/start-kineticwe.sh`: drop portal-kde / powerdevil / upowerd / kbuildsycoca; keep the dbus activation-environment update; start the freedesktop portal frontend (`xdg-desktop-portal.service`), which D-Bus-activates the `xdg-desktop-portal-kwe` backend by desktop name; start the Noctalia fork (power service + the Kinetic Settings module); supervise the shell (now the `kineticwe-shell.service` user unit, with `launch_apps_as_systemd_services` on so apps get their own units) and keep the logind teardown model; add first-run config seeding (Part 6.4).
- Update COPR / AUR / spec / nix packaging for the new desktop identity and the fork dependency.
- **Finish the `kwin-we` → `kinetic-we` re-brand** (the main compositor binary is already `kinetic-we`; AUR `pkgname` and RPM `Name` are already `kineticwe` — what's left is leftover naming):
  - Rename the last remaining old-named binary, `kwin-we_wayland_wrapper` → `kinetic-we_wayland_wrapper`: CMake target + logging category in `src/helpers/wayland_wrapper/`, `%files` + symlink sources in `kwin-we.spec` and `lgl-kineticwe-copr/kineticwe.spec`, symlink source in `PKGBUILD`, cleanup paths in `build-rpm-dev.sh`. Keep the KDE-compat `kwin_wayland` / `kwin_wayland_wrapper` symlinks — upstream code and Plasma tooling reference them.
  - Clean stale `kwin-we` labels in comments/docs/scripts (`start-kineticwe.sh`, `nix/`, `README.md`, `PACKAGING-COPR.md`, `PKGBUILD` comments).
  - Package-identity rename — breaking, coordinate at the 2.0 branch cut: nix `pname` and flake/overlay attrs `kwin-we` → `kinetic-we`, rename `nix/kwin-we.nix` → `nix/kinetic-we.nix` and `kwin-we.spec` → `kineticwe.spec`.

### Phase 6 — Cleanup & optional work

1. Patch out `src/activities.cpp` (kactivitymanagerd dependency); drop the `PlasmaActivities` build dep.
2. Leave `org.kde.kappmenu` optional/dormant.
3. Stretch: add `ext-session-lock-v1` to the compositor for standards-based locking.
4. Stretch: evaluate replacing the `kglobalaccelrc` storage with shell-owned config while keeping the `org.kde.kglobalaccel` D-Bus API for app compatibility.
5. Testing: extend `autotests/`; `ci/smoke.sh` asserts the packaged portal backend is present and KDE-free (backend binary `xdg-desktop-portal-kwe` shipped, `kwe.portal` contains `UseIn=KineticWE`, `kineticwe-portals.conf` present, no `xdg-desktop-portal-kde` files in the package). Live-session checks (no `xdg-desktop-portal-kde` process, `busctl --user list | grep impl.portal` shows the kwe backend, screen share works) go in the session test script; the power module responsive, the Kinetic Settings full-window UI opens from the shell and from the launcher (via the `dev.kineticwe.Settings.desktop` entry, `noctalia-kwe msg kinetic-settings-open`), tiling changes persist to `~/.config/kineticwe/` and apply live, `~/.config/kineticwe/` seeded.

---

## Part 8 — Constraints & Not-Possible

1. `org.kde.kglobalaccel` D-Bus API remains (embedded) — required by KDE apps for global shortcuts.
2. KDE Frameworks libraries remain in the compositor — removing them is a from-scratch rewrite, out of scope.
3. The `xdg-desktop-portal` frontend stays (freedesktop standard); only the KDE *backend* is removed (replaced by our `xdg-desktop-portal-kwe`).
4. Activities (kactivitymanagerd) and KDE global-menu (kappmenu) lose their backing services — accepted drops.
5. KWallet: the shell uses libsecret; the secret-service provider becomes GNOME Keyring or KeePassXC (neither is KDE). KWallet remains an optional install.
6. Electron/X11 apps: unchanged behavior (XWayland + portals).
7. Accessibility (Orca/screen readers) uses AT-SPI (freedesktop) — unaffected; the compositor already registers `org.freedesktop.a11y.Manager`.

---

## Part 9 — Decisions Log & Open Questions

### Decided

| Decision | Choice |
|---|---|
| Config location | `~/.config/kineticwe/kineticwe.kwe` + `rules.kwe` (+ seeded `defaults/`) |
| Config syntax | KConfig INI (native; no new parser) |
| Lock screen | Noctalia's own (its TOML `[lockscreen]` / `[idle.behavior.lock]`); no compositor lock config needed |
| Shell identity | Hard cut to `XDG_CURRENT_DESKTOP=KineticWE` |
| Compositor binary name | Already `kinetic-we`; leftover `kwin-we_wayland_wrapper` wrapper + nix/package identity renamed in Phase 5 (`kwin_wayland*` KDE-compat symlinks kept) |
| Compositor repo strategy | New `kineticwe-2.0` branch in this repo; `master` untouched |
| Shell repo strategy | Iteration 1: Noctalia vendored in this repo as a `shell/noctalia` git subtree (upstream-pullable); standalone `kineticwe/noctalia` fork repo deferred — split back out via `git subtree split` if wanted |
| kglobalacceld | Keep embedded (library-level service inside the compositor) |
| Portal backend | Our own `xdg-desktop-portal-kwe` backend — rebranded fork of xdg-desktop-portal-kde (v6.7.4), vendored at `portal/xdg-desktop-portal-kwe/` in this repo |
| Power management | In-shell power service; PowerDevil/upower removed from session |
| Settings app | Iteration 1: in-fork `kinetic-settings` module (additive subtree, full-window UI like the shell's settings, no separate binary; ships a `dev.kineticwe.Settings.desktop` launcher entry wrapping the module's IPC command); Iteration 2: extract to standalone `kineticwe/kinetic-settings` app |
| Activities | Patched out of the compositor |
| Docs | `docs/kineticwe-2.0.md` (this file) |
| Plugins | Keep upstream plugin system; IPC / runtime-API / TOML additive-only |
| Templates | Keep community catalogs (`api.noctalia.dev`); self-host only if independence is wanted |
| Upstream sync | Track v5 lineage; `sync-upstream` merge cadence + CI conflict check; contribute back generic work |

### Still open

1. Lock screen implementation detail: keep `kde_lockscreen_overlay_v1` (fast, works now) or add `ext-session-lock-v1` to the compositor (standard, more work)? — Default: overlay first, ext-session-lock as stretch.
2. Transition timing for COPR/AUR/Nix branches — to be coordinated when Phase 5 lands.
3. Config/plugin path separation: point the fork at its own `NOCTALIA_*_HOME` dirs for side-by-side install with upstream Noctalia, or share paths? — Default: share, revisit if conflicts arise.
4. Community template registry: self-host the catalog, or keep using `api.noctalia.dev`? — Default: keep upstream API for 2.0.
5. Upstream sync cadence — weekly or monthly? — Default: monthly + CI conflict check.

---

## Appendix — Config file mapping

### Where do my configs go?

- **Compositor settings** live in `~/.config/kineticwe/`: `kineticwe.kwe`, `rules.kwe`, `keybinds.kwe`, `custom-keybinds.kwe`, `appearance.kwe`, `kinetic-settings.kwe`, `kglobalaccelrc`, `kglobalshortcutsrc`, `kineticweoutputconfig.json`, `kactivitymanagerdrc`, `session`, plus a `kdeglobals` symlink to the shared file.
- **Everything else stays in your normal `~/.config`**: app configs (kitty, alacritty, fish, ...), `kdeglobals`, `qt5ct/` / `qt6ct/`, `fontconfig/`, and the shell's `noctalia-kwe/`. Apps in a KineticWE session read and write `~/.config` exactly as anywhere else.
- **Never symlink or copy app configs into `~/.config/kineticwe/`.** The login setup moves anything it doesn't own out of that folder to `~/.local/state/kineticwe/pruned/<timestamp>/`.
- **Logs and backups:** the login setup logs to `~/.local/state/kineticwe/config-setup.log` and backs up files before changing them in `~/.local/state/kineticwe/backups/`. The shell logs to the journal: `journalctl --user -u kineticwe-shell`.

| Legacy file | 2.0 location | Owner |
|---|---|---|
| `~/.config/kwinrc` | `~/.config/kineticwe/kineticwe.kwe` (compositor-only `XDG_CONFIG_HOME` redirect) | compositor / Kinetic Settings |
| `~/.config/kwinrulesrc` | `~/.config/kineticwe/rules.kwe` (compositor-only `XDG_CONFIG_HOME` redirect) | compositor / Kinetic Settings |
| `~/.config/kaccessrc`, `kcminputrc`, `kxkbrc`, `klaunchrc`, `plasmarc` (kwin-read bits) | merged into `kineticwe.kwe` | compositor / Kinetic Settings |
| `~/.config/kglobalaccelrc` | `~/.config/kineticwe/kglobalaccelrc` (migrated by session script) | embedded KGlobalAccelD (in compositor) |
| `~/.config/kglobalshortcutsrc` | `~/.config/kineticwe/kglobalshortcutsrc` | embedded KGlobalAccelD (in compositor) |
| `~/.config/kdeglobals` | unchanged — one shared copy in the real config home; the compositor opens it via `KWE_REAL_CONFIG_HOME` (`kineticweKdeGlobalsPath()`) | shell theme service / KDE apps / compositor |
| `~/.config/noctalia-kwe/config.toml` | unchanged | shell |
| `~/.config/fontconfig/fonts.conf` | unchanged | fontconfig / user |
| `~/.config/fontconfig/conf.d/99-kineticwe.conf` | new managed file for generic font preferences | Kinetic Settings |

User applications (yazi, zed, kitty, ...) are **not** affected: they keep reading/writing `~/.config` exactly as outside the KineticWE session.
