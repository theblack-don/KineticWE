# KineticWE

> **This repository is a fork of [KWin](https://github.com/KDE/kwin) from the [KDE](https://kde.org) project.**
>
> KineticWE adds native tiling support while keeping the Destktop Enviroment Feel.**KineticWE** — a KWin Window Environment that pairs the [Noctalia](https://github.com/noctalia-dev/noctalia) shell with this compositor.


---

> ## ⚠️ BETA Stage
>
> **KineticWE is currently in BETA.**
>
> Expect:
> - Bugs, crashes, and visual glitches.
> - Breaking changes between commits (config keys, scripts, CLI flags).
> - Features that work today may be removed or rewritten tomorrow.
>
> Please do **not** use KineticWE as your daily driver on a production system. Feedback and bug reports are welcome, but you are running pre-release software.

> ## 🤖 LLM-Generated Code Disclaimer
>
> Portions of this project — including (but not limited to) installer scripts, build glue, configuration files, documentation, and some C++/QML/JS source files — were **authored with the assistance of LLM tools** (large language models coding assistants).
=======
> Portions of this project — including (but not limited to) installer scripts, build glue, configuration files, documentation, and some C++/QML/JS source files — were **authored with the assistance of AI tools** (large language models coding assistants).
>>>>>>> master
>
> All LLM-generated content has been reviewed by the maintainers, but it has **not** been audited to the same standard as hand-written code. You may encounter:
> - Subtle bugs or edge cases that a human author would have caught.
> - Stylistic inconsistencies with the surrounding KWin codebase.
>
> **If you are not comfortable running code that was written (in whole or in part) by an LLM, do not use this project and keep it movin.** By using KineticWE you acknowledge and accept this.

---

## Installing KineticWE

Convenience scripts are provided that install all dependencies, build kglobalacceld (the global shortcuts daemon), build kwin-we, install the [noctalia-shell](https://github.com/noctalia-dev/noctalia), and set up session files so you can launch KineticWE from a TTY.

> **Why kglobalacceld is built from source:** kwin-we calls `keyEvent()`, `pointerPressed()`, `axisTriggered()` and `resetModifierOnlyState()` directly on the `KGlobalAccelD` object — an API introduced in kglobalacceld git master (post-6.7.0). The system package on most distros is still 6.7.0 or older and routes those calls through a separate `KGlobalAccelInterface` class, so kwin-we cannot compile against it. The install scripts clone kglobalacceld from `invent.kde.org/plasma/kglobalacceld.git` and build it into `$INSTALL_PREFIX` so kwin-we's CMake finds the newer version first. This requires **Qt ≥ 6.10** and **KF6 ≥ 6.26** (satisfied on Fedora 44+, Arch rolling, and Debian Trixie/Ubuntu 24.04+ with backports).

### Install via Scripts
Pick the script that matches your distribution:

| Distro | Script |
|--------|--------|
| Fedora / RHEL | `./install-fedora.sh` |
| Arch Linux / Arch-derivatives | `./install-arch.sh` |
| Debian / Ubuntu | `./install-debian.sh` |
| NixOS / Nix (flake) | — see below |

In all cases, by default binaries are installed to `$HOME/.local`. You can change this with the `INSTALL_PREFIX` environment variable:

```bash
INSTALL_PREFIX=/opt/kineticwe ./install-<distro>.sh
```

After installation:

- Start KineticWE from a TTY with:
  ```bash
  $HOME/.local/bin/start-kineticwe
  ```
- Or select **KineticWE** from the session menu in SDDM or another Wayland-compatible greeter. (This does not work well with PlasmaLogin, please use SDDM, LightDM, or another wayland greeter)
- If `$HOME/.local/bin` is not in your `PATH`, add the following lines to your shell profile (`~/.bashrc` or `~/.zshrc`):
  ```bash
  export PATH="$HOME/.local/bin:$PATH"
  # Fedora / Arch (64-bit lib dir):
  export LD_LIBRARY_PATH="$HOME/.local/lib64:$LD_LIBRARY_PATH"
  # Debian / Ubuntu (multiarch lib dir):
  # export LD_LIBRARY_PATH="$HOME/.local/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
  ```

### Fedora / RHEL

```bash
./install-fedora.sh
```

> **Note:** The script uses `sudo` to install system packages via `dnf`. Make sure you have sudo privileges before running it.

### Arch Linux

```bash
./install-arch.sh
```

> **Note:** The script uses `sudo` to install system packages via `pacman`. Arch unifies runtime and headers into a single package (no `-devel` split), so the package list is slightly different from the Fedora script — see the comments inside the script for details.

### Debian / Ubuntu

```bash
./install-debian.sh
```

> **Note:** Should work on **Debian 13 (Trixie)** and **Ubuntu 24.04+** with Plasma 6 packages available. On older releases you may need to enable backports (Debian) or the Kubuntu backports PPA (Ubuntu) so that KF6, Qt6 and Plasma 6 are available. The `sdbus-cpp-dev` package is not in the official Debian/Ubuntu repos; it is only needed for a small portion of the codebase and the build will succeed without it.

### NixOS / Nix

A [Nix flake](https://nixos.wiki/wiki/Flakes) provides a reproducible build of kwin-we, the full KineticWE session (kwin-we + noctalia), and Home Manager / NixOS modules.

**How it works:** The flake uses `nixpkgs/master` (for Plasma 6.7 / KF6 6.27) and includes a custom `kglobalacceld-git` derivation that builds the global shortcuts daemon from KDE invent git master. This provides the newer `KGlobalAccelD` API (`keyEvent`, `pointerPressed`, `axisTriggered`, `resetModifierOnlyState`) that kwin-we requires.

**Add to your flake.nix inputs:**
```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    kineticwe = {
      url = "gitlab:theblackdon/kineticwe";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
}
```

**Home Manager module:**
```nix
{
  imports = [ inputs.kineticwe.homeModules.default ];
  programs.kineticwe.enable = true;
}
```

> **Note:** On the first build, Nix will error with a hash mismatch for `kglobalacceld-git`. Copy the expected hash from the error into `nix/kglobalacceld-git.nix` and rebuild. (This should be resolved)

---

## KWin is not...

 * a standalone Wayland compositor (c.f. labwc, sway) and does not provide any functionality belonging to a Desktop Shell.
 * a replacement for window managers designed for use with a specific Desktop Shell (e.g. GNOME Shell)
 * a minimalistic window manager
 * designed for use with network transparency, though it is possible (with e.g. waypipe).

# Contributing to KWin

Please refer to the [contributing document](CONTRIBUTING.md) for everything you need to know to get started contributing to KWin.

# Contacting KWin development team

 * IRC: #kde-kwin on irc.libera.chat
 * Matrix: [#kwin:kde.org](https://go.kde.org/matrix/#/#kwin:kde.org)

# Support
## Application Developer
If you are an application developer having questions regarding windowing systems (either X11 or Wayland) please do not hesitate to contact us.

## End user
Please contact the support channels of your Linux distribution for user support. The KWin development team does not provide end user support.

# Reporting bugs

Please use [KDE's bugtracker](https://bugs.kde.org) and report for [product KWin](https://bugs.kde.org/enter_bug.cgi?product=kwin). For **KineticWE-specific** issues, please open an issue on this repository instead.

## Guidelines for new features

A new Feature can only be added to KWin if:

 * it does not violate the primary missions as stated at the start of this document
 * it does not introduce instabilities
 * it is maintained, that is bugs are fixed in a timely manner (second next minor release) if it is not a corner case.
 * it works together with all existing features
 * it supports both single and multi screen
 * it adds a significant advantage
 * it is feature complete, that is supports at least all useful features from competitive implementations
 * it is not a special case for a small user group
 * it does not increase code complexity significantly
 * it does not affect KWin's license (GPLv2+)

All new added features are under probation, that is if any of the non-functional requirements as listed above do not hold true in the next two feature releases, the added feature will be removed again.

The same non functional requirements hold true for any kind of plugins (effects, scripts, etc.). It is suggested to use scripted plugins and distribute them separately.
