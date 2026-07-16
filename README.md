# KineticWE

> **This repository is a fork of [KWin](https://github.com/KDE/kwin) from the [KDE](https://kde.org) project.**
>
> KineticWE adds native tiling support while keeping the Destktop Enviroment Feel.**KineticWE** — a KWin Window Environment that pairs the [Noctalia](https://github.com/noctalia-dev/noctalia) shell with this compositor.


## ❤ Support the Project

If you want to support this project, you can donate here: [Ko-fi — theblackdon](https://ko-fi.com/theblackdon)

---

> ## ⚠ BETA Stage
>
> **KineticWE is currently in BETA.**
>
> Expect:
> - Bugs, crashes, and visual glitches.
> - Breaking changes between commits (config keys, scripts, CLI flags).
> - Features that work today may be removed or rewritten tomorrow.
>
> Please do **note** use KineticWE as your daily driver on a production system. Feedback and bug reports are welcome, but you are running pre-release software.

> ## 🤖 LLM-Generated Code Disclaimer
>
> Portions of this project — including (but not limited to) installer scripts, build glue, configuration files, documentation, and some C++/QML/JS source files — were **authored with the assistance of LLM tools** (large language models coding assistants).
>
> All LLM-generated content has been reviewed by the maintainers, but it has **not** been audited to the same standard as hand-written code. You may encounter:
> - Subtle bugs or edge cases that a human author would have caught.
> - Stylistic inconsistencies with the surrounding KWin codebase.
>
> **If you are not comfortable running code that was written (in whole or in part) by an LLM, do not use this project and keep it movin.** By using KineticWE you acknowledge and accept this.

---

## Installing KineticWE

### COPR (Fedora) — Recommended - Use Fedora Linux for the best and tested experience 

The recommended way to install KineticWE on Fedora is via [COPR](https://copr.fedorainfracloud.org/).
COPR provides pre-built RPMs so you don't need to compile from source.

Enable the required COPR repositories, then install:

```bash
sudo dnf copr enable lionheartp/Hyprland
sudo dnf copr enable theblackdon/kineticwe
sudo dnf install kineticwe noctalia-git
```

After installation, select **KineticWE** from your display manager's session menu
or run `start-kineticwe` from a TTY.

The `kineticwe` package provides and obsoletes Fedora's stock `kwin`, `kwin-common`,
and `kwin-libs` packages — it replaces the stock compositor rather than installing
alongside it.

### Bazzite / Universal Blue (rpm-ostree)

> **Prerequisite:** The kineticwe COPR package requires **Qt ≥ 6.11**.
> Your base image must include Qt 6.11 or newer — update your system first:
> `sudo bootc upgrade && systemctl reboot`

On rpm-ostree based systems (Bazzite, Bluefin, Aurora, Silverblue, Kinoite),
`kineticwe` replaces the system's stock `kwin`, `kwin-common`, `kwin-libs`,
and `kglobalacceld`. These base packages must be removed from the immutable
image first via `override remove`:

```bash
sudo dnf copr enable lionheartp/Hyprland
sudo dnf copr enable theblackdon/kineticwe
sudo rpm-ostree override remove kwin kwin-common kwin-libs kglobalacceld \
    --install kineticwe --install noctalia-git
systemctl reboot
```

A convenience script is also provided that handles this automatically,
including checking the Qt version:

```bash
git clone https://gitlab.com/theblackdon/kineticwe.git
cd kineticwe
./install-bazzite.sh
systemctl reboot
```

Bazzite users can also use the `ujust` recipe from `bazzite/kineticwe-ublue.just`
(import it into your custom justfile or run directly):

```bash
just --justfile bazzite/kineticwe-ublue.just install-kineticwe
```

For building a custom bootc derivative image, see [`bazzite/Containerfile`](bazzite/Containerfile).

### Install via Scripts (Alternative / Other Distros)

Convenience scripts are provided that install all dependencies, build kglobalacceld
(the global shortcuts daemon), build kwin-we, install the
[noctalia-shell](https://github.com/noctalia-dev/noctalia), and set up session
files so you can launch KineticWE from a TTY.


> **For Fedora users:** Pre-built packages are available via COPR — see [Fedora / RHEL](#copr-fedora--recommended---use-fedora-linux-for-the-best-and-tested-experience) above.
> **For Arch users:** The kineticwe package is available on the [AUR](https://aur.archlinux.org/packages/kineticwe) (`yay -S kineticwe`).

> **Why kglobalacceld is built from source:** kwin-we calls `keyEvent()`, `pointerPressed()`, `axisTriggered()` and `resetModifierOnlyState()` directly on the `KGlobalAccelD` object — an API introduced in kglobalacceld git master (post-6.7.0). The system package on most distros is still 6.7.0 or older and routes those calls through a separate `KGlobalAccelInterface` class, so kwin-we cannot compile against it. The install scripts clone kglobalacceld from `invent.kde.org/plasma/kglobalacceld.git` and build it into `$INSTALL_PREFIX` so kwin-we's CMake finds the newer version first. This requires **Qt ≥ 6.10** and **KF6 ≥ 6.26** (satisfied on Fedora 44+, Arch rolling, and Debian Trixie/Ubuntu 24.04+ with backports).

Convenience scripts are provided that install all dependencies, build kglobalacceld
(the global shortcuts daemon), build kwin-we, install the
[noctalia-shell](https://github.com/noctalia-dev/noctalia), and set up session
files so you can launch KineticWE from a TTY.

** Again! Please Note** The COPR build on FEDORA is the best experience that is well tested as these scripts SHOULD work but may have issues. 

**Quick start (copy & paste):**

```bash
git clone https://gitlab.com/theblackdon/kineticwe.git
cd kineticwe
./install-fedora.sh   # or install-arch.sh, install-debian.sh
```

Pick the script that matches your distribution:

| Distro | Method |
|--------|--------|
| Fedora / RHEL | COPR (recommended) — see below |
| Fedora / RHEL | `./install-fedora.sh` (manual build) |
| **Bazzite / uBlue (rpm-ostree)** | `./install-bazzite.sh` — see below |
| Arch Linux / Arch-derivatives | [AUR](https://aur.archlinux.org/packages/kineticwe) — `yay -S kineticwe` |
| Arch Linux / Arch-derivatives | `./install-arch.sh` (manual build) |
| Debian / Ubuntu | `./install-debian.sh` |
| NixOS / Nix (flake) | — see below |
| Gentoo | `./install-gentoo.sh` — uses the community Portage overlay |

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

### Fedora / RHEL (COPR — Recommended)

KineticWE is available via COPR for Fedora. Enable both the Noctalia and KineticWE repositories, then install:

```bash
sudo dnf copr enable lionheartp/Hyprland
sudo dnf copr enable theblackdon/kineticwe
sudo dnf install kineticwe noctalia-git
```

> **Note:** The first time you install, reboot before logging in to a KineticWE session. On subsequent logins Noctalia will load automatically.
>
> **Tip:** Press **Meta+Space** to open the app drawer.

### Arch Linux

The recommended way to install on Arch is via the [AUR](https://aur.archlinux.org/packages/kineticwe):

```bash
yay -S kineticwe
```

Or build manually:

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

**NixOS module (recommended — registers the Wayland session with SDDM/greetd):**
```nix
{
  imports = [ inputs.kineticwe.nixosModules.default ];
  programs.kineticwe.enable = true;
}
```

**Home Manager module** (puts `start-kineticwe` on PATH; use the NixOS module so the greeter lists the session):
```nix
{
  imports = [ inputs.kineticwe.homeModules.default ];
  nixpkgs.overlays = [ inputs.kineticwe.overlays.default ];
  programs.kineticwe.enable = true;
}
```

> **Note:** On the first build, Nix may error with a hash mismatch for `kglobalacceld-git`. Copy the expected hash from the error into `nix/kglobalacceld-git.nix` and rebuild. (This should be resolved)

### Gentoo

Gentoo installation is provided through a community-maintained Portage overlay:

```text
https://github.com/TwelfthFace/kineticwe-overlay
```

The recommended install method is to use the Gentoo installer script:

```bash
./install-gentoo.sh
```

The script will:

* install `dev-vcs/git` if needed
* install `app-eselect/eselect-repository` if needed
* add the `kineticwe-overlay` Portage repository
* sync the overlay
* install `kde-plasma/kineticwe`

Manual installation:

```bash
sudo emerge --ask app-eselect/eselect-repository dev-vcs/git
sudo eselect repository add kineticwe-overlay git https://github.com/TwelfthFace/kineticwe-overlay.git
sudo emaint sync -r kineticwe-overlay
sudo emerge --ask kde-plasma/kineticwe
```

> **Note:** The Gentoo package is currently provided as a `9999` live ebuild.
>
> This means Portage builds KineticWE directly from the current upstream Git repository. Installing or updating the package may pull the latest development code, which can include new features, fixes, regressions, or breaking changes.
>
> In other words, the Gentoo package should currently be considered bleeding edge.
>
> Once KineticWE has stable tagged releases, the overlay will provide versioned ebuilds alongside the live `9999` ebuild.


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

