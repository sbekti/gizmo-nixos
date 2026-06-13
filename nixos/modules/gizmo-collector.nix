# SPDX-License-Identifier: MIT
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.gizmo.collector;
  gizmoPackages = import ../../pkgs { inherit pkgs; };
in
{
  options.gizmo.collector = {
    enable = lib.mkEnableOption "Gizmo stage 1 rescue collector";

    outputPath = lib.mkOption {
      type = lib.types.str;
      default = "/var/lib/gizmo-collector";
      description = "Path where collector bundles are written.";
    };

    autorun = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Run the collector once automatically after boot.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      gizmoPackages.gizmo-collector
      pkgs.flashrom
      pkgs.cbfstool
      pkgs.ifdtool
      pkgs.dosfstools
      pkgs.alsa-utils
      pkgs.i2c-tools
      pkgs.libgpiod
      pkgs.dmidecode
      pkgs.acpica-tools
    ];

    systemd.tmpfiles.rules = [
      "d ${cfg.outputPath} 0755 root root -"
      "d /var/lib/gizmo-collector 0755 root root -"
    ];

    systemd.services.gizmo-collector = lib.mkIf cfg.autorun {
      description = "Collect Gizmo firmware and hardware bring-up bundle";
      wantedBy = [ "multi-user.target" ];
      after = [
        "network-online.target"
        "gizmo-disable-watchdog.service"
        "local-fs.target"
      ];
      wants = [ "network-online.target" ];
      path = [
        pkgs.util-linux
        pkgs.coreutils
      ];
      script = ''
        mkdir -p ${cfg.outputPath}
        exec ${gizmoPackages.gizmo-collector}/bin/gizmo-collector ${lib.escapeShellArg cfg.outputPath}
      '';
      serviceConfig = {
        Type = "oneshot";
      };
    };
  };
}
