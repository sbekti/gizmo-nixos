#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

OUTPUT_ROOT="${1:-/var/lib/gizmo-collector}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$OUTPUT_ROOT/gizmo-collector-$STAMP"
RAW="$OUT/raw"
SUMMARY="$OUT/redacted-summary.txt"
LOG="$OUT/collector.log"

mkdir -p "$RAW" "$RAW/firmware" "$RAW/sysfs" "$RAW/acpi" "$RAW/display" "$RAW/audio" "$RAW/network" "$RAW/leds" "$RAW/input" "$RAW/iio" "$RAW/i2c" "$RAW/touchscreen" "$RAW/nfc"
exec > >(tee -a "$LOG") 2>&1

echo "Gizmo collector started at $STAMP"
echo "Output: $OUT"

run_capture() {
  local name="$1"
  shift
  echo "== $name =="
  if "$@" >"$RAW/$name.txt" 2>&1; then
    echo "ok: $name"
  else
    echo "failed: $name"
  fi
}

run_timeout_capture() {
  local name="$1"
  local duration="$2"
  local status

  shift 2
  echo "== $name =="
  set +e
  timeout "$duration" "$@" >"$RAW/$name.txt" 2>&1
  status=$?
  set -e

  if [ "$status" -eq 0 ] || [ "$status" -eq 124 ]; then
    echo "ok: $name"
  else
    echo "failed: $name"
  fi
}

copy_if_exists() {
  local src="$1"
  local dst="$2"
  if [ -e "$src" ]; then
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst" 2>/dev/null || true
  fi
}

collect_iio_attributes() {
  local dev out target f

  echo "== iio-attributes =="
  mkdir -p "$RAW/iio/devices"

  for dev in /sys/bus/iio/devices/iio:device*; do
    [ -e "$dev" ] || continue

    out="$RAW/iio/devices/$(basename "$dev")"
    mkdir -p "$out"
    target="$(readlink -f "$dev" 2>/dev/null || true)"
    printf '%s\n' "$target" >"$out/device-path.txt"

    for f in \
      "$dev"/name \
      "$dev"/in_* \
      "$dev"/sampling_frequency \
      "$dev"/sampling_frequency_available \
      "$dev"/integration_time* \
      "$dev"/scale_available \
      "$dev"/power/control \
      "$dev"/power/runtime_status; do
      [ -f "$f" ] || continue
      cp "$f" "$out/$(basename "$f")" 2>/dev/null || true
    done
  done

  echo "ok: iio-attributes"
}

write_sha512() {
  local path="$1"
  (cd "$(dirname "$path")" && sha512sum "$(basename "$path")" >"$(basename "$path").sha512")
}

disable_watchdog() {
  echo "== watchdog-disable =="
  if watchdog-control -d; then
    echo "watchdog disabled with watchdog-control"
  else
    echo "watchdog-control failed; attempting direct ISA write with isaset is intentionally not implemented here"
  fi
}

collect_firmware() {
  run_capture firmware-cpld-registers gizmo-io-dump 0x0280 0x40
  run_capture firmware-flashrom-probe flashrom -p internal:laptop=this_is_not_a_laptop

  local rom="$RAW/firmware/internal-active.rom"

  echo "== firmware-read-active =="
  if flashrom -p internal:laptop=this_is_not_a_laptop -r "$rom"; then
    write_sha512 "$rom"
    ifdtool -d "$rom" >"$rom.ifdtool.txt" 2>&1 || true
    ifdtool -f "$rom.layout" "$rom" >"$rom.ifdtool-layout.txt" 2>&1 || true
    cbfstool "$rom" print >"$rom.cbfstool.txt" 2>&1 || true
    echo "firmware read: $rom"
  else
    echo "firmware read failed"
  fi
}

