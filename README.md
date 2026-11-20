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
sudo dnf copr remove linuxgamerlife/lgl-experimental #this is where the pre-open beta was. Remove this repo first!
sudo dnf copr enable theblackdon/kwee #this is the official open beta repo. NOTE It's still a beta!
sudo dnf install kineticwe
```

`kineticwe` pulls in the Noctalia shell (`kineticwe-noctalia`), the KineticWE
portal backend (`xdg-desktop-portal-kwe`), and the Wayland session entry.
KineticWE 2.0 installs alongside Fedora's stock KWin packages rather than
replacing them.

After installation, log out and select **KineticWE** from your display manager's
session menu, or run `start-kineticwe` from a TTY.

> **Tip:** Press **Meta+Space** to open the app drawer.

Optionally, install the Noctalia greeter (greetd) and run its interactive setup:

```bash
sudo dnf install kineticwe-greeter
kineticwe-setup-greeter
```

Then reboot.

### [UNINSTALL KineticV1 INSTRUCTIONS]
1. login to a KDE or KineticWE session
2. `systemctl set-default multi-user.target` (TTY after reboot)
3. `reboot`
4. login
5. `sudo dnf copr remove theblackdon/kineticwe`
6. `sudo dnf copr remove lionheartp/Hyprland`
7. `sudo rpm -e --nodeps kineticwe plasma-desktop plasma-workspace kwin kwin-libs kwin-common`
8. `sudo dnf upgrade --refresh`
9. `sudo dnf group install kde-desktop`
10. `systemctl set-default graphical.target`
11. `reboot` 

## Community-maintained installation instructions

The following instructions are maintained by community contributors. They are
not part of the official Fedora COPR release and may lag behind the latest
changes. If a command fails, please
[open an issue](https://github.com/theblack-don/KineticWE/issues).

### Arch Linux

#### Binary repo (recommended)

The fastest way to install KineticWE on Arch-based distros (Arch, CachyOS, EndeavourOS, Manjaro, etc.) is the pre-built binary repo hosted on GitHub Releases — no compiling needed.

Packages are signed with the KineticWE repo key `FD258A99CDC5F61B04A335EEE163D34B273AE987`. Download it, check the fingerprint, and trust it once:

```bash
curl -fLO https://github.com/theblack-don/KineticWE/releases/download/repo/kineticwe.gpg
gpg --show-keys kineticwe.gpg          # must show FD258A99CDC5F61B04A335EEE163D34B273AE987
sudo pacman-key --add kineticwe.gpg
sudo pacman-key --lsign-key FD258A99CDC5F61B04A335EEE163D34B273AE987
```

(If GitHub is unreachable, fetch the key from a keyserver instead of the first three lines: `sudo pacman-key --recv-keys FD258A99CDC5F61B04A335EEE163D34B273AE987 --keyserver hkps://keys.openpgp.org`.)

Add the repo to `/etc/pacman.conf` (e.g. `sudo nano /etc/pacman.conf`):

```ini
[kineticwe]
Server = https://github.com/theblack-don/KineticWE/releases/download/repo
```

and install:

```bash
sudo pacman -Syu kineticwe-keyring kineticwe
```

This also pulls in the `xdg-desktop-portal-kwe` portal backend and the `kineticwe-noctalia` shell. `kineticwe-keyring` keeps the repo key up to date, so future key changes arrive as normal updates.

> **Coming from the old GitLab repo?** Replace the old `Server = https://theblackdon.gitlab.io/kineticwe/$arch` line in `/etc/pacman.conf` with the new one above (note: no `/$arch` at the end), then run `sudo pacman -Syu`. The signing key is the same, so there is nothing to re-trust. The old GitLab repo no longer receives updates.

Already using the repo with `SigLevel = Never`? Trust the key as above, install `kineticwe-keyring`, then delete the `SigLevel = Never` line.

Optionally, install the Noctalia greeter (greetd) and run its interactive setup (it asks for `sudo` itself when needed):

```bash
sudo pacman -S kineticwe-greeter
kineticwe-setup-greeter
```

