# SPDX-License-Identifier: MIT
{ stdenv, lib }:

stdenv.mkDerivation {
  pname = "gizmo-led-rainbow";
  version = "0.2.0";

  src = ./gizmo-led-rainbow.c;

  dontUnpack = true;

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    $CC $CFLAGS -O2 -Wall -Wextra $src -o $out/bin/gizmo-led-rainbow
    runHook postInstall
  '';

  meta = {
    description = "Animate Gizmo LP55231 LEDs for visual bring-up feedback";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
