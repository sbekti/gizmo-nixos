# Booting Gizmo over the network with iPXE

Gizmo can boot its kiosk system entirely from the network. The built-in storage holds the bootloader and device data. The operating system itself is downloaded into memory on every boot, so you can update it centrally without touching the device.

How it fits together:

```text
Built-in storage -> iPXE -> DHCP boot address
  -> kiosk files over HTTP -> system runs in memory
```

Gizmo ships with its own network boot firmware, but it is hardcoded to fetch its image from Meta's internal infrastructure, which is not reachable outside that network. Installing a vanilla iPXE bootloader on the built-in storage replaces that path so the device boots from your own server instead.

## What you will need

- A Gizmo with wired Ethernet.
- A USB drive you can rewrite.
- A Linux USB for one-time setup. The rescue image in this repo works well for this:

```sh
nix build .#rescue-collector-image --out-link result-rescue
sudo dd if=result-rescue/nixos.img of=/dev/disk/by-id/usb-YOUR_DEVICE bs=16M status=progress conv=fsync
```

It boots to a root console with `lsblk`, `sgdisk`, `mkfs.fat`, `mkfs.ext4`, and `ssh-keygen` available. Any recent Ubuntu or NixOS live USB with those tools works too.

- An iPXE boot file for UEFI (`snp.efi`). You can use a prebuilt one from ipxe.org.
- A place on your network to host the kiosk files, with DHCP that can give Gizmo a fixed address and a boot address (option 67), plus HTTP hosting.

The kiosk system itself can be the kiosk image in this repo:

```sh
nix build .#homeassistant-kiosk-image --out-link result-kiosk
```

Use it directly on USB while testing, then publish its kernel, initrd, and system image as the network files below.

A typical release folder looks like this:

```text
kiosk.ipxe
bzImage
initrd
store.squashfs
SHA256SUMS
```

Keep each release in its own versioned folder. To roll out an update, upload a new folder and point DHCP at it.

You can host these files on a router with USB storage, or on a NAS with a small file server container. Either is fine as long as Gizmo can reach it over HTTP.

## How the built-in storage is organized

The built-in storage has two partitions:

- A 512 MiB system partition (FAT32) with the iPXE bootloader, the SSH host key, and the recovery console password.
- A 1 GiB data partition (ext4) that only stores the browser profile.

The bootloader looks like this:

```text
EFI/BOOT/BOOTX64.EFI
ssh/ssh_host_ed25519_key
identity/breakglass-password-hash
```

The bootloader itself does not know your server address or version. It learns those from DHCP each time it starts, so the same setup works if you move the device or change servers.

## BIOS setup

The BIOS mode changes by stage:

- Test and final boot: UEFI mode. iPXE here is a UEFI application.
- One-time setup: Legacy mode, to boot the rescue USB.

Switch modes in the BIOS when each section tells you to. Do not leave legacy mode enabled for normal boots.

## Test the network path first

Do this before making any changes to the built-in storage. The BIOS should be in UEFI mode.

1. Copy `snp.efi` to the USB drive.
2. Plug it into Gizmo, start the device, open the BIOS boot menu, and choose the EFI Shell.
3. In the shell, find the USB drive and start iPXE:

```efi
map -r
fs0:
ls
\snp.efi
```

If that file does not start, try `ipxe.efi` if you have it on the drive.

4. In iPXE, check the network interfaces and request an address:

```ipxe
ifstat
dhcp net0
ifstat
route
```

Use the interface that shows link up. `route` should show the address from DHCP.

5. Load the kiosk files manually to confirm the full path works:

```ipxe
chain http://<your-server>:<your-port>/path/to/kiosk.ipxe
```

If the kiosk starts, your DHCP and HTTP setup are correct. Reboot and continue with setup. If not, fix the network first.

For DHCP, configure on your router:

- A fixed address for Gizmo's MAC address.
- The boot address (DHCP option 67) pointing to your `kiosk.ipxe` URL.
- The hostname option (option 12) if you want the system to pick up its name from DHCP.

The exact menu names depend on your router software.

## Set up the built-in storage

This erases the built-in storage. Check the device name carefully.

1. Rewrite the USB drive with the rescue image above. Keep a copy of the `snp.efi` you tested on it.
2. Reboot, enter the BIOS, and switch the boot mode to Legacy. No other BIOS change is needed.
3. Boot Gizmo from the USB drive and find the built-in disk:

```sh
lsblk -o NAME,SIZE,TYPE,MOUNTPOINTS
```

It is usually `/dev/mmcblk0`. Make sure none of its partitions are mounted before continuing.

3. Create the partitions:

```sh
sudo sgdisk --zap-all /dev/mmcblk0
sudo sgdisk \
  --new=1:2048:+512MiB --typecode=1:ef00 --change-name=1:"GIZMOEFI" \
  --new=2:0:+1GiB --typecode=2:8300 --change-name=2:"GIZMOSTATE" \
  /dev/mmcblk0
```

4. Format them:

```sh
sudo mkfs.fat -F 32 -n GIZMOEFI /dev/mmcblk0p1
sudo mkfs.ext4 -F -L GIZMOSTATE /dev/mmcblk0p2
```

5. Install the bootloader you already tested:

```sh
sudo mount /dev/mmcblk0p1 /mnt
sudo mkdir -p /mnt/EFI/BOOT /mnt/ssh /mnt/identity
sudo cp snp.efi /mnt/EFI/BOOT/BOOTX64.EFI
```

6. Create the device identity:

```sh
sudo ssh-keygen -q -t ed25519 -N '' -f /mnt/ssh/ssh_host_ed25519_key
```

Save the printed public key somewhere safe — you will need it to recognize the device over SSH later. Set the recovery console password in `/mnt/identity/` according to your system configuration.

```sh
sync
sudo umount /mnt
```

## Boot from the built-in storage

Remove the USB drive, enter the BIOS, switch the boot mode back to UEFI, and set the BIOS to boot from the built-in storage first.

On power-on, Gizmo now starts iPXE directly, picks up its boot address from DHCP, and loads the kiosk system. You no longer need the USB drive.

## What happens on each boot

The `kiosk.ipxe` file tells iPXE where to find the kernel and system image:

```ipxe
kernel bzImage init=<system-init> initrd=initrd
  gizmo.store_url=http://<your-server>:<your-port>/path/to/store.squashfs
  gizmo.store_sha256=<checksum>
  gizmo.mode=kiosk
initrd initrd
boot
```

The system downloads the image into memory, checks its checksum, and starts it. It then mounts the data partition for the browser profile and opens Chromium in kiosk mode.

## Checking it works

On the running device:

```sh
cat /proc/cmdline
systemctl --failed
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS
```

The command line should show the server address you configured. There should be no failed services, and the data partition should be mounted.

## Updates and care

- To update the system, publish a new version folder and update the DHCP boot address. Reboot the device.
- Only update the bootloader if its network behavior changes.
- Keep the system partition mounted read-only during normal use.
- A backup of the system partition is useful for quick repair. It restores booting — the running system version is still selected by DHCP.

## Security notes

Only use this on a network you trust. The checksum protects against corrupted downloads, but plain HTTP does not prove the server's identity. Use SSH keys for remote access, keep password logins off, and store the recovery console password separately from the device.
