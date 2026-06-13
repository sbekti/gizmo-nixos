# SPDX-License-Identifier: MIT
{
  pkgs,
  kernel ? pkgs.linuxPackages.kernel,
}:

rec {
  watchdog-control = pkgs.callPackage ./watchdog-control { };
  gizmo-backlight = pkgs.callPackage ./gizmo-backlight { };
  gizmo-io-dump = pkgs.callPackage ./gizmo-io-dump { };
  gizmo-led-rainbow = pkgs.callPackage ./gizmo-led-rainbow { };
  gizmo-nfc-tools = pkgs.callPackage ./gizmo-nfc-tools { };
  gizmo-sensor-tools = pkgs.callPackage ./gizmo-sensor-tools { };

  gizmo-cpld-i2c = pkgs.callPackage ./gizmo-cpld-i2c { inherit kernel; };
  gizmo-ft7511 = pkgs.callPackage ./gizmo-ft7511 { inherit kernel; };
  gizmo-pn544 = pkgs.callPackage ./gizmo-pn544 { inherit kernel; };

  gizmo-collector = pkgs.callPackage ./gizmo-collector {
    inherit
      watchdog-control
      gizmo-backlight
      gizmo-io-dump
      gizmo-nfc-tools
      gizmo-sensor-tools
      ;
  };
}
