# SPDX-License-Identifier: MIT
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.gizmo.kiosk;
  # Chromium is the target browser. Firefox remains selectable as a diagnostic
  # browser for separating browser rendering issues from X/i915 issues.
  browserPackage =
    if cfg.browser == "firefox" then
      pkgs.firefox
    else
      pkgs.chromium;
  browserBin =
    if cfg.browser == "firefox" then
      "${pkgs.firefox}/bin/firefox"
    else
      "${pkgs.chromium}/bin/chromium";
  chromiumFlags = [
    "--kiosk"
    "--no-first-run"
    "--disable-infobars"
    "--disable-session-crashed-bubble"
    "--disable-features=Translate"
    "--start-fullscreen"
    "--window-position=0,0"
    "--window-size=1920,1200"
    "--force-device-scale-factor=1.5"
    "--touch-events=enabled"
    "--ozone-platform=x11"
    "--user-data-dir=${cfg.profileDir}"
    cfg.url
  ];
  firefoxFlags = [
    "--kiosk"
    "--profile"
    cfg.profileDir
    cfg.url
  ];
  browserFlags =
    if cfg.browser == "firefox" then
      firefoxFlags
    else
      chromiumFlags;
  browserEnvArgs = lib.escapeShellArgs (
    [
      "HOME=/var/lib/gizmo-kiosk"
      "XDG_RUNTIME_DIR=/run/gizmo-kiosk"
    ]
    ++ lib.optionals (cfg.browser == "firefox") [
      "MOZ_USE_XINPUT2=1"
      "MOZ_ENABLE_WAYLAND=0"
    ]
  );
  kioskSession = pkgs.writeShellScript "gizmo-kiosk-session" ''
    set -euo pipefail

    export DISPLAY=''${DISPLAY:-:0}
    # Keep a tiny WM: Chromium did not reliably occupy the full rotated screen
    # when it was the only X client.
    ${pkgs.openbox}/bin/openbox &
    sleep 0.5
    ${pkgs.xset}/bin/xset s off -dpms s noblank || true

    exec ${pkgs.util-linux}/bin/runuser -u ${cfg.user} -- \
      ${pkgs.coreutils}/bin/env ${browserEnvArgs} \
      ${browserBin} ${lib.escapeShellArgs browserFlags}
  '';
in
{
  options.gizmo.kiosk = {
    enable = lib.mkEnableOption "Gizmo HomeAssistant browser kiosk";

    browser = lib.mkOption {
      type = lib.types.enum [ "chromium" "firefox" ];
      default = "chromium";
      description = "Browser to run in kiosk mode.";
    };

    url = lib.mkOption {
      type = lib.types.str;
      default = "about:blank";
      description = "Dashboard URL to open in kiosk mode. Site/use-case profiles should set this.";
    };

    user = lib.mkOption {
      type = lib.types.str;
      default = "gizmo-kiosk";
      description = "Unprivileged user that runs the browser.";
    };

    profileDir = lib.mkOption {
      type = lib.types.str;
      default = "/var/lib/gizmo-kiosk/chromium";
      description = "Persistent Chromium profile directory.";
    };
  };

  config = lib.mkIf cfg.enable {
    users.users.${cfg.user} = {
      isSystemUser = true;
      group = cfg.user;
      home = "/var/lib/gizmo-kiosk";
      createHome = true;
    };

    users.groups.${cfg.user} = { };

    # Apply rotation at Xorg startup instead of live xrandr. Live RandR rotation
    # plus touch/pointer movement caused visible framebuffer jumps.
    services.xserver = {
      enable = true;
      displayManager.lightdm.enable = false;
      displayManager.startx.enable = true;
      windowManager.openbox.enable = true;
      videoDrivers = [ "modesetting" ];
      enableTearFree = true;
      monitorSection = ''
        Option "PreferredMode" "1200x1920"
        Option "Rotate" "right"
      '';
      screenSection = ''
        SubSection "Display"
          Virtual 1920 1200
        EndSubSection
      '';
      # SWCursor avoids Intel hardware cursor plane corruption on the rotated
      # output. PageFlip=false reduced residual tearing while scrolling.
      deviceSection = ''
        Option "SWCursor" "true"
        Option "PageFlip" "false"
      '';
    };

    hardware.graphics.enable = true;
    services.pipewire = {
      enable = true;
      alsa.enable = true;
      pulse.enable = true;
    };

    environment.systemPackages = [
      browserPackage
      pkgs.openbox
      pkgs.xset
      pkgs.xrandr
      pkgs.alsa-utils
      pkgs.util-linux
    ];

    systemd.tmpfiles.rules = [
      "d /var/lib/gizmo-kiosk 0755 ${cfg.user} ${cfg.user} -"
      "d ${cfg.profileDir} 0700 ${cfg.user} ${cfg.user} -"
    ];

    environment.etc."X11/xinit/xinitrc".source = kioskSession;
    environment.etc."gizmo-kiosk-session".source = kioskSession;

    systemd.services.gizmo-kiosk = {
      description = "Gizmo HomeAssistant Chromium kiosk";
      wantedBy = [ "multi-user.target" ];
      after = [
        "network-online.target"
        "systemd-user-sessions.service"
        "gizmo-disable-watchdog.service"
        "gizmo-load-touchscreen.service"
        "gizmo-load-leds.service"
      ];
      wants = [ "network-online.target" "gizmo-load-touchscreen.service" "gizmo-load-leds.service" ];
      environment = {
        HOME = "/var/lib/gizmo-kiosk";
      };
      path = [
        pkgs.coreutils
      ];
      preStart = ''
        install -d -o ${cfg.user} -g ${cfg.user} /var/lib/gizmo-kiosk
        install -d -m 0700 -o ${cfg.user} -g ${cfg.user} ${cfg.profileDir}
        install -d -m 0700 -o ${cfg.user} -g ${cfg.user} /run/gizmo-kiosk
      '';
      restartIfChanged = true;
      serviceConfig = {
        Type = "simple";
        WorkingDirectory = "/var/lib/gizmo-kiosk";
        # Run kiosk X on vt7 so it does not contend with root autologin/getty on tty1.
        ExecStart = "${pkgs.xinit}/bin/xinit ${kioskSession} -- ${pkgs.xorg-server}/bin/Xorg :0 vt7 -nolisten tcp -ac";
        StandardInput = "tty";
        StandardOutput = "journal";
        StandardError = "journal";
        TTYPath = "/dev/tty7";
        TTYReset = true;
        TTYVHangup = true;
        TTYVTDisallocate = true;
        Restart = "always";
        RestartSec = 5;
      };
    };
  };
}
