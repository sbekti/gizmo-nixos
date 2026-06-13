# SPDX-License-Identifier: MIT
{
  lib,
  stdenvNoCC,
  coreutils,
  gawk,
  makeWrapper,
}:

stdenvNoCC.mkDerivation {
  pname = "gizmo-sensor-tools";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = [ makeWrapper ];

  installPhase = ''
    runHook preInstall

    install -Dm755 gizmo-list-sensors "$out/bin/gizmo-list-sensors"
    install -Dm755 gizmo-watch-accelerometer "$out/bin/gizmo-watch-accelerometer"
    install -Dm755 gizmo-watch-magnetometer "$out/bin/gizmo-watch-magnetometer"
    install -Dm755 gizmo-watch-ambient-light "$out/bin/gizmo-watch-ambient-light"

    patchShebangs "$out/bin"

    for tool in \
      "$out/bin/gizmo-list-sensors" \
      "$out/bin/gizmo-watch-accelerometer" \
      "$out/bin/gizmo-watch-magnetometer" \
      "$out/bin/gizmo-watch-ambient-light"; do
      wrapProgram "$tool" --prefix PATH : ${lib.makeBinPath [
        coreutils
        gawk
      ]}
    done

    ln -s gizmo-watch-ambient-light "$out/bin/gizmo-watch-light"
    ln -s gizmo-watch-magnetometer "$out/bin/gizmo-watch-compass"

    runHook postInstall
  '';

  meta = {
    description = "Interactive Gizmo IIO sensor inspection helpers";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
  };
}
