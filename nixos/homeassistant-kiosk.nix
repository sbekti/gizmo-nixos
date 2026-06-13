# SPDX-License-Identifier: MIT
{
  lib,
  ...
}:

{
  imports = [
    ./modules/gizmo-common.nix
    ./modules/gizmo-kiosk.nix
    ./modules/gizmo-minimal-image.nix
  ];

  gizmo.common.enable = true;
  # This profile fixes the UI at 90 degrees clockwise. Apply the matching
  # transform in the FT7511 driver so X sees already-rotated coordinates.
  # Dynamic accelerometer-based orientation is future work.
  gizmo.common.touchscreen.swapXY = true;
  gizmo.common.touchscreen.invertY = true;
  gizmo.common.leds.animateOnBoot = false;
  gizmo.kiosk.enable = true;
  gizmo.kiosk.url = lib.mkDefault "http://homeassistant.local:8123/";

  networking.hostName = "gizmo-kiosk";
  networking.useDHCP = lib.mkDefault true;

  environment.etc."motd".text = ''
    Gizmo Chromium Kiosk
    --------------------
    Kiosk service: systemctl status gizmo-kiosk --no-pager
    Kiosk logs:    journalctl -u gizmo-kiosk -b --no-pager
    Touch logs:    journalctl -u gizmo-load-touchscreen -b --no-pager
    LED setup:     journalctl -u gizmo-load-leds -b --no-pager

    Set gizmo.kiosk.url from a downstream configuration for your dashboard.
  '';

  system.stateVersion = "25.11";
}
