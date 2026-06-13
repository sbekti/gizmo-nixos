# Customizing Gizmo Images

Use this repository as a flake input and build your site-specific image from a
small downstream flake. Avoid editing the included rescue or kiosk profiles
unless you are changing the shared project defaults.

The exported NixOS modules are directly importable. You do not need to pass
`specialArgs.self` to use them.

See `examples/flake/flake.nix` for a complete starter.

## Build A Downstream Image

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    gizmo-nixos.url = "github:OWNER/gizmo-nixos";
    gizmo-nixos.inputs.nixpkgs.follows = "nixpkgs";
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

              gizmo.common.enable = true;
              gizmo.kiosk.enable = true;
              gizmo.kiosk.url = "http://homeassistant.local:8123/";

              networking.hostName = "gizmo-kiosk";
              system.stateVersion = "25.11";
            })
          ];
        };
    };
}
```

Build it with:

```sh
nix build .# --out-link result-gizmo
```

## Common Options

Set the dashboard URL:

```nix
gizmo.kiosk.url = "https://homeassistant.example.test:8123/lovelace/default_view?kiosk";
```

Choose the browser:

```nix
gizmo.kiosk.browser = "chromium";
# or, for diagnostics:
gizmo.kiosk.browser = "firefox";
```

Tune touch coordinates:

```nix
gizmo.common.touchscreen.swapXY = true;
gizmo.common.touchscreen.invertX = false;
gizmo.common.touchscreen.invertY = true;
gizmo.common.touchscreen.pollMs = 20;
```

Set boot-time hardware behavior:

```nix
gizmo.common.disableWatchdogOnBoot = true;
gizmo.common.backlight.brightness = 192;
gizmo.common.audio.masterVolumePercent = 60;
gizmo.common.leds.animateOnBoot = false;
```

Override NFC detection only when auto-detection is wrong:

```nix
gizmo.common.nfc.bus = 12;
gizmo.common.nfc.address = 0x28;
gizmo.common.nfc.irqGpio = null;
gizmo.common.nfc.venGpio = null;
```

## Add Packages And Services

The flake exports a userspace package overlay. Kernel modules are normally
installed by `gizmo.common.enable` and built against the configured kernel.

```nix
{ pkgs, ... }:
{
  nixpkgs.overlays = [ gizmoNixos.overlays.default ];

  environment.systemPackages = [
    pkgs.gizmo-backlight
    pkgs.gizmo-nfc-tools
    pkgs.gizmo-sensor-tools
  ];
}
```

`gizmo-minimal-image` is optional. Import it for appliance-style images where
smaller defaults are preferred; omit it when adding Gizmo hardware support to a
general-purpose NixOS system.

Add normal NixOS services in the same module:

```nix
{
  services.avahi.enable = true;
  time.timeZone = "America/Denver";
}
```

## Enable SSH With Keys

Public sample images leave SSH disabled. For a trusted site image, enable SSH
explicitly and prefer keys over passwords:

```nix
{
  services.openssh = {
    enable = true;
    settings = {
      PasswordAuthentication = false;
      PermitRootLogin = "prohibit-password";
    };
  };

  users.users.root.openssh.authorizedKeys.keys = [
    "ssh-ed25519 AAAA... user@example"
  ];
}
```

## Add Custom CA Trust

For a dashboard using a private CA, add the CA to the system bundle and import
it into Chromium's NSS database for the kiosk user:

```nix
{ config, lib, pkgs, ... }:
let
  cfg = config.gizmo.kiosk;
  siteRootCa = pkgs.fetchurl {
    url = "https://example.test/root-ca.crt";
    hash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
  };
  certName = "Example Site Root CA";
  nssDb = "/var/lib/gizmo-kiosk/.pki/nssdb";
in
{
  security.pki.certificateFiles = [ siteRootCa ];

  systemd.services.gizmo-kiosk = lib.mkIf cfg.enable {
    path = [
      pkgs.coreutils
      pkgs.nssTools
    ];
    preStart = lib.mkAfter ''
      install -d -m 0700 -o ${cfg.user} -g ${cfg.user} /var/lib/gizmo-kiosk/.pki
      install -d -m 0700 -o ${cfg.user} -g ${cfg.user} ${nssDb}

      if [ ! -f ${nssDb}/cert9.db ]; then
        certutil -N -d sql:${nssDb} --empty-password
      fi

      certutil -D -d sql:${nssDb} -n "${certName}" >/dev/null 2>&1 || true
      certutil -A -d sql:${nssDb} -n "${certName}" -t "C,," -i ${siteRootCa}
      chown -R ${cfg.user}:${cfg.user} /var/lib/gizmo-kiosk/.pki
    '';
  };
}
```

Replace the URL and hash with your own CA. Get the Nix hash with:

```sh
nix store prefetch-file https://example.test/root-ca.crt
```
