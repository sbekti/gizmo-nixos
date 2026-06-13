# SPDX-License-Identifier: MIT
{
  lib,
  stdenv,
  makeWrapper,
  coreutils,
  gnugrep,
  kmod,
  i2c-tools,
}:

stdenv.mkDerivation {
  pname = "gizmo-nfc-tools";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = [ makeWrapper ];

  installPhase = ''
    runHook preInstall

    mkdir -p "$out/bin"
    $CC $CFLAGS -O2 -Wall -Wextra gizmo-nfc-test.c -o "$out/bin/gizmo-nfc-test"
    install -Dm755 gizmo-test-nfc "$out/bin/gizmo-test-nfc"
    patchShebangs "$out/bin/gizmo-test-nfc"
    wrapProgram "$out/bin/gizmo-test-nfc" --prefix PATH : ${lib.makeBinPath [
      coreutils
      gnugrep
      kmod
      i2c-tools
    ]}:$out/bin

    runHook postInstall
  '';

  meta = {
    description = "Gizmo PN547/PN544 NFC bring-up test helpers";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
