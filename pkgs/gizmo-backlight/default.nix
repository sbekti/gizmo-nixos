# SPDX-License-Identifier: MIT
{ stdenv, lib }:

stdenv.mkDerivation {
  pname = "gizmo-backlight";
  version = "0.1.0";
  src = ./gizmo-backlight.c;

  dontUnpack = true;

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    $CC $CFLAGS -O2 -Wall -Wextra $src -o $out/bin/gizmo-backlight
    runHook postInstall
  '';

  meta = {
    description = "Read or set the Gizmo CPLD backlight brightness byte";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
