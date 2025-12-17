{
  description = "CLBOSS Automated Core Lightning Node Manager";
  nixConfig.bash-prompt = "\[nix-develop\]$ ";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    flake-compat = {
      url = "github:edolstra/flake-compat";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      flake-compat,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        nativeBuildInputs = [
          pkgs.autoconf-archive
          pkgs.autoreconfHook
          pkgs.pkg-config
	];
        buildInputs = [
          pkgs.libev
          pkgs.libunwind
          pkgs.curlWithGnuTls
          pkgs.sqlite
        ];
        clboss = pkgs.stdenv.mkDerivation {
          name = "clboss";
          src = ./.;
          inherit nativeBuildInputs;
          inherit buildInputs;

          enableParallelBuilding = true;

          preBuild = ''
            ./generate_commit_hash.sh
          '';

          doCheck = false;

          meta = with pkgs.lib; {
            description = "Automated Core Lightning Node Manager";
            homepage = "https://github.com/ZmnSCPxj/clboss";
            license = licenses.mit;
            platforms = platforms.linux ++ platforms.darwin;
            mainProgram = "clboss";
          };
        };
      in
      {
        packages.default = clboss;
        checks.default = clboss.overrideAttrs (old: {
          doCheck = true;
          checkPhase = ''
            make -j4 distcheck
          '';
        });
        formatter = pkgs.nixfmt-tree;

        devShells.default = pkgs.mkShell {
          inherit nativeBuildInputs;
          buildInputs = buildInputs ++ (with pkgs; [
            gcc
            bind
            autoconf
            libtool
            automake
            git
            # editor support
            bear
          ]);
        };
      }
    );
}
