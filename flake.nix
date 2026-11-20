{
  description = "UNA Watch app and firmware development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [
          # Arm GNU Toolchain 14.3.rel1 (Arm's official binaries, the version
          # the SDK docs name). Includes arm-none-eabi-gdb.
          gcc-arm-embedded-14
          cmake
          ninja
          gnumake
          pkg-config

          # app_packer / app_merging (pyelftools, pillow), firmware_packer
          # (bincopy), lvgl_assets (pypng, lz4); the rest is for the Zephyr
          # build (west and zephyr/scripts/requirements-base.txt)
          (python3.withPackages (
            ps: with ps; [
              pyelftools
              pillow
              bincopy
              pypng
              lz4
              west
              pyyaml
              pykwalify
              packaging
              anytree
              intelhex
              jsonschema
              pyserial
              semver
              # imgtool (MCUboot image signing)
              cryptography
              click
              cbor2
            ]
          ))

          # LVGL / glance simulators (host build)
          gcc
          SDL2

          # lvgl_assets.py runs `npx lv_font_conv`
          nodejs

          # Zephyr build: devicetree compiler, gperf for the kobject hash
          dtc
          gperf

          # mcumgr client: image upload over the watch's SMP serial port
          smpmgr

          # debug, serial console, editor support
          openocd
          probe-rs-tools
          tio
          clang-tools
        ];
      };
    };
}
