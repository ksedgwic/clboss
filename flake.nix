{
  description = "CLBOSS Automated Core Lightning Node Manager";
  nixConfig.bash-prompt = "\[nix-develop\]$ ";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        nativeBuildInputs = [
          pkgs.autoconf-archive
          pkgs.autoreconfHook
          pkgs.pkg-config
          pkgs.libev
          pkgs.libunwind
          pkgs.curlWithGnuTls
          pkgs.sqlite
        ];
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          name = "clboss";
          src = ./.;
          inherit nativeBuildInputs;

          enableParallelBuilding = true;

          preBuild = ''
            ./generate_commit_hash.sh
          '';

          checkPhase = ''
            make -j4 distcheck
          '';

          doCheck = true;

          meta = with pkgs.lib; {
            description = "Automated Core Lightning Node Manager";
            homepage = "https://github.com/ZmnSCPxj/clboss";
            license = licenses.mit;
            platforms = platforms.linux ++ platforms.darwin;
            mainProgram = "clboss";
          };
        };

        formatter = pkgs.nixfmt-tree;

        devShells.default = pkgs.mkShell {
          inherit nativeBuildInputs;
          buildInputs =
            with pkgs;
            nativeBuildInputs
            ++ [
              gcc
              libevdev
              bind
              autoconf
              autoconf-archive
              libtool
              automake
              git

              # editor support
              bear
            ];
        };
      }
    );
}
