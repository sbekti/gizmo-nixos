# SPDX-License-Identifier: MIT
{ stdenv, lib }:

stdenv.mkDerivation {
  pname = "watchdog-control";
  version = "0.1.0";

  src = ./watchdog-control.c;
  dontUnpack = true;

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    $CC $CFLAGS -O2 -Wall -Wextra $src -o $out/bin/watchdog-control
    runHook postInstall
  '';

  meta = {
    description = "Disable or query the Gizmo CPLD watchdog I/O port";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
