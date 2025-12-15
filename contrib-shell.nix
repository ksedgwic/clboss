{
  pkgs ? import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/nixpkgs-unstable.tar.gz";
  }) { },
}:

let
  poetry2nix = import (builtins.fetchGit {
    url = "https://github.com/nix-community/poetry2nix";
  }) { inherit pkgs; };
in

pkgs.mkShell {
  buildInputs = [
    pkgs.poetry
    (poetry2nix.mkPoetryEnv {
      projectDir = ./contrib;
    })
  ];
}
