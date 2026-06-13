# SPDX-License-Identifier: MIT
{
  description = "Repeatable NixOS images for Gizmo hardware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    {
      nixpkgs,
      ...
    }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };

      overlay =
        final: _prev:
        let
          localPackages = import ./pkgs { pkgs = final; };
        in
        {
          inherit (localPackages)
            watchdog-control
            gizmo-backlight
            gizmo-io-dump
            gizmo-led-rainbow
            gizmo-nfc-tools
            gizmo-sensor-tools
            gizmo-collector
            ;
        };

      mkGizmoSystem =
        {
          modules,
          specialArgs ? { },
          system ? "x86_64-linux",
        }:
        nixpkgs.lib.nixosSystem {
          inherit specialArgs;
          modules = [
            { nixpkgs.hostPlatform = system; }
          ] ++ modules;
        };

      mkGizmoImage =
        {
          modules,
          specialArgs ? { },
          system ? "x86_64-linux",
        }:
        let
          imagePkgs = import nixpkgs { inherit system; };
          rawImage = (mkGizmoSystem { inherit modules specialArgs system; }).config.system.build.images.raw;
        in
        imagePkgs.runCommand "gizmo-${rawImage.name}" { } ''
          mkdir -p "$out"
          ln -s "${rawImage}/${rawImage.passthru.filePath}" "$out/nixos.img"
        '';

      localPackages = import ./pkgs { inherit pkgs; };
      rescueBaseImage = mkGizmoImage {
        modules = [ ./nixos/rescue-collector.nix ];
      };
      nixosModules = {
        default = import ./nixos/modules/gizmo-common.nix;
        gizmo-common = import ./nixos/modules/gizmo-common.nix;
        gizmo-collector = import ./nixos/modules/gizmo-collector.nix;
        gizmo-kiosk = import ./nixos/modules/gizmo-kiosk.nix;
        gizmo-minimal-image = import ./nixos/modules/gizmo-minimal-image.nix;
      };
    in
    {
      overlays.default = overlay;

      lib.${system} = {
        inherit mkGizmoImage mkGizmoSystem;
      };

      packages.${system} = localPackages // {
        rescue-collector-image = rescueBaseImage;
        rescue-collector-base-image = rescueBaseImage;
        homeassistant-kiosk-image = mkGizmoImage {
          modules = [ ./nixos/homeassistant-kiosk.nix ];
        };

        default = rescueBaseImage;
      };

      inherit nixosModules;

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          cbfstool
          flashrom
          git-lfs
          ifdtool
          poppler-utils
        ];
      };
    };
}
