{
  description = "FSR 4.1.1 for the native Linux build of BeamNG.drive (Vulkan layer)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: rec {
        bng-fsr4 = pkgs.callPackage ./package.nix { version = nixpkgs.lib.fileContents ./VERSION; };
        default = bng-fsr4;
      });

      overlays.default = final: _: {
        bng-fsr4 = final.callPackage ./package.nix { version = nixpkgs.lib.fileContents ./VERSION; };
      };

      homeManagerModules.default = import ./nix/home-manager.nix self;

      devShells = forAll (pkgs: {
        default = pkgs.mkShell { inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ]; };
      });
    };
}
