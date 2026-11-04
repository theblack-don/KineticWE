# Installing a dev branch from the Nix flake (Fedora + dcli)

This is a development install: it builds your checked-out branch through the
repository's `flake.nix` and installs it with dcli instead of an RPM. It is
meant for working on KineticWE itself, not for daily use.

For the packaged install see the Fedora/COPR section in the
[README](../README.md).

## What you need

- A Fedora system with `nix` (daemon + flakes) — `dcli nix install` sets this up.
- dcli in standalone `nix profile` mode (`nix_profile_enabled = true`,
  `home_manager_enabled = false`). `flake_enabled` is Home-Manager-only and can
  stay `false`.
- `module_processing = "sequential"` in the dcli host file. Sequential mode runs
  module hooks with `run_hooks_as_user`; the parallel path always runs hooks
  through `sudo`, which would break a per-user `nix profile`.
- The KineticWE checkout. This guide assumes
  `/mnt/portablehome/Projects/kineticwe`; override the path with `KWE_REPO`.
  The hook builds whatever is checked out (see
  [Flake source semantics](#flake-source-semantics)); set `KWE_REF` only to
  build a clean committed ref instead.

## dcli module

`~/.config/dcli/modules/kineticwe-dev/` contains:

- `module.nix` — enables the hook and re-runs it on every `dcli sync`
  (`post_hook_behavior = "always"`).
- `scripts/install-kineticwe.sh` — builds and installs the flake outputs.

Enable it in `~/.config/dcli/hosts/<host>.nix`:

```nix
enabled_modules = [ ... "kineticwe-dev" ];
```

Then:

```bash
dcli validate
dcli sync --dry-run
dcli sync
```

`dcli sync` now means **rebuild and install the dev branch**. Use
`dcli sync --no-hooks` to skip the build.

### Default mode (`local`)

The hook builds two flake outputs into stable out-links:

```
~/.local/state/kineticwe/nix/kinetic-we   -> /nix/store/…-kinetic-we
~/.local/state/kineticwe/nix/noctalia     -> /nix/store/…-noctalia
```

and writes shims to `~/.local/bin/kinetic-we` and
`~/.local/bin/noctalia-kwe`. Your existing `start-kineticwe` launcher
(`INSTALL_PREFIX=$HOME/.local`), portal stack, config setup and
`kineticwe-shell.service` keep working unchanged.

The shims exec the out-link at run time. They clear the host launcher's
`QT_PLUGIN_PATH` / `QML*_IMPORT_PATH` (including the `KWE_*` variants) so the
nix build uses its own Qt/KF6 instead of `~/.local/lib64`, but they *keep* the
host GPU drivers: nixpkgs' `libglvnd` looks in `/run/opengl-driver`, which only
exists on NixOS, so `LD_LIBRARY_PATH` is pointed at dcli's curated
`~/.local/share/dcli/nix-gui-libs` (graphics sonames only — `libEGL_mesa`,
`libEGL_nvidia`, `libgbm`, `libGLX_*`, `libnvidia-*`, …) and
`__EGL_VENDOR_LIBRARY_DIRS` / `LIBGL_DRIVERS_PATH` / `GBM_BACKENDS_PATH` at the
host driver directories. Without that, a nix-built Noctalia or compositor dies
with `eglGetDisplay failed: libEGL_mesa.so.0: cannot open shared object file`.

Existing files are backed up once as `*.pre-nix` (for example
`~/.local/bin/kinetic-we.pre-nix`).

Because the shims resolve the out-link at exec time, a rebuild does not need to
be re-installed — only the out-link has to move:

```bash
nix build --out-link ~/.local/state/kineticwe/nix/kinetic-we .#kinetic-we
```

## Applying a new build without logging out

| What changed | How to apply it | Logout? |
|---|---|---|
| Compositor (`src/`) | New session: log out and pick the dev session again | **Yes** |
| A nested compositor for iteration | see below | No |
| Shell QML (`shell/noctalia`) | `local`: `systemctl --user restart kineticwe-shell` (or Meta+Alt+R)<br>`profile`: `pkill -x noctalia-kwe`, the supervisor restarts it | No |
| Compositor/shell config | hot-reloaded (~200 ms) | No |
| Portal backend | `systemctl --user restart xdg-desktop-portal` | No |

The compositor **is** the Wayland display server: there is no `--replace` on
Wayland, and killing it ends the session. A re-login is enough — no reboot.

### Nested session (no logout)

The standard KWin loop: build, then run the compositor in a window inside your
current session.

```bash
cd /mnt/portablehome/Projects/kineticwe
nix build .#kinetic-we

dbus-run-session ./result/bin/kinetic-we \
    --wayland-display "$WAYLAND_DISPLAY" \
    --width 1920 --height 1080 \
    --xwayland konsole
```

`dbus-run-session` keeps the nested instance off your session bus (global
shortcuts, D-Bus names). Use `--virtual` instead of `--wayland-display` for a
headless run, or `--x11-display :0` to nest on X11.

## Flake source semantics

The hook builds `git+file://$KWE_REPO` — the checkout you are working in:

- Committed changes and **uncommitted changes to tracked files** are built
  (Nix reports `Git tree … is dirty`).
- **Untracked files are not** — `git add` new files before syncing.
- `.gitignore`d paths (`build*/`, `result*`) never enter the store, which is
  why `git+file` is used instead of a `path:` flake reference (that would copy
  the whole checkout, build directories included).

Because the URL has no `?ref=`, it follows the checked-out HEAD instead of a
pinned branch — pinning a ref makes Nix build that clean revision and **drop
your local edits**. Set `KWE_REF=kineticwe-2.0` to do that on purpose.

Shallow clones (`git rev-parse --is-shallow-repository` → `true`) get
`?shallow=1` appended, because Nix refuses to fetch from a shallow repository
otherwise. `git fetch --unshallow` makes the parameter unnecessary.

Each sync rebuilds the KineticWE derivation from scratch (Nix builds are not
incremental). For faster iterations use `ccache` in the daemon or fall back to
`./build-rpm-dev.sh --kwin-only` for the compile step.

## Alternative mode (`profile`)

Instead of shims, the hook can put the flake outputs in the nix profile:

```bash
KWE_INSTALL_MODE=profile dcli sync
```

This installs `#kinetic-we`, `#noctalia` and `#session`, and later syncs run
`nix profile upgrade` on them. Point the dev session entry at the profile
launcher once:

```bash
sudo sed -i 's|^Exec=.*|Exec='"$HOME"'/.nix-profile/bin/start-kineticwe|' \
    /usr/local/share/wayland-sessions/kineticwe-dev.desktop
```

With the profile mode, `start-kineticwe` resolves the compositor and the shell
through `~/.nix-profile/bin`, so:

- a new login picks up a new compositor build,
- restarting the shell picks up a new shell build,
- `nix profile rollback` undoes the last install.

`#session` currently wires `kdePackages.xdg-desktop-portal-kde` rather than the
in-tree `xdg-desktop-portal-kwe`, so screen sharing behaviour can differ from
the packaged session.

## Rolling back

```bash
# local mode: restore the host binaries, drop the out-links
mv ~/.local/bin/kinetic-we.pre-nix    ~/.local/bin/kinetic-we
mv ~/.local/bin/noctalia-kwe.pre-nix  ~/.local/bin/noctalia-kwe
rm -rf ~/.local/state/kineticwe/nix

# profile mode
nix profile rollback

# stop managing the checkout
dcli module disable kineticwe-dev
```
