{
  description = "KineticWE — a KWin window environment (KWin fork) bundled with the noctalia shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    flake-utils.url = "github:numtide/flake-utils";

    # Track the upstream noctalia-shell flake. Its outputs include the
    # binary, dev shell, home-manager and NixOS modules.
    noctalia = {
      url = "github:noctalia-dev/noctalia";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { self
    , nixpkgs
    , flake-utils
    , noctalia
    }:
    {
      # ---- cross-system modules --------------------------------------------
      # Apply the overlay automatically so pkgs.kineticwe resolves when the
      # module is imported from a flake-based NixOS config.
      nixosModules.default = { ... }: {
        imports = [ ./nix/nixos-module.nix ];
        nixpkgs.overlays = [ self.overlays.default ];
      };
      homeModules.default = import ./nix/home-module.nix;

      overlays.default = final: prev: {
        kineticwe = self.packages.${prev.stdenv.hostPlatform.system}.default
          or (throw "kineticwe flake does not support ${prev.stdenv.hostPlatform.system}");
        kwin-we = self.packages.${prev.stdenv.hostPlatform.system}.kwin-we
          or (throw "kineticwe flake does not support ${prev.stdenv.hostPlatform.system}");
        noctalia = self.packages.${prev.stdenv.hostPlatform.system}.noctalia
          or (throw "kineticwe flake does not support ${prev.stdenv.hostPlatform.system}");
      };
    }
    //     flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          # KWin + noctalia build a lot of native code; keep the closure
          # to linux only.
          config.allowUnfree = true;
        };

        kwinWe = pkgs.callPackage ./nix/kwin-we.nix {
          kglobalacceldGit = pkgs.callPackage ./nix/kglobalacceld-git.nix { };
        };

        # The session package wires kwin-we + noctalia + xdg-desktop-portal
        # together and produces `bin/start-kineticwe`.
        session = pkgs.callPackage ./nix/session.nix {
          inherit kwinWe;
          noctalia = noctalia.packages.${system}.default;
          xwayland = pkgs.xwayland;
        };
      in
      {
        # ---- packages ---------------------------------------------------------
        packages = {
          default = session;
          kwin-we = kwinWe;
          session = session;
          noctalia = noctalia.packages.${system}.default;
        };

        # ---- apps ------------------------------------------------------------
        apps = {
          default = {
            type = "app";
            program = "${session}/bin/start-kineticwe";
            meta.description = "Start a KineticWE Wayland session (kwin-we + noctalia)";
          };
          kwin-we = {
            type = "app";
            program = "${kwinWe}/bin/kinetic-we";
            meta.description = "Run the kwin-we Wayland compositor directly";
          };
        };

        # ---- dev shell -------------------------------------------------------
        devShells = {
          default = pkgs.callPackage ./nix/devshell.nix {
            inherit kwinWe;
            noctalia = noctalia.packages.${system}.default;
          };
        };
      }
    );
}