collect_emmc() {
  echo "== emmc-backup =="
  mkdir -p "$RAW/emmc"
  lsblk -o NAME,KNAME,PATH,SIZE,TYPE,FSTYPE,MOUNTPOINTS >"$RAW/emmc/lsblk.txt" 2>&1 || true

  local dev=""
  for candidate in /dev/mmcblk0 /dev/mmcblk1; do
    if [ -b "$candidate" ]; then
      dev="$candidate"
      break
    fi
  done

  if [ -z "$dev" ]; then
    echo "no mmcblk device found"
    return 0
  fi

  echo "found eMMC candidate: $dev"
  echo "$dev" >"$RAW/emmc/device.txt"
  blockdev --getsize64 "$dev" >"$RAW/emmc/device-size-bytes.txt" 2>&1 || true
  fdisk -l "$dev" >"$RAW/emmc/fdisk.txt" 2>&1 || true
  sfdisk -d "$dev" >"$RAW/emmc/sfdisk-dump.txt" 2>&1 || true
  parted -s "$dev" unit s print >"$RAW/emmc/parted-sectors.txt" 2>&1 || true
  blkid -p "$dev" >"$RAW/emmc/device-blkid-probe.txt" 2>&1 || true
  file -s "$dev" >"$RAW/emmc/device-file.txt" 2>&1 || true

  echo "capturing first 64 MiB of $dev"
  dd if="$dev" of="$RAW/emmc/$(basename "$dev").head64M.img" bs=1M count=64 status=none || true
  write_sha512 "$RAW/emmc/$(basename "$dev").head64M.img"

  for bootdev in "$dev"boot0 "$dev"boot1; do
    [ -b "$bootdev" ] || continue
    local bootbase
    bootbase="$(basename "$bootdev")"
    echo "capturing $bootdev"
    dd if="$bootdev" of="$RAW/emmc/$bootbase.img" bs=1M status=none || true
    write_sha512 "$RAW/emmc/$bootbase.img"
  done

  local available_mib dev_mib captured_full_device
  available_mib="$(df -Pm "$OUTPUT_ROOT" | awk 'NR == 2 { print $4 }')"
  dev_mib="$(( ($(blockdev --getsize64 "$dev" 2>/dev/null || echo 0) + 1048575) / 1048576 ))"
  captured_full_device=0
  if [ "$dev_mib" -gt 0 ] && [ "$available_mib" -gt $((dev_mib + 512)) ]; then
    echo "capturing compressed raw image for $dev"
    dd if="$dev" bs=4M status=progress | zstd -T0 -19 -o "$RAW/emmc/$(basename "$dev").img.zst"
    write_sha512 "$RAW/emmc/$(basename "$dev").img.zst"
    captured_full_device=1
  else
    echo "skipping full raw image for $dev; need about $((dev_mib + 512)) MiB free, have ${available_mib:-0} MiB"
  fi

  for part in "$dev"p*; do
    [ -b "$part" ] || continue
    local base mountpoint
    base="$(basename "$part")"
    blockdev --getsize64 "$part" >"$RAW/emmc/$base.size-bytes.txt" 2>&1 || true
    blkid -p "$part" >"$RAW/emmc/$base.blkid-probe.txt" 2>&1 || true
    file -s "$part" >"$RAW/emmc/$base.file.txt" 2>&1 || true
    mountpoint="$RAW/emmc/mount-$base"
    mkdir -p "$mountpoint"
    if mount -o ro "$part" "$mountpoint"; then
      tar --xattrs --acls --one-file-system -C "$mountpoint" -cpf "$RAW/emmc/$base.tar" .
      write_sha512 "$RAW/emmc/$base.tar"
      umount "$mountpoint"
      rmdir "$mountpoint"
      echo "archived $part"
    else
      echo "could not mount $part read-only"
      rmdir "$mountpoint" || true
    fi

    local available_mib part_mib
    if [ "$captured_full_device" -eq 1 ]; then
      echo "skipping full raw image for $part; already captured full $dev"
      continue
    fi
    available_mib="$(df -Pm "$OUTPUT_ROOT" | awk 'NR == 2 { print $4 }')"
    part_mib="$(( ($(blockdev --getsize64 "$part" 2>/dev/null || echo 0) + 1048575) / 1048576 ))"
    if [ "$part_mib" -gt 0 ] && [ "$available_mib" -gt $((part_mib + 512)) ]; then
      echo "capturing compressed raw image for $part"
      dd if="$part" bs=4M status=progress | zstd -T0 -19 -o "$RAW/emmc/$base.img.zst"
      write_sha512 "$RAW/emmc/$base.img.zst"
    else
      echo "skipping full raw image for $part; need about $((part_mib + 512)) MiB free, have ${available_mib:-0} MiB"
    fi
  done
}

