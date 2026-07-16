# NixOS module for KineticWE.
#
# Usage (flake-based configuration):
#   {
#     imports = [ inputs.kineticwe.nixosModules.default ];
#     programs.kineticwe.enable = true;
#   }
#
# The flake's nixosModules.default also applies the kineticwe overlay so
# pkgs.kineticwe / pkgs.kwin-we / pkgs.noctalia resolve automatically.
#
# This installs the session package system-wide and registers it with
# services.displayManager.sessionPackages so SDDM / greetd offer a real
# Wayland session (not an X11 wrapper from displayManager.session).
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
      default = pkgs.kineticwe or (throw "programs.kineticwe.package is not set and pkgs.kineticwe is not available. Did you forget to apply the kineticwe overlay? Use inputs.kineticwe.nixosModules.default (which applies it), or add `kineticwe.overlays.default` to `nixpkgs.overlays`.");
      defaultText = lib.literalExpression "pkgs.kineticwe";
      description = ''
        The KineticWE session package providing `bin/start-kineticwe`.
        Typically the output of the kineticwe flake.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      cfg.package
      pkgs.kwin-we
      pkgs.noctalia
    ];

    # Register as a Wayland session. Requires passthru.providedSessions on
    # the package (see nix/session.nix). Do NOT use the legacy
    # services.xserver.displayManager.session list — that generates an X11
    # session wrapper and login fails for a Wayland compositor.
    services.displayManager.sessionPackages = [ cfg.package ];

    # KDE portal backend for screenshots / screencast / file dialogs.
    xdg.portal = {
      enable = true;
      extraPortals = [ pkgs.kdePackages.xdg-desktop-portal-kde ];
      config.common.default = [ "kde" ];
    };
  };
}
