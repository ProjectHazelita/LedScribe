{
  description = "LedScribe C++ development shell";

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
      formatter = forAllSystems (system: (import nixpkgs { inherit system; }).nixfmt);
      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages;
          qt = pkgs.qt6;
          qtModules = [
            qt.qtbase
            qt.qtwayland
          ];
        in
        {
          default = pkgs.mkShell {
            packages = [
              llvm.clang
              llvm.lld
              llvm.llvm
              pkgs.gdb
              llvm.clang-tools
              pkgs.cmake
              pkgs.ninja
              pkgs.gnumake
              pkgs.coreutils
              pkgs.bear
              pkgs.nixfmt
              pkgs.libusb1
              pkgs.pkg-config
            ];
            nativeBuildInputs = [ qt.wrapQtAppsHook ];
            buildInputs = qtModules;
            # Locally built binaries are not wrapped by Nix's package fixup phase.
            QT_PLUGIN_PATH = pkgs.lib.makeSearchPath qt.qtbase.qtPluginPrefix qtModules;
          };
        }
      );
    };
}
