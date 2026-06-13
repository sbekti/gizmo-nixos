# SPDX-License-Identifier: MIT
{
  lib,
  coreutils,
  makeWrapper,
  bash,
  gnugrep,
  gawk,
  gnused,
  gnutar,
  gzip,
  xz,
  zstd,
  util-linux,
  parted,
  file,
  pciutils,
  usbutils,
  kmod,
  iproute2,
  ethtool,
  dmidecode,
  acpica-tools,
  alsa-utils,
  i2c-tools,
  libgpiod,
  flashrom,
  cbfstool,
  ifdtool,
  dosfstools,
  gizmo-backlight,
  gizmo-io-dump,
  gizmo-nfc-tools,
  gizmo-sensor-tools,
  watchdog-control,
  writeShellApplication,
}:

writeShellApplication {
  name = "gizmo-collector";

  runtimeInputs = [
    coreutils
    gnugrep
    gawk
    gnused
    gnutar
    gzip
    xz
    zstd
    util-linux
    parted
    file
    pciutils
    usbutils
    kmod
    iproute2
    ethtool
    dmidecode
    acpica-tools
    alsa-utils
    i2c-tools
    libgpiod
    flashrom
    cbfstool
    ifdtool
    dosfstools
    gizmo-backlight
    gizmo-io-dump
    gizmo-nfc-tools
    gizmo-sensor-tools
    watchdog-control
  ];

  text = builtins.readFile ./gizmo-collector.sh;

  meta = {
    description = "Gizmo firmware and hardware bring-up collector";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
