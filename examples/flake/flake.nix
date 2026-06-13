# SPDX-License-Identifier: MIT
{
  description = "Example downstream Gizmo image";

  inputs = {
    # This makes the example buildable from this checkout. Replace with
    # github:OWNER/gizmo-nixos when copying the example into another repo.
    gizmo-nixos.url = "path:../..";
  };

  outputs = inputs@{ ... }:
    let
      system = "x86_64-linux";
      gizmoNixos = inputs."gizmo-nixos";
    in
    {
      packages.${system}.default =
        gizmoNixos.lib.${system}.mkGizmoImage {
          modules = [
            ({ pkgs, ... }: {
              imports = [
                gizmoNixos.nixosModules.gizmo-common
                gizmoNixos.nixosModules.gizmo-kiosk
                gizmoNixos.nixosModules.gizmo-minimal-image
              ];

              nixpkgs.overlays = [ gizmoNixos.overlays.default ];

              gizmo.common.enable = true;
              gizmo.common.touchscreen.swapXY = true;
              gizmo.common.touchscreen.invertY = true;
              gizmo.common.leds.animateOnBoot = false;

              gizmo.kiosk.enable = true;
              gizmo.kiosk.url = "http://homeassistant.local:8123/";

              networking.hostName = "gizmo-kiosk";
              networking.useDHCP = true;

              environment.systemPackages = [
                pkgs.gizmo-backlight
                pkgs.gizmo-nfc-tools
                pkgs.gizmo-sensor-tools
              ];

              # Enable SSH explicitly for your site if needed.
              # services.openssh = {
              #   enable = true;
              #   settings = {
              #     PasswordAuthentication = false;
              #     PermitRootLogin = "prohibit-password";
              #   };
              # };
              # users.users.root.openssh.authorizedKeys.keys = [
              #   "ssh-ed25519 AAAA... user@example"
              # ];

              system.stateVersion = "25.11";
            })
          ];
        };
    };
}