collect_platform() {
  run_capture kernel-cmdline cat /proc/cmdline
  run_capture proc-interrupts cat /proc/interrupts
  run_capture proc-stat cat /proc/stat
  run_capture proc-ioports cat /proc/ioports
  run_capture proc-iomem cat /proc/iomem
  run_capture proc-bus-input-devices cat /proc/bus/input/devices
  run_capture dmesg dmesg
  run_capture journalctl-b journalctl -b --no-pager
  run_capture systemctl-failed systemctl --failed --no-pager
  run_capture gizmo-startup-audio-status systemctl status gizmo-startup-audio --no-pager
  run_capture gizmo-startup-audio-journal journalctl -u gizmo-startup-audio -b --no-pager
  run_capture gizmo-load-sensors-status systemctl status gizmo-load-sensors --no-pager
  run_capture gizmo-load-sensors-journal journalctl -u gizmo-load-sensors -b --no-pager
  run_capture gizmo-load-nfc-status systemctl status gizmo-load-nfc --no-pager
  run_capture gizmo-load-nfc-journal journalctl -u gizmo-load-nfc -b --no-pager
  run_capture nfc/gizmo-test-nfc-reset gizmo-test-nfc --nci-reset
  run_capture nfc/gizmo-test-nfc-discover-5s gizmo-test-nfc --nci-discover --seconds 5
  run_capture modules lsmod
  run_capture udevadm-export-db udevadm info --export-db
  run_capture lspci lspci -nnvv
  run_capture lsusb lsusb -v
  run_capture lsblk lsblk -O
  run_capture blkid blkid
  run_capture findmnt findmnt
  run_capture cpld-backlight gizmo-backlight -q
  run_capture df-h df -h
  run_capture mounts cat /proc/mounts
  run_capture ip-address ip address
  run_capture ip-route ip route
  run_capture iio/gizmo-list-sensors gizmo-list-sensors
  run_timeout_capture iio/gizmo-watch-accelerometer-2s 2s gizmo-watch-accelerometer 0.2
  run_timeout_capture iio/gizmo-watch-magnetometer-2s 2s gizmo-watch-magnetometer 0.2
  run_timeout_capture iio/gizmo-watch-ambient-light-2s 2s gizmo-watch-ambient-light 0.5
  echo "== ethtool-interfaces =="
  {
    for iface in /sys/class/net/*; do
      name="$(basename "$iface")"
      echo "## $name"
      ethtool "$name" || true
    done
  } >"$RAW/ethtool-interfaces.txt" 2>&1
  echo "ok: ethtool-interfaces"
  run_capture dmidecode dmidecode
  run_capture acpidump acpidump
  run_capture acpi-tables-find find /sys/firmware/acpi/tables -maxdepth 2 -type f -print

  copy_if_exists /sys/class/drm "$RAW/display/drm"
  copy_if_exists /sys/class/graphics "$RAW/display/graphics"
  copy_if_exists /sys/class/backlight "$RAW/display/backlight"
  copy_if_exists /sys/class/sound "$RAW/audio/sound-sysfs"
  copy_if_exists /sys/module/gizmo_pn544 "$RAW/nfc/module-gizmo-pn544"
  copy_if_exists /dev/pn544 "$RAW/nfc/dev-pn544"
  copy_if_exists /proc/asound "$RAW/audio/proc-asound"
  copy_if_exists /sys/class/input "$RAW/input/class-input"
  copy_if_exists /sys/bus/input/devices "$RAW/input/bus-input-devices"
  copy_if_exists /sys/bus/acpi/devices "$RAW/acpi/devices"
  copy_if_exists /sys/bus/iio/devices "$RAW/iio/bus-iio-devices"
  collect_iio_attributes
  copy_if_exists /sys/bus/hid/devices "$RAW/sysfs/hid-devices"
  copy_if_exists /sys/class/leds "$RAW/leds/class-leds"
  copy_if_exists /sys/class/gpio "$RAW/leds/class-gpio"
  copy_if_exists /sys/kernel/debug/gpio "$RAW/leds/debug-gpio"
  copy_if_exists /sys/kernel/debug/irq "$RAW/sysfs/debug-irq"
  copy_if_exists /sys/kernel/debug/pinctrl "$RAW/leds/debug-pinctrl"
  copy_if_exists /sys/bus/i2c/devices "$RAW/sysfs/i2c-devices"
  copy_if_exists /sys/class/i2c-dev "$RAW/sysfs/i2c-dev"
  copy_if_exists /sys/bus/platform/devices "$RAW/sysfs/platform-devices"
}

collect_audio() {
  run_capture aplay-l aplay -l
  run_capture aplay-L aplay -L
  run_capture amixer amixer
  run_capture amixer-card-pch amixer -c PCH
  run_capture amixer-card-cx20921 amixer -c CX20921
  run_capture speaker-test-pch speaker-test -D default:CARD=PCH -t sine -f 1000 -l 1
}

collect_i2c_leds() {
  run_capture i2cdetect-l i2cdetect -l
  run_capture i2c-relevant-modinfo modinfo gizmo-cpld-i2c gizmo-ft7511 gizmo-pn544 intel-lpss intel-lpss-acpi intel-lpss-pci i2c-designware-pci i2c-ocores i2c-mux i2c-mux-gpio i2c-mux-pca954x i2c-mux-reg i2c-hid-acpi hid-multitouch edt-ft5x06 focaltech isl29018 bmc150-accel-core bmc150-accel-i2c bmc150_magn bmc150_magn_i2c leds-lp5523 pn544 nxp-nci nxp-nci_i2c nfc nci
  run_capture cpld-registers-pre-i2c-probe gizmo-io-dump 0x0280 0x40
  run_capture ocores-registers-pre-i2c-probe gizmo-io-dump 0x02b8 0x08

  local mux_buses
  mux_buses="$(i2cdetect -l 2>/dev/null | awk 'tolower($0) ~ /-mux/ { sub(/^i2c-/, "", $1); print $1 }')"

  for bus in $mux_buses; do
    run_capture "i2cdetect-bus-$bus" i2cdetect -y "$bus"
    for addr in 0x32 0x33 0x34 0x35; do
      run_capture "lp55231-bus-$bus-addr-$addr" i2cdump -y "$bus" "$addr"
    done
  done

  echo "== i2c-ocores-device-probe =="
  {
    echo "Known Gizmo I2C devices:"
    echo "  CPLD signature 0xface at I/O ports 0x280/0x281"
    echo "  OpenCores I2C controller ocores-92c at I/O 0x2b8 size 0x08, irq 8"
    echo "  CPLD GPIO chips: gizmo-gpio-enables, gizmo-gpio-resets, gizmo-gpgpio, gizmo-intc"
    echo "  NFC power is gizmo-gpio-enables:nfc_en; NFC IRQ is gizmo-intc:nfc_intr"
    echo "  0x10 bmc150_accel accelerometer"
    echo "  0x12 bmc150_magn magnetometer"
    echo "  0x28 pn547 NFC on right mux channel; /dev/pn544 via gizmo-pn544"
    echo "  0x38 ft7511 touchscreen: max touches 10, active report period 100 Hz, max x 1200, max y 1920"
    echo "  0x44 isl29023 ambient light sensor"
    echo "  0x72 pca9543 mux"
    echo "  0x41 CX20921 audio codec control interface; audio also appears as USB audio"
    echo
    i2cdetect -l
  } >"$RAW/i2c/known-devices.txt" 2>&1 || true

  local ocores_buses known_probe_buses
  ocores_buses="$(i2cdetect -l 2>/dev/null | awk 'tolower($0) ~ /i2c-ocores/ { sub(/^i2c-/, "", $1); print $1 }')"
  if [ -z "$ocores_buses" ]; then
    echo "no i2c-ocores adapters exposed" >>"$RAW/i2c/known-devices.txt"
  fi

  known_probe_buses="$(i2cdetect -l 2>/dev/null | awk '
    tolower($0) !~ /i915|gmbus|aux/ {
      sub(/^i2c-/, "", $1)
      print $1
    }
  ')"

  for bus in $known_probe_buses; do
    run_capture "i2c/known-bus-$bus-scan" i2cdetect -y "$bus"
    for addr in 0x10 0x12 0x28 0x29 0x32 0x33 0x34 0x35 0x38 0x41 0x44 0x72; do
      run_capture "i2c/known-bus-$bus-addr-$addr-byte" i2cget -y "$bus" "$addr" 0x00
    done
  done

  for bus in $ocores_buses; do
    run_capture "i2c/ocores-bus-$bus-scan" i2cdetect -y "$bus"
    run_capture "i2c/ocores-bus-$bus-registers-after-scan" gizmo-io-dump 0x02b8 0x08
    for addr in 0x10 0x12 0x38 0x41 0x44 0x72; do
      run_capture "i2c/ocores-bus-$bus-addr-$addr-byte" i2cget -y "$bus" "$addr" 0x00
      run_capture "i2c/ocores-bus-$bus-registers-after-addr-$addr" gizmo-io-dump 0x02b8 0x08
    done
    run_capture "i2c/bmc150-accel-bus-$bus-addr-0x10-head" i2cdump -y -r 0x00-0x3f "$bus" 0x10
    run_capture "i2c/bmc150-magn-bus-$bus-addr-0x12-head" i2cdump -y -r 0x00-0x3f "$bus" 0x12
    run_capture "i2c/ft7511-bus-$bus-addr-0x38-head" i2cdump -y -r 0x00-0x20 "$bus" 0x38
    run_capture "touchscreen/ft7511-bus-$bus-reg-0x00" i2cget -y "$bus" 0x38 0x00
    run_capture "touchscreen/ft7511-bus-$bus-threshold-group-0x80" i2cget -y "$bus" 0x38 0x80
    run_capture "touchscreen/ft7511-bus-$bus-active-period-0x88" i2cget -y "$bus" 0x38 0x88
    run_capture "touchscreen/ft7511-bus-$bus-lib-version-high-0xa1" i2cget -y "$bus" 0x38 0xa1
    run_capture "touchscreen/ft7511-bus-$bus-lib-version-low-0xa2" i2cget -y "$bus" 0x38 0xa2
    run_capture "touchscreen/ft7511-bus-$bus-cipher-vendor-id-0xa3" i2cget -y "$bus" 0x38 0xa3
    run_capture "touchscreen/ft7511-bus-$bus-firmware-id-0xa6" i2cget -y "$bus" 0x38 0xa6
    run_capture "touchscreen/ft7511-bus-$bus-focaltech-id-0xa8" i2cget -y "$bus" 0x38 0xa8
    run_capture "touchscreen/ft7511-bus-$bus-release-code-id-0xaf" i2cget -y "$bus" 0x38 0xaf
    run_capture "touchscreen/ft7511-bus-$bus-identity-0x80-0xaf" i2cdump -y -r 0x80-0xaf "$bus" 0x38
    run_capture "i2c/isl29023-bus-$bus-addr-0x44-head" i2cdump -y -r 0x00-0x0f "$bus" 0x44
    run_capture "i2c/pca9543-bus-$bus-addr-0x72-head" i2cdump -y -r 0x00-0x03 "$bus" 0x72
  done

  echo "== sensor-topology =="
  {
    echo "Gizmo IIO sensor notes"
    echo
    echo "All are on the CPLD OpenCores I2C adapter:"
    echo "  0x10 bmc150_accel accelerometer"
    echo "  0x12 bmc150_magn magnetometer"
    echo "  0x44 isl29023 ambient light sensor"
    echo
    echo "Expected kernel modules:"
    echo "  bmc150-accel-i2c"
    echo "  bmc150_magn_i2c"
    echo "  isl29018"
    echo
    echo "Current IIO devices:"
    find -L /sys/bus/iio/devices -maxdepth 2 -type f -print -exec sh -c 'printf "%s: " "$1"; cat "$1"' sh {} \; 2>/dev/null || true
  } >"$RAW/iio/topology-notes.txt" 2>&1 || true

  echo "== touchscreen-topology =="
  {
    echo "Gizmo FT7511 touchscreen notes"
    echo
    echo "I2C:"
    echo "  address=0x38"
    echo "  expected client name=ft7511"
    echo "  max_touches=10"
    echo "  report_period_active=100 Hz"
    echo "  max_x=1200"
    echo "  max_y=1920"
    echo
    echo "Registers:"
    echo "  0x80 threshold group"
    echo "  0x88 active report period"
    echo "  0xa1 lib version high"
    echo "  0xa2 lib version low"
    echo "  0xa3 cipher/vendor id"
    echo "  0xa6 firmware id"
    echo "  0xa8 focaltech id"
    echo "  0xaf release code id"
    echo
    echo "Reset sequence:"
    echo "  reset GPIO high, sleep 5..20 ms, reset GPIO low, sleep 300 ms"
    echo
    echo "Probe requirements:"
    echo "  I2C functionality, nonzero IRQ, reset GPIO requested output-low before reset,"
    echo "  readable register 0x00, optional monitor-disable writes to 0x86 and 0xa5,"
    echo "  active period write of report_period_active / 10 to 0x88."
    echo
    echo "Interrupt read flow:"
    echo "  read reg 0x02, low nibble is touch count; read count*6 bytes from 0x03."
    echo "  point: event=xhi_event>>6, id=yhi_id>>4, x=((xhi_event&0x0f)<<8)|xlo,"
    echo "  y=((yhi_id&0x0f)<<8)|ylo, active when event is 0 or 2, pressure=weight&0x7f."
    echo
    echo "GPIO/IRQ notes:"
    echo "  reset line is gizmo-gpio-resets:touchpanel_rst (offset 3)."
    echo "  interrupt line is gizmo-intc:touchpanel_intr (bit 0)."
    echo "  gizmo-ft7511 uses that IRQ when mapped and falls back to polling otherwise."
    echo
    echo "Current adapters:"
    i2cdetect -l
    echo
    echo "Current interrupts:"
    cat /proc/interrupts
    echo
    echo "GPIO lines:"
    gpioinfo || true
  } >"$RAW/touchscreen/topology-notes.txt" 2>&1 || true

  echo "== led-topology =="
  {
    echo "Gizmo LED topology notes"
    echo
    echo "Parent bus:"
    echo "  Lower bus adapter name starts with i2c-ocores."
    echo "  0x72 is the pca9543 mux."
    echo
    echo "Mux channels:"
    echo "  Adapter name format is expected to include i2c-<lower_bus>-mux (chan_id 0|1)."
    echo "  Right side is the channel with PN547 NFC at 0x28 plus LP55231 devices at 0x32-0x35."
    echo "  If chan_id 1 does not match the right-side list, try chan_id 0 and swap left/right."
    echo
    echo "Left side:"
    echo "  0x32 lp55231 left RGB LEDs 1-3"
    echo "  0x33 lp55231 left RGB LEDs 4-6"
    echo "  0x34 lp55231 left RGB LEDs 7-9"
    echo "  0x35 lp55231 left RGB LEDs 10-12"
    echo
    echo "Right side:"
    echo "  0x28 pn547 NFC"
    echo "  0x32 lp55231 right RGB LEDs 1-3"
    echo "  0x33 lp55231 right RGB LEDs 4-6"
    echo "  0x34 lp55231 right RGB LEDs 7-9"
    echo "  0x35 lp55231 right RGB LEDs 10-12"
    echo
    echo "LP55231 platform data:"
    echo "  num_channels=9"
    echo "  clock_mode=lp55xx_clock_auto"
    echo "  enable_gpio=-1"
    echo "  led_current=50"
    echo "  max_current=100"
    echo "  channel names use <side>::<logical_number>::<color>"
    echo "  channel order: 0 blue, 1 blue, 2 blue, 3 green, 4 green, 5 green, 6 red, 7 red, 8 red"
    echo
    echo "LP55231 logical LED mapping:"
    echo "  0x32 -> logical LEDs 1,2,3"
    echo "  0x33 -> logical LEDs 4,5,6"
    echo "  0x34 -> logical LEDs 7,8,9"
    echo "  0x35 -> logical LEDs 10,11,12"
    echo
    echo "Current adapters:"
    i2cdetect -l
  } >"$RAW/leds/topology-notes.txt" 2>&1 || true

  for lower_bus in $ocores_buses; do
    local chan0_bus chan1_bus right_bus left_bus
    chan0_bus="$(i2cdetect -l 2>/dev/null | awk -v lower="$lower_bus" 'tolower($0) ~ ("i2c-" lower "-mux") && $0 ~ /chan_id 0/ { sub(/^i2c-/, "", $1); print $1; exit }')"
    chan1_bus="$(i2cdetect -l 2>/dev/null | awk -v lower="$lower_bus" 'tolower($0) ~ ("i2c-" lower "-mux") && $0 ~ /chan_id 1/ { sub(/^i2c-/, "", $1); print $1; exit }')"

    {
      echo "lower_bus=$lower_bus"
      echo "chan_id_0_bus=${chan0_bus:-missing}"
      echo "chan_id_1_bus=${chan1_bus:-missing}"
      echo
      echo "right-side signature requires visible addresses: 0x28, 0x32, 0x33, 0x34, 0x35"
    } >"$RAW/leds/lower-bus-$lower_bus-mux-topology.txt"

    if [ -n "$chan0_bus" ]; then
      run_capture "leds/lower-bus-$lower_bus-mux-chan0-scan" i2cdetect -y "$chan0_bus"
    fi
    if [ -n "$chan1_bus" ]; then
      run_capture "leds/lower-bus-$lower_bus-mux-chan1-scan" i2cdetect -y "$chan1_bus"
    fi

    right_bus=""
    left_bus=""
    if [ -n "$chan1_bus" ] && i2cdetect -y "$chan1_bus" 2>/dev/null | grep -Eq '(^| )28( |$)'; then
      right_bus="$chan1_bus"
      left_bus="$chan0_bus"
    elif [ -n "$chan0_bus" ] && i2cdetect -y "$chan0_bus" 2>/dev/null | grep -Eq '(^| )28( |$)'; then
      right_bus="$chan0_bus"
      left_bus="$chan1_bus"
    fi

    {
      echo
      echo "detected_right_bus=${right_bus:-unknown}"
      echo "detected_left_bus=${left_bus:-unknown}"
      if [ -z "$right_bus" ]; then
        echo "warning=no mux channel showed the right-side PN547 address 0x28"
      fi
    } >>"$RAW/leds/lower-bus-$lower_bus-mux-topology.txt"

    for side_bus in "$left_bus" "$right_bus"; do
      [ -n "$side_bus" ] || continue
      local side
      if [ "$side_bus" = "$right_bus" ]; then
        side="right"
      else
        side="left"
      fi

      for addr in 0x32 0x33 0x34 0x35; do
        run_capture "leds/lp55231-$side-bus-$side_bus-addr-$addr-head" i2cdump -y -r 0x00-0x20 "$side_bus" "$addr"
      done
    done
  done

  run_capture gpioinfo gpioinfo
}

make_redacted_summary() {
  {
    echo "Gizmo collector summary"
    echo "Generated: $STAMP"
    echo
    echo "Files:"
    find "$OUT" -maxdepth 3 -type f | sort
    echo
    echo "Selected hardware inventory:"
    sed -E 's/([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}/<redacted-mac>/Ig' "$RAW/lspci.txt" 2>/dev/null || true
    echo
    sed -E 's/([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}/<redacted-mac>/Ig' "$RAW/lsblk.txt" 2>/dev/null || true
    echo
    sed -E 's/([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}/<redacted-mac>/Ig' "$RAW/ip-address.txt" 2>/dev/null || true
  } >"$SUMMARY"
}

disable_watchdog
collect_firmware
collect_emmc
collect_platform
collect_audio
collect_i2c_leds
make_redacted_summary

tar -C "$OUTPUT_ROOT" -caf "$OUT.tar.zst" "$(basename "$OUT")"
write_sha512 "$OUT.tar.zst"

echo "Gizmo collector complete"
echo "Bundle: $OUT.tar.zst"
