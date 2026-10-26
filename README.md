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

> # LLM-Generated Code Disclaimer
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

### Fedora (COPR) — Recommended

The recommended way to install KineticWE on Fedora is via [COPR](https://copr.fedorainfracloud.org/).
COPR provides pre-built RPMs, so you don't need to compile from source.

```bash
sudo dnf copr enable linuxgamerlife/lgl-experimental
sudo dnf install kineticwe
```

`kineticwe` pulls in the Noctalia shell (`kineticwe-noctalia`), the KineticWE
portal backend (`xdg-desktop-portal-kwe`), and the Wayland session entry.
KineticWE 2.0 installs alongside Fedora's stock KWin packages rather than
replacing them.

After installation, log out and select **KineticWE** from your display manager's
session menu, or run `start-kineticwe` from a TTY.

> **Tip:** Press **Meta+Space** to open the app drawer.

Working on KineticWE itself? [docs/nix-dev-install.md](docs/nix-dev-install.md)
describes a development install that builds your checkout through the repo's
Nix flake and installs it with dcli.

Optionally, install the Noctalia greeter (greetd) and run its interactive setup:

```bash
sudo dnf install kineticwe-greeter
kineticwe-setup-greeter
```

Then reboot.

### Arch Linux

#### Binary repo (recommended)

The fastest way to install KineticWE on Arch-based distros (Arch, CachyOS, EndeavourOS, Manjaro, etc.) is the pre-built binary repo hosted on GitLab Pages — no compiling needed.

Packages are signed with the KineticWE repo key `FD258A99CDC5F61B04A335EEE163D34B273AE987`. Trust it once:

```bash
sudo pacman-key --recv-keys FD258A99CDC5F61B04A335EEE163D34B273AE987 --keyserver hkps://keys.openpgp.org
sudo pacman-key --lsign-key FD258A99CDC5F61B04A335EEE163D34B273AE987
```

Add the repo to `/etc/pacman.conf` (e.g. `sudo nano /etc/pacman.conf`):

```ini
[kineticwe]
Server = https://theblackdon.gitlab.io/kineticwe/$arch
```

and install:

```bash
sudo pacman -Sy kineticwe-keyring kineticwe
```

This also pulls in the `xdg-desktop-portal-kwe` portal backend and the `kineticwe-noctalia` shell. `kineticwe-keyring` keeps the repo key up to date, so future key changes arrive as normal updates.

Already using the repo with `SigLevel = Never`? Run the two `pacman-key` commands above, install `kineticwe-keyring`, then delete the `SigLevel = Never` line.

Optionally, install the Noctalia greeter (greetd) and run its interactive setup (it asks for `sudo` itself when needed):

```bash
sudo pacman -S kineticwe-greeter
kineticwe-setup-greeter
```

#### Build from source

To build the split packages (`kineticwe`, `xdg-desktop-portal-kwe`, `kineticwe-noctalia`, `kineticwe-greeter`) from your working tree and install them with pacman:

```bash
./install-arch.sh
```

Already have a display manager you want to keep (e.g. SDDM from Plasma or GDM from GNOME)? Add `--no-greeter`: `kineticwe-greeter` isn't built or installed, and your login screen is left alone.

```bash
./install-arch.sh --no-greeter
```

If `kineticwe-greeter` is already installed, remove it first (`sudo pacman -R kineticwe-greeter`): it requires the exact `kineticwe` version, so `kineticwe` can't be rebuilt without it.

After that first install, rebuild only when something changed:

```bash
./install-arch.sh --update
```

`--update` compares each component's sources (compositor, portal, shell, greeter; uncommitted edits count) with what they were last built from and exits without building anything when nothing changed. Otherwise it rebuilds all packages together, because they require each other's exact version. The build uses ccache (kept in `build.arch-pkg/ccache`), so code that didn't change compiles from the cache; the first build only fills it. A Qt update counts as a change for the compositor and the portal.

The script shows what it will do and asks before starting. Pass `--yes` to skip the question (required when it isn't run from a terminal).

For a development loop that installs into `~/.local` instead (`--local-dev`), see [docs/local-arch-dev.md](docs/local-arch-dev.md).

> **Note:** KWin uses Qt private APIs, so a source build is tied to the exact Qt version it was built against. After every Arch Qt update, rebuild (the binary repo is rebuilt automatically for this).

> **Note:** The Fedora spec (`lgl-kineticwe-copr/kineticwe.spec`) is the source of truth for the version, release and pinned commits. Builds always use the spec's values; if the committed `PKGBUILD` lags behind, the build prints a warning and continues. Run `arch/sync-from-spec.sh PKGBUILD` to update it. Changes beyond version and pins (dependencies, files, patches) still need a manual `PKGBUILD` edit.

Used an older version of the script (`build-arch-native.sh`, `--full`, `--binary`, `--kwin-only`, `--noct-only`)? See [docs/arch-migration.md](docs/arch-migration.md).

#### Setting up the greeter

Installing `kineticwe-greeter` does not change your display manager. Run the setup once, then reboot:

```bash
kineticwe-setup-greeter
```

Enabling `greetd.service` on its own is not enough: Arch's default greetd config runs `agreety`, a text login that drops you into a shell.

#### Migrating from a local (`~/.local`) build to the packages

Used `./install-arch.sh --local-dev` before? See [docs/local-arch-dev.md](docs/local-arch-dev.md#migrating-from-a-local-local-build-to-the-packages) to remove the dev install after switching to the packages.

### openSUSE (Tumbleweed & Slowroll)

The easiest way to install KineticWE on openSUSE Tumbleweed or Slowroll is via the openSUSE Build Service (OBS).

**For Tumbleweed:**
```bash
sudo zypper addrepo https://download.opensuse.org/repositories/home:benyx31:kineticwe/openSUSE_Tumbleweed/home:benyx31:kineticwe.repo
sudo zypper refresh
sudo zypper install kineticwe
```

**For Slowroll:**
```bash
sudo zypper addrepo https://download.opensuse.org/repositories/home:benyx31:kineticwe/openSUSE_Slowroll/home:benyx31:kineticwe.repo
sudo zypper refresh
sudo zypper install kineticwe
```

Optionally, install the Noctalia greeter and run its setup:
```bash
sudo zypper install kineticwe-greeter
kineticwe-setup-greeter
```
