# SPDX-License-Identifier: MIT
{
  config,
  lib,
  pkgs,
  ...
}:

let
  collectDebugData = pkgs.writeShellScript "collect-debug-data.sh" ''
    set -euo pipefail

    mkdir -p /var/lib/gizmo-collector
    exec gizmo-collector /var/lib/gizmo-collector
  '';

  jauntyStartupMp3 = pkgs.runCommand "gizmo-rescue-startup-jingle.mp3" {
    nativeBuildInputs = [
      pkgs.lame
      pkgs.python3
    ];
  } ''
    python3 ${pkgs.writeText "make-gizmo-jingle.py" ''
      import math
      import struct
      import wave

      rate = 44100
      notes = [
          (523.25, 0.14), (659.25, 0.14), (783.99, 0.14),
          (1046.50, 0.22), (783.99, 0.12), (987.77, 0.24),
          (880.00, 0.16), (1046.50, 0.32),
      ]

      samples = []
      for freq, seconds in notes:
          count = int(rate * seconds)
          for i in range(count):
              t = i / rate
              env = min(1.0, i / (rate * 0.015), (count - i) / (rate * 0.035))
              tone = math.sin(2 * math.pi * freq * t)
              overtone = 0.35 * math.sin(2 * math.pi * freq * 2 * t)
              samples.append(int(18000 * env * (tone + overtone) / 1.35))
          samples.extend([0] * int(rate * 0.025))

      with wave.open("jingle.wav", "wb") as wav:
          wav.setnchannels(1)
          wav.setsampwidth(2)
          wav.setframerate(rate)
          wav.writeframes(b"".join(struct.pack("<h", s) for s in samples))
    ''}
    lame --quiet jingle.wav "$out"
  '';

  rescueReadme = pkgs.writeText "gizmo-rescue-readme.txt" ''
    Gizmo Rescue Collector
    ======================

    Collector output directory:
      /var/lib/gizmo-collector

    On boot, the rescue image grows its single ext4 root partition to fill
    the USB drive, then stores collector output under:
      /var/lib/gizmo-collector/gizmo-collector-<timestamp>/
      /var/lib/gizmo-collector/gizmo-collector-<timestamp>.tar.zst

    Check current collector status:
      systemctl status gizmo-collector --no-pager
      journalctl -u gizmo-collector -b --no-pager
      journalctl -u gizmo-startup-audio -b --no-pager

    Rerun collection manually:
      /root/collect-debug-data.sh

    Check disks and mounts:
      lsblk -f
      findmnt /
      systemctl status growpart --no-pager

    Watch live sensors:
      gizmo-list-sensors
      gizmo-watch-accelerometer
      gizmo-watch-magnetometer
      gizmo-watch-ambient-light

    Test NFC:
      systemctl status gizmo-load-nfc --no-pager
      gizmo-test-nfc
  '';
in
{
  imports = [
    ./modules/gizmo-common.nix
    ./modules/gizmo-collector.nix
    ./modules/gizmo-minimal-image.nix
  ];

  gizmo.common.enable = true;
  gizmo.collector.enable = true;

  boot.growPartition = true;
  fileSystems."/".autoResize = true;

  networking.hostName = "gizmo-rescue";
  networking.useDHCP = lib.mkDefault true;

  # Physical-console autologin keeps the rescue image usable without network
  # credentials. Enable SSH from a downstream config if remote access is needed.
  services.getty.autologinUser = "root";

  environment.etc."motd".text = ''
    Gizmo Rescue Collector
    ----------------------
    Output path:    /var/lib/gizmo-collector
    Instructions:   /root/README-GIZMO-RESCUE.txt
    Rerun collect:  /root/collect-debug-data.sh
    Grow logs:      journalctl -u growpart -b --no-pager
    Service logs:   journalctl -u gizmo-collector -b --no-pager
    Audio logs:     journalctl -u gizmo-startup-audio -b --no-pager
  '';

  environment.systemPackages = [
    pkgs.mpg123
  ];

  systemd.services.gizmo-startup-audio = {
    description = "Play Gizmo rescue startup audio";
    wantedBy = [ "multi-user.target" ];
    after = [ "systemd-modules-load.service" "sound.target" "gizmo-prepare-audio.service" ];
    before = [ "gizmo-collector.service" ];
    path = [
      pkgs.coreutils
      pkgs.gnugrep
      pkgs.mpg123
    ];
    script = ''
      set -eu

      for _ in $(seq 1 20); do
        if [ -s /proc/asound/cards ] && grep -q '^[[:space:]]*[0-9]' /proc/asound/cards; then
          break
        fi
        sleep 0.5
      done

      mpg123 -q -o alsa -a default:CARD=PCH ${jauntyStartupMp3}
    '';
    serviceConfig = {
      Type = "oneshot";
    };
  };

  environment.etc."gizmo-rescue/README.txt".source = rescueReadme;
  environment.etc."gizmo-rescue/collect-debug-data.sh".source = collectDebugData;

  system.activationScripts.gizmoRootInstructions = ''
    mkdir -p /root
    ln -sfn /etc/gizmo-rescue/README.txt /root/README-GIZMO-RESCUE.txt
    ln -sfn /etc/gizmo-rescue/collect-debug-data.sh /root/collect-debug-data.sh
  '';

  environment.etc."gizmo-stock-boot/loader.conf".text = ''
    timeout 10
    default gizmo-rescue
  '';

  environment.etc."gizmo-stock-boot/entries/gizmo-rescue.conf".text = ''
    title Gizmo Rescue Collector
    linux /kernel
    initrd /initrd
    options root=LABEL=nixos ro i915.modeset=1 i915.enable_psr=0 i915.enable_fbc=0 i915.enable_dc=0 module_blacklist=intel-spi,intel-spi-pci,intel-spi-platform,spi_intel,spi_intel_platform modprobe.blacklist=intel-spi,intel-spi-pci,intel-spi-platform,spi_intel,spi_intel_platform video=eDP-1 noquiet fbcon=rotate:1 vt.global_cursor_default=1 console=tty0 console=ttyS0,115200
  '';

  system.stateVersion = "25.11";
}
