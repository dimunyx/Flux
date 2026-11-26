{
  description = "Flux — one interface, every package manager";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        rec {
          flux = pkgs.callPackage ./nix/package.nix {
            # Flux is tuned for Clang and uses ThinLTO in Release builds.
            stdenv = pkgs.clangStdenv;
          };
          default = flux;
        }
      );

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/flux";
          meta.description = "Launch Flux";
        };
      });

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              clang
              clang-tools
              lld
              just
              glfw
              freetype
              fontconfig
              libpng
              librsvg
              libepoxy
              libGL
            ];

            shellHook = ''
              echo "Flux development shell"
              echo "  just build   # configure and build Release"
              echo "  just run     # run the built executable"
            '';
          };
        }
      );

      formatter = forAllSystems (system: nixpkgs.legacyPackages.${system}.nixfmt-rfc-style);

      checks = forAllSystems (system: {
        default = self.packages.${system}.default;
      });
    };
}
