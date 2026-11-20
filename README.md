# UNA Watch development

Workspace for writing apps (LVGL) and building firmware for the UNA Watch.

- `refs/` — upstream clones, not tracked: `una-sdk`, `una-kernel-public`, `una-hardware`
- `docs/` — datasheets and hardware notes, see [docs/README.md](docs/README.md)
- `scripts/` — `una-install`, `una-build-kernel`, `una-pack`, `una-z` (on `PATH` inside the dev shell)
- `zephyr/` — west workspace for the Zephyr firmware; only `zephyr/una/` is tracked
- `apps/` — your own apps
- `build/` — out-of-tree build output, not tracked

Upstream documentation is in `refs/una-sdk/Docs/`; start with `sdk-setup.md`,
`platform-overview.md` and `Tutorials/HelloWorld/ARCHITECTURE.md`.

## Environment

`direnv allow` once; the flake provides Arm GNU Toolchain 14.3.rel1, CMake, Ninja,
Python with the packer dependencies, SDL2 for the simulator, Node.js for the LVGL
font converter, plus openocd, probe-rs, tio and clangd. `UNA_SDK` points at
`refs/una-sdk`.

## Build an app

```sh
cmake -G Ninja -S refs/una-sdk/Docs/Tutorials/HelloWorld/Software/Apps/HelloWorldLVGL-CMake -B build/hello
cmake --build build/hello        # -> build/hello/HelloWorldLVGL_<version>.uapp
```

`apps/hello` is a working copy of that tutorial with its own name and ID:

```sh
cmake -G Ninja -S apps/hello/Software/Apps/Hello-CMake -B build/hello
cmake --build build/hello        # also copied to apps/hello/Output/
cmake -G Ninja -S apps/hello/Software/Apps/LVGL-GUI/simulator -B build/hello-sim
cmake --build build/hello-sim    # -> apps/hello/Software/Apps/LVGL-GUI/simulator/build/bin/HelloSimulator
```

To start another app, copy an LVGL tutorial (`refs/una-sdk/Docs/Tutorials/*`, the
`LVGL-GUI` and `*LVGL-CMake` folders plus `Libs`) into `apps/` and change
`APP_NAME` and `APP_ID` in its `CMakeLists.txt`.

## Simulator

```sh
cmake -G Ninja -S refs/una-sdk/Docs/Tutorials/HelloWorld/Software/Apps/LVGL-GUI/simulator -B build/hello-sim
cmake --build build/hello-sim
```

The binary is written to `bin/` inside the build directory. Keys 1-4 are the
buttons L1, L2, R1, R2; Esc quits.

## Install on the watch

Connect USB and wait for the `UNA WATCH` drive, then:

```sh
una-install build/hello/HelloWorldLVGL_*.uapp   # copies to Apps/HelloWorldLVGL/
una-install -l                                  # list what is installed
```

The app list is only rebuilt at boot. `una-install -r FILE.uapp` reboots the watch
over its serial console after copying (`una-cli reset` does only the reboot).