> **Heads-up: "breaks dependency 'qt6-base=…'" during `pacman -Syu`? Don't panic, just wait.**
>
> KineticWE has to be rebuilt for each new Qt version. When Arch ships a new Qt, `pacman -Syu` may stop with an error like this:
>
> ```
> :: installing qt6-base (6.x.y-1) breaks dependency 'qt6-base=6.x.x' required by kineticwe
> ```
>
> That's expected, and your system is fine. The repo checks for new Qt versions **every 12 hours** and rebuilds automatically, and the build itself can take a couple more hours. **Please wait at least 12 hours** and run `sudo pacman -Syu` again before opening an issue. In most cases it will just work by then.
>
> Please don't force it with `--overwrite`, `-dd`, or `--ignore qt6-base`. That gives you a partial upgrade, and KineticWE may not start afterwards. If it still fails after a day, [open an issue](https://github.com/theblack-don/KineticWE/issues) and we'll take a look.

#### Build from source

> **Warning:** A source build only works with the exact Qt version it was built against, because KWin uses Qt's private APIs. **After every Qt update from Arch, run `./install-arch.sh --update` before you log back in.** If you skip it, KineticWE may crash or fail to start. The binary repo avoids this: it's rebuilt regularly for new Qt versions, and pacman won't install a new Qt until the matching KineticWE build is published (see the heads-up above).

Build and install everything (compositor, portal, shell and greeter):

```bash
./install-arch.sh
```

The script shows what it will do and asks before starting.

**Keeping your current login screen** (e.g. SDDM or GDM): add `--no-greeter`. If `kineticwe-greeter` is already installed, remove it first with `sudo pacman -R kineticwe-greeter`.

```bash
./install-arch.sh --no-greeter
```

**Updating:** after a `git pull` or a Qt update, run:

```bash
./install-arch.sh --update
```

If nothing changed, it exits right away. Otherwise it rebuilds. Rebuilds after the first one are much faster.

**More options:** `./install-arch.sh --help`. For a development setup that installs into `~/.local`, see [docs/local-arch-dev.md](docs/local-arch-dev.md). Coming from the old `build-arch-native.sh`? See [docs/arch-migration.md](docs/arch-migration.md).

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

### Nix / NixOS

KineticWE ships a flake that builds the compositor, the Noctalia shell, the
session launcher, and the portal backend. The first build compiles KWin, so it
can take a while. The flake builds the branch it is fetched from;
`github:theblack-don/KineticWE` resolves the stable `master` branch. Moving from the
old `gitlab:theblackdon/kineticwe` input? Change the URL, then run
`nix flake update kineticwe` once so your `flake.lock` follows it.

#### NixOS (recommended)

Add the flake as an input:

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    kineticwe.url = "github:theblack-don/KineticWE";
  };

  outputs = { self, nixpkgs, kineticwe, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux"; # or aarch64-linux
      modules = [
        kineticwe.nixosModules.default
        { programs.kineticwe.enable = true; }
      ];
    };
  };
}
```

Then rebuild:

```bash
sudo nixos-rebuild switch --flake .#myhost
```

The module installs KineticWE, Noctalia, the decorations, and registers the
**KineticWE** Wayland session with your display manager. Log out and pick it
from the session menu.

#### Other Nix installations

Install the session and its runtime packages into your profile:

```bash
nix profile install \
  github:theblack-don/KineticWE#kinetic-we \
  github:theblack-don/KineticWE#noctalia \
  github:theblack-don/KineticWE#kdecoration \
  github:theblack-don/KineticWE#kglobalacceld \
  github:theblack-don/KineticWE#session
```

The session entry is installed at
`~/.nix-profile/share/wayland-sessions/KineticWE.desktop`. If your display
manager does not scan that directory, copy it into the system session directory:

```bash
sudo install -Dm644 ~/.nix-profile/share/wayland-sessions/KineticWE.desktop \
  /usr/local/share/wayland-sessions/KineticWE.desktop
```

Then log out and select **KineticWE**, or run `start-kineticwe` from a TTY.
Update later with `nix profile upgrade --all`.

#### Development install

Working on KineticWE itself? See
[docs/nix-dev-install.md](docs/nix-dev-install.md) for a development install
that builds your checkout through the flake and installs it with dcli.
