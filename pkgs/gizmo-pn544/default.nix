# SPDX-License-Identifier: GPL-2.0-only
{
  stdenv,
  lib,
  kernel,
}:

stdenv.mkDerivation {
  pname = "gizmo-pn544";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = kernel.moduleBuildDependencies;

  makeFlags = [
    "KERNELRELEASE=${kernel.modDirVersion}"
    "KERNEL_DIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "INSTALL_MOD_PATH=$(out)"
  ];

  installPhase = ''
    runHook preInstall
    make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build M=$PWD INSTALL_MOD_PATH=$out modules_install
    runHook postInstall
  '';

  meta = {
    description = "Gizmo PN547/PN544 NFC misc device driver";
    license = lib.licenses.gpl2Only;
    platforms = [ "x86_64-linux" ];
  };
}
