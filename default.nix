# nix-build: the native Linux build (README.md, section Linux). Uses the nixpkgs channel by
# default; pass --arg pkgs to use another.
{ pkgs ? import <nixpkgs> { } }:
pkgs.callPackage ./nix/package.nix { }
