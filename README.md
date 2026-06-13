# NixOS board-support flake for Gizmo

A NixOS board support package for Facebook Gizmo devices, 
providing image builders, hardware modules, kernel drivers, and bring-up tools.

This flake builds:

- a rescue collector USB image for firmware and hardware bring-up data
- a generic HomeAssistant-style Chromium kiosk image
- reusable NixOS modules for downstream Gizmo images
- userspace helpers and out-of-tree kernel modules for Gizmo hardware

The current target is x86_64 USB boot. PXE/netboot is not wired up yet.

## Requirements

- Nix with flakes enabled
- an x86_64 Linux builder
- a USB stick large enough for the image and collector output

For full eMMC captures from the rescue image, use a 16 GiB or larger USB stick.

## Build The Rescue Image

```sh
nix build .#rescue-collector-image --out-link result-rescue
```

The image will be available at:

```text
result-rescue/nixos.img
```

## Write An Image To USB

Find the target USB disk:

```sh
lsblk -o NAME,SIZE,MODEL,SERIAL,TRAN,TYPE,MOUNTPOINTS
```

Use the whole disk, preferably through `/dev/disk/by-id/...`, not a partition.
The `dd` command overwrites the target disk.

```sh
sudo dd if=result-rescue/nixos.img of=/dev/disk/by-id/usb-YOUR_DEVICE bs=16M status=progress conv=fsync
sync
```

For the kiosk image, build and write:

```sh
nix build .#homeassistant-kiosk-image --out-link result-kiosk
sudo dd if=result-kiosk/nixos.img of=/dev/disk/by-id/usb-YOUR_DEVICE bs=16M status=progress conv=fsync
sync
```

## Rescue Image

The rescue image disables the CPLD watchdog, grows the root filesystem to fill
the USB stick, and runs `gizmo-collector` automatically.

Collector output is written under:

```text
/var/lib/gizmo-collector
```

The booted rescue image has local console root autologin for physical bring-up.
SSH is disabled by default. Instructions are available on the device at:

- `/etc/motd`
- `/root/README-GIZMO-RESCUE.txt`
- `/root/collect-debug-data.sh`

Useful commands on the booted Gizmo:

```sh
lsblk -f
findmnt /
systemctl status growpart --no-pager
systemctl status gizmo-collector --no-pager
journalctl -u gizmo-collector -b --no-pager
/root/collect-debug-data.sh
gizmo-backlight -q
gizmo-list-sensors
gizmo-test-nfc --nci-discover --seconds 15
gpioinfo
```

Collector bundles can include firmware images, eMMC snapshots, hardware IDs,
logs, and large binary data. See [docs/rescue-collector.md](docs/rescue-collector.md)
for the output layout.

## Kiosk Image

The public kiosk profile is generic. It starts Chromium in kiosk mode at:

```text
http://homeassistant.local:8123/
```

Downstream configurations should override:

```nix
gizmo.kiosk.url = "https://homeassistant.example.test:8123/lovelace/default_view?kiosk";
```

The kiosk image enables the common Gizmo hardware support, fixed clockwise
portrait display orientation, and matching FT7511 touch coordinate transform.
SSH and root autologin are disabled by default.

## Customize Your Own Image

Use this repo as a flake input and build your own image instead of editing the
included profiles directly. A complete starter is in
[examples/flake](examples/flake).

The exported NixOS modules are directly importable; downstream flakes do not
need to pass `specialArgs.self`.

Minimal shape:

```nix
{
  packages.x86_64-linux.default =
    gizmoNixos.lib.x86_64-linux.mkGizmoImage {
      modules = [
        ({ ... }: {
          imports = [
            gizmoNixos.nixosModules.gizmo-common
            gizmoNixos.nixosModules.gizmo-kiosk
            gizmoNixos.nixosModules.gizmo-minimal-image
          ];

          gizmo.common.enable = true;
          gizmo.kiosk.enable = true;
          gizmo.kiosk.url = "http://homeassistant.local:8123/";
          system.stateVersion = "25.11";
        })
      ];
    };
}
```

Common settings are documented in [docs/customizing.md](docs/customizing.md),
including overlays, extra packages, SSH keys, custom CA trust, browser choice,
touch transforms, NFC overrides, and LED behavior.

## Public Outputs

- `packages.x86_64-linux.rescue-collector-image`
- `packages.x86_64-linux.homeassistant-kiosk-image`
- `packages.x86_64-linux.gizmo-collector`
- `packages.x86_64-linux.gizmo-backlight`
- `packages.x86_64-linux.gizmo-led-rainbow`
- `packages.x86_64-linux.gizmo-nfc-tools`
- `packages.x86_64-linux.gizmo-sensor-tools`
- `nixosModules.gizmo-common`
- `nixosModules.gizmo-collector`
- `nixosModules.gizmo-kiosk`
- `nixosModules.gizmo-minimal-image`
- `nixosModules.default`
- `lib.x86_64-linux.mkGizmoSystem`
- `lib.x86_64-linux.mkGizmoImage`
- `overlays.default`

## Hardware Notes

Hardware bring-up details live in [docs/hardware.md](docs/hardware.md).

## License

This repository uses SPDX license identifiers in source files. See
[LICENSE.md](LICENSE.md) for the license map.