On kernel 1.7.0 the drive does not come back after such a reboot until the cable
is replugged, so each new build still needs one replug. Upstream PR
[una-kernel-public#15](https://github.com/UNAWatch/una-kernel-public/pull/15)
adds a `usb drive on` console command that removes that.

## Console and logs

`una-cli menu` lists the console commands; `tio /dev/una-watch` opens it
interactively (commands must end in LF, prompt `una>`). Stay off the port for
about 10 seconds after the watch starts: typing in that window enters factory
test mode (`una-test>`); closing the port restarts the watch normally.

The debug UART (USART3 TX, 921600 baud) needs a USB-C debug adapter, see
[docs/README.md](docs/README.md).

## Kernel

`kernel/` is a working clone of the kernel repo (not tracked here), on branch
`custom`. `refs/una-kernel-public` stays pristine.

```sh
una-build-kernel        # Docker, image xanderhendriks/stm32cubeide:16.0, ~4 min
```

Output goes to `kernel/Exe/`. Images worth keeping are in `firmware/` with a
`SHA256SUMS` file:

- `firmware/official-v1.7.0/` — UNA's release, the restore image. A local build
  of tag `v1.7.0` is bit-identical to it.
- `firmware/custom-<version>/` — own builds.

### Install a kernel (same steps for custom and for restoring the official one)

1. Charge the watch; the bootloader refuses to update on low battery.
2. Turn the watch off, then hold the power button for 4 seconds as it turns on,
   until the bootloader menu appears.
3. Connect USB. The watch shows "USB connected" and a drive appears.
4. First time only: run `una-backup` to save what is on that drive, including the
   factory-reset image (`.gld`) the watch shipped with.
5. Copy the `.ota` to the root of the drive. Leave the `.gld` alone unless you
   want to change what a factory reset restores.
6. Eject, unplug, and choose **Continue** in the bootloader menu. On bootloader
   0.1.4 this starts the old firmware once more; the update is installed at
   the next reset (hold power ~20 s). It then stays at 100 % until one more
   reset.

To go back to stock, do the same with
`firmware/official-v1.7.0/UnaWatch-Kernel_1.7.0.ota`. The bootloader accepts any
version, including older ones.

## Zephyr

`zephyr/una/` holds the west manifest, the `una_watch` board, the display
driver and the app. The stock bootloader stays in place and starts MCUboot at
0x08060000, which starts the app:

| Flash | Contents |
|---|---|
| 0x08000000, 384 KB | UNA bootloader and shared C library; never written |
| 0x08060000, 128 KB | MCUboot |
| 0x08080000, 1536 KB | slot 0, the running image |
| 0x08200000, 1536 KB | slot 1, where an upload lands |
| 0x08380000, 512 KB | storage, unused so far |

```sh
west init -l zephyr/una && (cd zephyr && west update --narrow -o=--depth=1)   # once
west build -b una_watch --sysbuild -d build/zephyr zephyr/una/app
```

### Day to day, cable in, no buttons

```sh
west build -d build/zephyr && una-z flash   # ~30 s: upload, test boot, confirm
una-z sh kernel uptime                      # any shell command
una-z shot                                  # screenshot -> build/shot.png
una-z press 3                               # press SW3 (SW1-SW4, see the board dts)
```

`una-z flash` boots the new image once as a test and confirms it when its shell
answers. An image that crashes, hangs (8 s watchdog) or is never confirmed is
swapped back for the previous one at the next reset.

The watch is two USB serial ports, `2fe3:0004` "UNA Watch Zephyr": interface 0
is the shell, interface 2 is mcumgr. `/dev/una-watch` does not point at either.

### When the app does not come up

- **MCUboot recovery:** hold SW1 (top-left) through a reset. The screen keeps
  the UNA logo and the watch enumerates as "UNA Watch MCUboot". Then
  `smpmgr --port /dev/serial/by-id/usb-UNA_UNA_Watch_MCUboot_*-if00 image upload build/zephyr/app/zephyr/zephyr.signed.bin`
  followed by `... os reset`. This writes slot 0 directly.
- **Reset without a working app:** hold the power button (top-right) for about
  20 s; the PMIC power-cycles the watch. Keep holding for the UNA bootloader
  menu, release for a normal start.
- **First install, or MCUboot itself is broken:** pack MCUboot and the app into
  one `.ota` and install it like a kernel (below). `firmware/zephyr-<version>/`
  keeps one that is known to work.

```sh
una-pack build/zephyr/mcuboot/zephyr/zephyr.bin \
         --at 0x20000 build/zephyr/app/zephyr/zephyr.signed.bin \
         -o build/zephyr/una-zephyr.ota
```

### What protects the restore path

- An `.ota` holds only the kernel (flash from 0x08060000). The bootloader and the
  shared C library in the first 384 KB are not written by an update.
- The bootloader runs before the kernel and its menu is reached with the power
  button, so a kernel that does not boot can still be replaced.
- Not covered: a kernel that erases the bootloader itself, or a watch that
  cannot be powered off. The only fix then is the `.hex` (whole flash) over SWD,
  which needs a USB-C debug adapter that we do not have (see docs/README.md).
