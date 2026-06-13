# SPDX-License-Identifier: MIT
{ stdenv, lib }:

stdenv.mkDerivation {
  pname = "gizmo-io-dump";
  version = "0.1.0";
  src = ./gizmo-io-dump.c;

  dontUnpack = true;

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    $CC $CFLAGS $src -o $out/bin/gizmo-io-dump
    runHook postInstall
  '';

  meta = {
    description = "Dump Gizmo I/O port ranges";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
