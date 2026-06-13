# Rescue Collector

The rescue image is the first image to boot on a Gizmo when bringing up a new
device. It disables the CPLD watchdog, grows the USB root filesystem, and runs
`gizmo-collector` once at boot.

## Build

```sh
nix build .#rescue-collector-image --out-link result-rescue
```

The image is:

```text
result-rescue/nixos.img
```

## Boot Behavior

- Root is autologged in on the local console.
- SSH is disabled by default.
- The root filesystem grows to fill the USB stick.
- Collector output is written under `/var/lib/gizmo-collector`.
- Instructions are available at `/root/README-GIZMO-RESCUE.txt`.

## Output Layout

Each collector run creates:

```text
/var/lib/gizmo-collector/gizmo-collector-<timestamp>/
/var/lib/gizmo-collector/gizmo-collector-<timestamp>.tar.zst
/var/lib/gizmo-collector/gizmo-collector-<timestamp>.tar.zst.sha512
```

The expanded run directory contains:

- `collector.log`: command progress and failures
- `redacted-summary.txt`: compact hardware summary
- `raw/firmware/`: flashrom probe output, active firmware dump, IFDtool and CBFS metadata
- `raw/emmc/`: partition tables, filesystem archives, and optional raw images when space permits
- `raw/display/`: DRM, framebuffer, backlight, and graphics sysfs snapshots
- `raw/audio/`: ALSA inventory, mixer state, and speaker-test output
- `raw/network/`: network inventory captured through the platform commands
- `raw/i2c/`, `raw/leds/`, `raw/touchscreen/`, `raw/nfc/`: device scans and protocol checks
- `raw/sysfs/`, `raw/acpi/`, `raw/input/`: platform and device-tree context

Bundles can include firmware images, eMMC snapshots, hardware IDs, logs, and
large binary data.

## Useful Commands

Check collection status:

```sh
systemctl status gizmo-collector --no-pager
journalctl -u gizmo-collector -b --no-pager
```

Rerun collection:

```sh
/root/collect-debug-data.sh
```

Check root filesystem growth:

```sh
lsblk -f
findmnt /
systemctl status growpart --no-pager
```

Run quick hardware checks:

```sh
gizmo-backlight -q
gizmo-backlight -s 255
gizmo-list-sensors
gizmo-watch-accelerometer
gizmo-watch-magnetometer
gizmo-watch-ambient-light
gizmo-test-nfc
gizmo-test-nfc --nci-reset
gizmo-test-nfc --nci-discover --seconds 15
gpioinfo
```

## Copying Output

Mount the USB stick on another Linux machine and copy the latest tarball from
`/var/lib/gizmo-collector`. The tarball already contains the expanded run
directory and a matching `.sha512` checksum file.
