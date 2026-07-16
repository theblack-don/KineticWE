{
  lib,
  stdenv,
  makeWrapper,
  kwinWe,
  noctalia,
  xdg-desktop-portal,
  xwayland,
  kdePackages,
  psmisc,
  upower,
}:

let
  # Literal dollar — needed so Nix doesn't interpret shell ${...}
  # as Nix interpolation inside the script heredoc.
  dl = "$";
in
stdenv.mkDerivation {
  pname = "kineticwe-session";
  version = "unstable";

  dontUnpack = true;
  dontConfigure = true;
  dontBuild = true;

  nativeBuildInputs = [ makeWrapper ];

  installPhase = ''
    runHook preInstall

    mkdir -p "$out/bin" "$out/share/wayland-sessions"

    # Desktop entry so SDDM / greetd can list KineticWE as a Wayland session.
    # The basename (KineticWE) must match passthru.providedSessions below.
    cat > "$out/share/wayland-sessions/KineticWE.desktop" << DESKTOPEOF
[Desktop Entry]
Name=KineticWE
Comment=KineticWE Wayland session
Type=Application
Exec=$out/bin/start-kineticwe
DesktopNames=KDE
DESKTOPEOF

    cat > "$out/bin/start-kineticwe" << 'SCRIPTEOF'
#!/bin/bash
# KineticWE — session launcher (Nix-built).
# Mirrors scripts/start-kineticwe.sh / the COPR RPM launcher.
set -e

KINETIC_WE="${kwinWe}/bin/kinetic-we"

export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_TYPE=wayland
export XDG_SESSION_DESKTOP=KDE
export KDE_SESSION_VERSION=6
# Without this, kbuildsycoca6 fails on minimal installs (Arch, Nix, etc.).
export XDG_MENU_PREFIX=plasma-

# Prefer kwin-we data (effects, scripts, configs) over anything else.
export XDG_DATA_DIRS="${kwinWe}/share:${dl}{XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

SESSION_LOG_DIR="${dl}{XDG_RUNTIME_DIR:-/tmp}/kineticwe-${dl}USER"
mkdir -p "${dl}SESSION_LOG_DIR" 2>/dev/null || true
SESSION_LOG="${dl}SESSION_LOG_DIR/session.log"

log() {
    echo "[$(date '+%H:%M:%S')] ${dl}*" >> "${dl}SESSION_LOG"
}

log "=== KineticWE session starting ==="
log "KINETIC_WE=${dl}KINETIC_WE"

# Build the KWin startup payload (runs as KWin's child process).
STARTUP_PAYLOAD_DIR="${dl}{XDG_RUNTIME_DIR:-/tmp}/kineticwe-${dl}USER"
mkdir -p "${dl}STARTUP_PAYLOAD_DIR" 2>/dev/null || true
STARTUP_PAYLOAD="${dl}STARTUP_PAYLOAD_DIR/startup.sh"

cat > "${dl}STARTUP_PAYLOAD" << 'PAYLOADEOF'
#!/bin/bash
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_TYPE=wayland
export XDG_SESSION_DESKTOP=KDE
export KDE_SESSION_VERSION=6
export XDG_MENU_PREFIX=plasma-
export XDG_DATA_DIRS="@kwin_share@:${dl}{XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

# Rebuild KDE service cache so shortcuts / System Settings find components.
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental 2>/dev/null || true
fi

# Portals: KDE backend first so the frontend discovers Screenshot/ScreenCast.
killall -q xdg-desktop-portal-kde xdg-desktop-portal xdg-desktop-portal-gtk xdg-document-portal 2>/dev/null || true
sleep 1

mkdir -p "$HOME/.local/share" 2>/dev/null || true

# xdg-desktop-portal-kde needs XDG_CURRENT_DESKTOP=KDE to register correctly.
XDG_CURRENT_DESKTOP=KDE nohup @xdg_desktop_portal_kde@ >"$HOME/.local/share/xdg-desktop-portal-kde.log" 2>&1 &
sleep 2
nohup @xdg_desktop_portal@ >"$HOME/.local/share/xdg-desktop-portal.log" 2>&1 &

# Power management (best-effort; paths are Nix store binaries).
nohup @upowerd@ >/dev/null 2>&1 &
nohup @powerdevil@ >/dev/null 2>&1 &

# Shell must stay in the foreground: --exit-with-session ends the compositor
# when this payload exits. Do not background noctalia or sleep-and-exit.
exec @noctalia@
PAYLOADEOF

chmod +x "${dl}STARTUP_PAYLOAD"

# Stop any competing global-shortcuts daemon so kwin-we can own the D-Bus name.
systemctl --user stop plasma-kglobalaccel.service 2>/dev/null || true
pkill -x kglobalacceld 2>/dev/null || true

log "Launching: ${dl}KINETIC_WE --xwayland --exit-with-session ${dl}STARTUP_PAYLOAD"
exec "${dl}KINETIC_WE" --xwayland --exit-with-session "${dl}STARTUP_PAYLOAD"
SCRIPTEOF
    chmod +x "$out/bin/start-kineticwe"

    # Substitute store paths into the payload (so children of kinetic-we
    # find the binaries without relying on PATH propagation).
    substituteInPlace "$out/bin/start-kineticwe" \
      --replace-fail "@noctalia@" "${noctalia}/bin/noctalia" \
      --replace-fail "@kwin_share@" "${kwinWe}/share" \
      --replace-fail "@xdg_desktop_portal@" "${xdg-desktop-portal}/bin/xdg-desktop-portal" \
      --replace-fail "@xdg_desktop_portal_kde@" "${kdePackages.xdg-desktop-portal-kde}/bin/xdg-desktop-portal-kde" \
      --replace-fail "@upowerd@" "${upower}/libexec/upowerd" \
      --replace-fail "@powerdevil@" "${kdePackages.powerdevil}/libexec/org_kde_powerdevil"

    # Wrap so helpers (xwayland, killall, kbuildsycoca6) are on PATH.
    wrapProgram "$out/bin/start-kineticwe" \
      --prefix PATH : "${lib.makeBinPath [
        xwayland
        psmisc
        kdePackages.kservice
      ]}"

    runHook postInstall
  '';

  # Required by services.displayManager.sessionPackages — basename of the
  # wayland-sessions desktop file without .desktop.
  passthru.providedSessions = [ "KineticWE" ];

  meta = with lib; {
    description = "KineticWE session entry (start-kineticwe launcher)";
    license = licenses.gpl2Plus;
    platforms = platforms.linux;
    mainProgram = "start-kineticwe";
  };
}
