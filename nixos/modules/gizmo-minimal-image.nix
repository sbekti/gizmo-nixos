# SPDX-License-Identifier: MIT
{ lib, ... }:

{
  documentation.enable = lib.mkDefault false;
  documentation.nixos.enable = lib.mkDefault false;
  programs.command-not-found.enable = lib.mkDefault false;
  environment.defaultPackages = lib.mkDefault [ ];
}
