# SPDX-License-Identifier: MIT
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.gizmo.common;
  gizmoPackages = import ../../pkgs {
    inherit pkgs;
    kernel = config.boot.kernelPackages.kernel;
  };
in
{
  options.gizmo.common = {
    enable = lib.mkEnableOption "Gizmo common hardware support";

    disableWatchdogOnBoot = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Disable the CPLD watchdog early during boot.";
    };

    backlight = {
      brightness = lib.mkOption {
        type = lib.types.nullOr (lib.types.ints.between 0 255);
        default = null;
        description = "Optional CPLD backlight brightness byte to write at boot; 0 turns the backlight off.";
      };
    };

    audio = {
      masterVolumePercent = lib.mkOption {
        type = lib.types.ints.between 0 100;
        default = 50;
        description = "Default HDA Master playback volume percentage applied during boot.";
      };
    };

    leds = {
      animateOnBoot = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = "Start the userspace LED animation after resolving the LED mux buses.";
      };
    };

    touchscreen = {
      pollMs = lib.mkOption {
        type = lib.types.ints.positive;
        default = 20;
        description = "FT7511 polling fallback interval in milliseconds if the named CPLD interrupt GPIO cannot be mapped.";
      };

      swapXY = lib.mkOption {
        type = lib.types.bool;
        default = false;
        description = "Swap FT7511 X/Y coordinates.";
      };

      invertX = lib.mkOption {
        type = lib.types.bool;
        default = false;
        description = "Invert FT7511 X coordinates.";
      };

      invertY = lib.mkOption {
        type = lib.types.bool;
        default = false;
        description = "Invert FT7511 Y coordinates.";
      };
    };

    nfc = {
      enable = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = "Load the Gizmo PN547/PN544 NFC misc device driver.";
      };

      bus = lib.mkOption {
        type = lib.types.nullOr lib.types.int;
        default = null;
        description = "Optional right-mux I2C bus override for PN547; null auto-detects from the CPLD mux.";
      };

      address = lib.mkOption {
        type = lib.types.ints.between 0 127;
        default = 40;
        description = "PN547 I2C address on the right-side mux bus.";
      };

      irqGpio = lib.mkOption {
        type = lib.types.nullOr lib.types.int;
        default = null;
        description = "Optional legacy Linux GPIO number for nfc_intr; null uses the named CPLD gizmo-intc nfc_intr line.";
      };

      venGpio = lib.mkOption {
        type = lib.types.nullOr lib.types.int;
        default = null;
        description = "Optional legacy Linux GPIO number for nfc_en/VEN; null uses the named CPLD gizmo-gpio-enables nfc_en line.";
      };

      firmGpio = lib.mkOption {
        type = lib.types.nullOr lib.types.int;
        default = null;
        description = "Optional firmware-download GPIO; null disables firmware-download power mode.";
      };
    };
  };

  config = lib.mkIf cfg.enable {
    # The current images use a fixed clockwise-portrait orientation. Rotate
    # the boot console to match it and disable i915 power-saving paths that
    # caused black flashes and framebuffer jumps on the rotated display.
    # Facebook's stack reportedly used an accelerometer for dynamic orientation;
    # that is not wired up here yet.
    boot.kernelParams = [
      "i915.enable_psr=0"
      "i915.enable_fbc=0"
      "i915.enable_dc=0"
      "module_blacklist=intel-spi,intel-spi-pci,intel-spi-platform,spi_intel,spi_intel_platform"
      "modprobe.blacklist=intel-spi,intel-spi-pci,intel-spi-platform,spi_intel,spi_intel_platform"
      "video=eDP-1"
      "noquiet"
      "fbcon=rotate:1"
      "vt.global_cursor_default=1"
      "console=tty0"
      "console=ttyS0,115200"
    ];

    boot.initrd.availableKernelModules = [
      "ahci"
      "ehci_pci"
      "xhci_pci"
      "sd_mod"
      "usb_storage"
      "usbhid"
      "mmc_block"
      "i915"
    ];

    boot.kernelModules = [
      "i2c-dev"
      "i2c-i801"
      "intel-lpss"
      "intel-lpss-acpi"
      "intel-lpss-pci"
      "i2c-designware-pci"
      "i2c-ocores"
      "gizmo-cpld-i2c"
      "i2c-mux"
      "i2c-mux-gpio"
      "i2c-mux-pca954x"
      "i2c-mux-reg"
      "i2c-hid-acpi"
      "hid-multitouch"
      "edt-ft5x06"
      "snd_hda_intel"
      "snd_usb_audio"
    ];

    # The kernel Intel SPI MTD drivers probe this platform and log -EINVAL.
    # Firmware reads use flashrom's internal programmer instead, so suppress
    # the noisy auto-probe rather than exposing an unused MTD device.
    #
    # The OpenCores-attached IIO sensor drivers are intentionally loaded by
    # gizmo-load-sensors.service after the CPLD bus is present and stable.
    # Letting udev/systemd-modules-load bind them immediately can race the
    # freshly registered polling I2C adapter and produce probe timeouts.
    boot.blacklistedKernelModules = [
      "intel-spi"
      "intel-spi-pci"
      "intel-spi-platform"
      "spi_intel"
      "spi_intel_platform"
      "isl29018"
      "bmc150-accel-i2c"
      "bmc150_magn_i2c"
    ];

    boot.extraModulePackages = [
      gizmoPackages.gizmo-cpld-i2c
      gizmoPackages.gizmo-ft7511
      gizmoPackages.gizmo-pn544
    ];

    boot.supportedFilesystems = [
      "vfat"
    ];

    environment.systemPackages = [
      gizmoPackages.watchdog-control
      gizmoPackages.gizmo-backlight
      gizmoPackages.gizmo-io-dump
      gizmoPackages.gizmo-led-rainbow
      gizmoPackages.gizmo-nfc-tools
      gizmoPackages.gizmo-sensor-tools
      gizmoPackages.gizmo-cpld-i2c
      gizmoPackages.gizmo-ft7511
      gizmoPackages.gizmo-pn544
      pkgs.alsa-utils
      pkgs.evtest
      pkgs.i2c-tools
      pkgs.pciutils
      pkgs.usbutils
      pkgs.flashrom
      pkgs.cbfstool
      pkgs.ifdtool
    ];

    systemd.services.gizmo-disable-watchdog = lib.mkIf cfg.disableWatchdogOnBoot {
      description = "Disable Gizmo CPLD watchdog";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" ];
      before = [ "gizmo-load-sensors.service" "gizmo-load-touchscreen.service" "gizmo-load-leds.service" "gizmo-collector.service" ];
      serviceConfig = {
        Type = "oneshot";
        ExecStart = "${gizmoPackages.watchdog-control}/bin/watchdog-control -d";
        RemainAfterExit = true;
      };
    };

    systemd.services.gizmo-load-sensors = {
      description = "Register Gizmo IIO sensors";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" "gizmo-disable-watchdog.service" ];
      before = [
        "gizmo-load-touchscreen.service"
        "gizmo-load-leds.service"
        "gizmo-load-nfc.service"
        "gizmo-collector.service"
        "gizmo-kiosk.service"
      ];
      path = [
        pkgs.coreutils
        pkgs.gawk
        pkgs.i2c-tools
        pkgs.kmod
      ];
      script = ''
        set -u

        ocores_bus=
        for _ in $(seq 1 30); do
          ocores_bus="$(i2cdetect -l 2>/dev/null | awk 'tolower($0) ~ /i2c-ocores/ { sub(/^i2c-/, "", $1); print $1; exit }')"
          if [ -n "$ocores_bus" ]; then
            break
          fi
          sleep 0.2
        done

        if [ -z "$ocores_bus" ]; then
          echo "gizmo-load-sensors: no i2c-ocores bus found" >&2
          exit 0
        fi

        wait_readable() {
          local addr="$1"
          local name="$2"

          for _ in $(seq 1 10); do
            if i2cget -y "$ocores_bus" "$addr" 0x00 >/dev/null 2>&1; then
              echo "gizmo-load-sensors: $name at $addr responded on i2c-$ocores_bus"
              return 0
            fi
            sleep 0.1
          done

          echo "gizmo-load-sensors: $name at $addr did not respond before driver load" >&2
          return 1
        }

        keep_awake() {
          local dev="$1"
          local path

          for path in \
            "/sys/bus/i2c/devices/$dev/power/control" \
            "/sys/bus/i2c/devices/$dev"/iio:device*/power/control; do
            if [ -e "$path" ]; then
              echo on > "$path" 2>/dev/null || true
            fi
          done
        }

        retry_bind() {
          local driver="$1"
          local dev="$2"

          if [ -d "/sys/bus/i2c/devices/$dev" ] &&
             [ ! -e "/sys/bus/i2c/devices/$dev/driver" ] &&
             [ -e "/sys/bus/i2c/drivers/$driver/bind" ]; then
            echo "$dev" > "/sys/bus/i2c/drivers/$driver/bind" 2>/dev/null || true
          fi
        }

        wait_readable 0x10 bmc150_accel || true
        wait_readable 0x12 bmc150_magn || true
        wait_readable 0x44 isl29023 || true

        modprobe bmc150_accel_i2c || true
        keep_awake "$ocores_bus-0010"
        sleep 0.1

        modprobe bmc150_magn_i2c || true
        keep_awake "$ocores_bus-0012"
        sleep 0.1

        modprobe isl29018 || true
        sleep 0.5
        retry_bind isl29018 "$ocores_bus-0044"

        echo "gizmo-load-sensors: requested bmc150_accel, bmc150_magn, and isl29023 on i2c-$ocores_bus"
      '';
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
    };

    # Current hardware exposes playback through the HDA ALC298 device. The
    # CX20921 USB-Audio interface is present, but the captured descriptors only
    # expose a capture PCM, and ALSA's default Master switch comes up muted.
    # Keep downstream controls open, then use Master as the single default
    # loudness knob so percentage changes are predictable.
    systemd.services.gizmo-prepare-audio = {
      description = "Prepare Gizmo audio playback controls";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" "sound.target" ];
      before = [ "gizmo-collector.service" "gizmo-kiosk.service" "gizmo-startup-audio.service" ];
      path = [
        pkgs.alsa-utils
        pkgs.coreutils
        pkgs.gnugrep
      ];
      script = ''
        set -eu

        for _ in $(seq 1 40); do
          if [ -r /proc/asound/cards ] && grep -q '^[[:space:]]*[0-9].*PCH' /proc/asound/cards && amixer -c PCH info >/dev/null 2>&1; then
            break
          fi
          sleep 0.25
        done

        if ! amixer -c PCH info >/dev/null 2>&1; then
          echo "gizmo-prepare-audio: HDA PCH mixer not available; leaving audio unchanged" >&2
          exit 0
        fi

        if amixer -c PCH scontrols | grep -Fq "Simple mixer control 'Master',"; then
          amixer -q -c PCH sset Master ${toString cfg.audio.masterVolumePercent}% || true
          amixer -q -c PCH sset Master unmute || true
        fi

        for control in Speaker PCM "Line Out"; do
          if amixer -c PCH scontrols | grep -Fq "Simple mixer control '$control',"; then
            amixer -q -c PCH sset "$control" 100% || true
            amixer -q -c PCH sset "$control" unmute || true
          fi
        done

        amixer -c PCH sget Master || true
      '';
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
    };

    systemd.services.gizmo-set-backlight = lib.mkIf (cfg.backlight.brightness != null) {
      description = "Set Gizmo CPLD backlight brightness";
      wantedBy = [ "multi-user.target" ];
      after = [ "gizmo-disable-watchdog.service" ];
      before = [ "gizmo-collector.service" "gizmo-kiosk.service" ];
      serviceConfig = {
        Type = "oneshot";
        ExecStart = "${gizmoPackages.gizmo-backlight}/bin/gizmo-backlight -s ${toString cfg.backlight.brightness}";
        RemainAfterExit = true;
      };
    };

    # The FT7511 is reachable at 0x38 on the CPLD OpenCores bus. The driver
    # uses named CPLD reset/interrupt GPIOs and keeps polling available as a
    # fallback when the interrupt domain is not available.
    systemd.services.gizmo-load-touchscreen = {
      description = "Register Gizmo FT7511 touchscreen";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" "gizmo-disable-watchdog.service" ];
      before = [ "gizmo-collector.service" ];
      path = [
        pkgs.coreutils
        pkgs.gawk
        pkgs.i2c-tools
        pkgs.kmod
      ];
      script = ''
        set -eu

        ocores_bus=
        for _ in $(seq 1 20); do
          ocores_bus="$(i2cdetect -l 2>/dev/null | awk 'tolower($0) ~ /i2c-ocores/ { sub(/^i2c-/, "", $1); print $1; exit }')"
          if [ -n "$ocores_bus" ] && i2cget -y "$ocores_bus" 0x38 0x00 >/dev/null 2>&1; then
            break
          fi
          sleep 0.5
        done

        if [ -z "$ocores_bus" ]; then
          echo "gizmo-load-touchscreen: no i2c-ocores bus found" >&2
          exit 1
        fi

        if ! i2cget -y "$ocores_bus" 0x38 0x00 >/dev/null 2>&1; then
          echo "gizmo-load-touchscreen: FT7511 at 0x38 not detected on i2c-$ocores_bus" >&2
          exit 1
        fi

        modprobe gizmo-ft7511 \
          bus="$ocores_bus" \
          poll_ms=${toString cfg.touchscreen.pollMs} \
          swap_xy=${if cfg.touchscreen.swapXY then "1" else "0"} \
          invert_x=${if cfg.touchscreen.invertX then "1" else "0"} \
          invert_y=${if cfg.touchscreen.invertY then "1" else "0"}
        echo "gizmo-load-touchscreen: loaded gizmo-ft7511 on i2c-$ocores_bus using gizmo-intc:touchpanel_intr with poll_ms fallback ${toString cfg.touchscreen.pollMs}"
      '';
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
    };

    # Resolve the mux topology here for logs and ordering. LED animation uses a
    # direct userspace I2C helper because the LP55231 kernel LED class driver
    # still intermittently times out during post-init on this bus.
    systemd.services.gizmo-load-leds = {
      description = "Resolve Gizmo LED mux buses";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" "gizmo-disable-watchdog.service" ];
      before = [ "gizmo-collector.service" ];
      path = [
        pkgs.coreutils
        pkgs.gawk
        pkgs.gnugrep
        pkgs.i2c-tools
      ];
      script = ''
        set -eu

        ocores_bus=
        chan0_bus=
        chan1_bus=

        for _ in $(seq 1 20); do
          ocores_bus="$(i2cdetect -l 2>/dev/null | awk 'tolower($0) ~ /i2c-ocores/ { sub(/^i2c-/, "", $1); print $1; exit }')"
          if [ -n "$ocores_bus" ]; then
            chan0_bus="$(i2cdetect -l 2>/dev/null | awk -v parent="i2c-''${ocores_bus}-mux" '$0 ~ parent && $0 ~ /chan_id 0/ { sub(/^i2c-/, "", $1); print $1; exit }')"
            chan1_bus="$(i2cdetect -l 2>/dev/null | awk -v parent="i2c-''${ocores_bus}-mux" '$0 ~ parent && $0 ~ /chan_id 1/ { sub(/^i2c-/, "", $1); print $1; exit }')"
            if [ -n "$chan0_bus" ] && [ -n "$chan1_bus" ]; then
              break
            fi
          fi
          sleep 0.5
        done

        if [ -z "$chan0_bus" ] || [ -z "$chan1_bus" ]; then
          echo "gizmo-load-leds: mux buses not available for ocores bus ''${ocores_bus:-unknown}" >&2
          exit 1
        fi

        if i2cget -y "$chan1_bus" 0x28 0x00 >/dev/null 2>&1; then
          right_bus="$chan1_bus"
          left_bus="$chan0_bus"
        elif i2cget -y "$chan0_bus" 0x28 0x00 >/dev/null 2>&1; then
          right_bus="$chan0_bus"
          left_bus="$chan1_bus"
        else
          echo "gizmo-load-leds: PN547 0x28 not detected; defaulting chan0 left, chan1 right" >&2
          left_bus="$chan0_bus"
          right_bus="$chan1_bus"
        fi

        echo "gizmo-load-leds: LED mux ready, left_bus=$left_bus right_bus=$right_bus"
      '';
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
    };

    systemd.services.gizmo-load-nfc = lib.mkIf cfg.nfc.enable {
      description = "Register Gizmo PN547 NFC controller";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" "gizmo-disable-watchdog.service" "gizmo-load-leds.service" ];
      wants = [ "gizmo-load-leds.service" ];
      before = [ "gizmo-collector.service" "gizmo-kiosk.service" ];
      path = [
        pkgs.coreutils
        pkgs.gawk
        pkgs.gnugrep
        pkgs.i2c-tools
        pkgs.kmod
      ];
      script =
        let
          gpioParam = name: value:
            if value == null then "${name}=-1" else "${name}=${toString value}";
          address = toString cfg.nfc.address;
          irqSource =
            if cfg.nfc.irqGpio == null then
              "gizmo-intc:nfc_intr"
            else
              "legacy-gpio-${toString cfg.nfc.irqGpio}";
          venSource =
            if cfg.nfc.venGpio == null then
              "gizmo-gpio-enables:nfc_en"
            else
              "legacy-gpio-${toString cfg.nfc.venGpio}";
        in
        ''
          set -eu

          nfc_bus=${if cfg.nfc.bus == null then "" else toString cfg.nfc.bus}
          ocores_bus=
          chan0_bus=
          chan1_bus=

          if [ -z "$nfc_bus" ]; then
            for _ in $(seq 1 20); do
              ocores_bus="$(i2cdetect -l 2>/dev/null | awk 'tolower($0) ~ /i2c-ocores/ { sub(/^i2c-/, "", $1); print $1; exit }')"
              if [ -n "$ocores_bus" ]; then
                chan0_bus="$(i2cdetect -l 2>/dev/null | awk -v parent="i2c-''${ocores_bus}-mux" '$0 ~ parent && $0 ~ /chan_id 0/ { sub(/^i2c-/, "", $1); print $1; exit }')"
                chan1_bus="$(i2cdetect -l 2>/dev/null | awk -v parent="i2c-''${ocores_bus}-mux" '$0 ~ parent && $0 ~ /chan_id 1/ { sub(/^i2c-/, "", $1); print $1; exit }')"
                if [ -n "$chan0_bus" ] && [ -n "$chan1_bus" ]; then
                  break
                fi
              fi
              sleep 0.5
            done

            if [ -z "$chan0_bus" ] || [ -z "$chan1_bus" ]; then
              echo "gizmo-load-nfc: mux buses not available for ocores bus ''${ocores_bus:-unknown}" >&2
              exit 1
            fi

            if i2cget -y "$chan1_bus" ${address} 0x00 >/dev/null 2>&1; then
              nfc_bus="$chan1_bus"
            elif i2cget -y "$chan0_bus" ${address} 0x00 >/dev/null 2>&1; then
              nfc_bus="$chan0_bus"
            else
              echo "gizmo-load-nfc: PN547 ${address} did not ACK on either mux channel; defaulting to chan1 as right side" >&2
              nfc_bus="$chan1_bus"
            fi
          fi

          modprobe gizmo-pn544 \
            bus="$nfc_bus" \
            address=${address} \
            ${gpioParam "irq_gpio" cfg.nfc.irqGpio} \
            ${gpioParam "ven_gpio" cfg.nfc.venGpio} \
            ${gpioParam "firm_gpio" cfg.nfc.firmGpio}

          echo "gizmo-load-nfc: loaded gizmo-pn544 on i2c-$nfc_bus addr=${address} irq=${irqSource} ven=${venSource} firm_gpio=${toString (if cfg.nfc.firmGpio == null then -1 else cfg.nfc.firmGpio)}"
        '';
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
    };

    systemd.services.gizmo-led-rainbow = lib.mkIf cfg.leds.animateOnBoot {
      description = "Animate Gizmo LEDs for visual bring-up feedback";
      wantedBy = [ "multi-user.target" ];
      after = [ "gizmo-load-leds.service" ];
      wants = [ "gizmo-load-leds.service" ];
      environment = {
        MAX_BRIGHTNESS = "48";
        COMET_LENGTH = "11";
        FRAME_DELAY_US = "50000";
      };
      serviceConfig = {
        Type = "simple";
        ExecStart = "${gizmoPackages.gizmo-led-rainbow}/bin/gizmo-led-rainbow";
        Restart = "on-failure";
        RestartSec = "2s";
      };
    };
  };
}
