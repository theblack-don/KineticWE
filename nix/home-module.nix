# Home Manager module for KineticWE.
#
# Usage:
#   programs.kineticwe = {
#     enable = true;
#     # package = pkgs.kineticwe; # optional override
#   };
#
# This will:
#   - Put `start-kineticwe` on the user's PATH
#   - Install a wayland-sessions desktop entry under ~/.local/share
#
# Note: most display managers (SDDM) only scan the system session
# directories. Prefer the NixOS module (programs.kineticwe.enable) so the
# session appears in the greeter. The home-manager desktop file is useful
# for greeters configured to read XDG_DATA_DIRS / user data dirs.
#
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.kineticwe;
in
{
  options.programs.kineticwe = {
    enable = lib.mkEnableOption "KineticWE — KWin window environment with noctalia";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.kineticwe or (throw "programs.kineticwe.package is not set and pkgs.kineticwe is not available. Did you forget to apply the kineticwe overlay? Add `kineticwe.overlays.default` to `nixpkgs.overlays`, or set programs.kineticwe.package explicitly.");
      defaultText = lib.literalExpression "pkgs.kineticwe";
      description = ''
        The KineticWE session package providing `bin/start-kineticwe`.
        Typically the output of the kineticwe flake.
      '';
    };

    autostart = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Whether to autostart KineticWE from the graphical session.
        Most users should leave this `false` and pick "KineticWE" from
        the display manager instead.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    # wayland-sessions entry — picked up by greeters that search XDG data dirs.
    xdg.dataFile."wayland-sessions/KineticWE.desktop".text = ''
      [Desktop Entry]
      Name=KineticWE
      Comment=A Kwin Window Environment
      Type=Application
      Exec=${cfg.package}/bin/start-kineticwe
      DesktopNames=KDE
    '';

    # Optional: autostart from within an existing DE (rare).
    xdg.configFile."autostart/kineticwe.desktop" = lib.mkIf cfg.autostart {
      text = ''
        [Desktop Entry]
        Name=KineticWE
        Comment=Start KineticWE (kwin-we + noctalia)
        Type=Application
        Exec=${cfg.package}/bin/start-kineticwe
        Terminal=false
        X-GNOME-Autostart-enabled=true
      '';
    };
  };
}
