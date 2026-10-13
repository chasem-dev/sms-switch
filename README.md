# sms-switch

The [Super Mario Sunshine PC port](https://github.com/chasem-dev/sms-pc-port) built as Nintendo Switch homebrew: one `sms.nro` for the homebrew menu (libnx, devkitPro).

No game data is included.
Like the PC port, it reads the models, textures, levels, music and movies from **your own disc image** (GMSE01, North America, Rev 0) at run time.

**Status: early.** It builds, and in an emulator (Eden) it boots, plays the opening logos, and renders through the Switch's OpenGL stack.
It has not been run on a console yet; see [Status](#status).

## Build

The only requirement is Docker; the build runs in the official `devkitpro/devkita64` image.

```sh
git clone --recursive https://github.com/chasem-dev/sms-switch.git
cd sms-switch
./build.sh
```

That produces `build/sms.nro`.
[GitHub Actions](.github/workflows/build.yml) builds the same way on every push; pushing a `v*` tag (or running the workflow by hand with a version) publishes a release with `sms-switch.zip`, `sms.nro` and `gx_selftest.nro`.
The first build compiles about 600 game files; later builds only rebuild what changed.

| Command | What it does |
| --- | --- |
| `./build.sh` | configure (first time) and build `build/sms.nro` (and `build/gx_selftest.nro`) |
| `./build.sh --clean` | delete `build/` first |
| `JOBS=2 ./build.sh` | limit parallel compiler jobs |
| `SMS_SWITCH_NATIVE=1 ./build.sh` | use an installed devkitPro (`$DEVKITPRO`) instead of Docker |
| `SMS_SWITCH_DISC=/path/GMSE01.iso ./build.sh` | also build `build/sms-standalone.nro` with that image inside (see below); remembered for later builds, `SMS_SWITCH_DISC=` stops it |

The icon is SteamGridDB's box art for the game (`SMS_SWITCH_ICON=steamgriddb`, the default for local builds), fetched at build time and never stored here; `disc` uses the memory-card Mario from your disc, `default` libnx's icon (what releases use), or give a 256x256 JPEG.

Builds use ccache (in the `sms-switch-build` image, devkitA64 plus ccache, which `build.sh` makes the first time; the cache is `~/.cache/sms-switch/ccache`).
On a 4-core i5-6600K a first build takes about 3 minutes, a rebuild from scratch from the cache about 30 seconds, and a one-file change about 10 seconds.

### Installed on the HOME menu (NSP)

`SMS_SWITCH_KEYS=/path/to/prod.keys ./build.sh` also packs `build/nsp/sms-switch.nsp`: an installable title with its own HOME menu icon that starts the game with a game's full memory, no R-hold needed.
Its program is [nx-hbloader](https://github.com/switchbrew/nx-hbloader) (`third_party/`, the homebrew menu's own loader) set to start `sdmc:/switch/sms/sms-standalone.nro` (with `SMS_SWITCH_DISC`) or `sdmc:/switch/sms/sms.nro`; `SMS_SWITCH_NSP_NRO=sdmc:/...` picks another.
Install it with Goldleaf or DBI; the NRO stays on the SD card, so updating the game is copying a new NRO.

- The keys are your console's: boot the Lockpick_RCM payload from Hekate and dump them to `sdmc:/switch/prod.keys`. Only their path is remembered (`.switch-keys`); they are mounted read-only into the build and never leave your machine.
- Installing a self-made NSP needs sigpatches on the console.
- The icon is `SMS_SWITCH_ICON` (below), for the NRO and the NSP alike.

### With your disc image inside (personal use)

`SMS_SWITCH_DISC=/path/to/your.iso ./build.sh` also builds `build/sms-standalone.nro`: the same program with the image packed into its RomFS (the NRO's own read-only file system), so the SD card needs only that one file in `sdmc:/switch/sms/`.
It contains the game: keep it to yourself, never upload or share it (CI and releases never build it).
Re-packing the image adds a few seconds to builds that change the program.

## Install and play

1. Download `sms-switch.zip` from the [latest release](https://github.com/chasem-dev/sms-switch/releases) and extract it to the root of the SD card (it holds `switch/sms/sms.nro`), or copy your own `build/sms.nro` to `sdmc:/switch/sms/sms.nro`.
2. Put the disc image beside it: `sdmc:/switch/sms/` (or `sdmc:/switch/sms/rom/`), as `.iso`, `.gcm` or Dolphin `.ciso`, any file name.
3. Start it from the homebrew menu **through a game**: on the HOME menu hold R while starting any game (installed or cartridge) until the homebrew menu appears, then pick it. Nothing is installed; homebrew started that way gets the game's memory.
   Started from the Album (applet mode) it only shows a message: on a console it had 86 MiB left there, and Mesa could not create the window surface.

Everything else lives in the same folder:

| File | |
| --- | --- |
| `settings.txt`, `bindings.txt` | the PC port's settings and key bindings (see its README) |
| `env.txt` | `NAME=value` lines set as environment variables, for the PC port's switches (`SMS_SKIP_MOVIES=1`, `SMS_OVERLAY=1`, `SMS_FRAME_RATE=30`, ...) |
| `save/sms-port/card-a/` | the memory card |
| `cache/` | the shader cache |
| `sms.log` | the log of the last run (or the nxlink console, when started with `nxlink -s`) |

The game fills the screen (720p handheld, 1080p docked) with its 4:3 picture centred; `widescreen = 16:9` in `settings.txt` gives the PC port's widescreen.
Leaving through HOME ends the game as closing any game does; it does not return to the homebrew menu.

## Controls

Joy-Con and Pro Controllers work as the GameCube controller (the PC port's mapping: A/B/X/Y, + = Start, R = Z, ZL/ZR = L/R, sticks, d-pad).

| | |
| --- | --- |
| **L3 + R3** (press both sticks) | show or hide the performance overlay (frame rate, where each frame's time goes) |
| **−** with the overlay shown | the warp menu (stages and episodes) |

`SMS_OVERLAY=1` in `env.txt` shows the overlay from the start.

## How it works

The PC port is a native build of the decompilation: game code runs as AArch64 code, with the GameCube SDK replaced by host code (`sms-port/platform/`).
This repository adds what the Switch needs:

- **`sms-port/`**: the PC port, as a submodule at a pinned commit, built by its own CMake.
- **`patches/sms-port/`**: the Switch's cases in the PC port (CMake, a few `__SWITCH__` branches), applied to the submodule by `build.sh` (`tools/apply-patches.sh`, once each).
  They are written so they could go upstream, after which they are dropped here.
- **`switch/`**: the Switch runtime.
  - `src/low_memory.cpp`: the 64-bit game keeps pointers in 4-byte fields, so everything it can point at must lie below 4 GiB (sms-port `docs/64-BIT.md`).
    hbloader maps an NRO, its heap and every thread stack anywhere in the 39-bit address space.
    Before relocation, a replacement `__nx_dynamic` copies the still-unrelocated program to `0x10000000` (`svcMapProcessCodeMemory`, as hbloader maps an NRO) and enters the copy as hbloader would.
    `__libnx_initheap` then maps two windows of the heap below 4 GiB: MEM1 at `0x80000000` (elsewhere if the loader is in the way) and one for thread stacks, which a small `mmap` serves.
  - `src/stack_switch.cpp`: libnx maps every thread's stack again above 4 GiB, so threads that run game code switch onto their low stacks (as the 64-bit Windows build does).
  - `src/posix_shims.cpp`, `include/`: what newlib lacks (`dlsym` over the static SDL2, `backtrace`, `pread`, `sysconf`, ...).
  - `src/switch_main.cpp`: paths, the log, `env.txt`, the disc image search, exit.
- **Graphics**: the PC renderer unchanged in kind: OpenGL 3.3 core through SDL2 on devkitPro's Mesa (nouveau, OpenGL 4.3 on the Tegra X1).
  The GX shaders are generated at run time, which rules out deko3d without a shader compiler in the game.

## Testing without a console

`tools/emu/` runs `sms.nro` in [Eden](https://eden-emu.dev) (a yuzu-derived emulator), which loads NROs without keys or firmware:

```sh
tools/emu/run-host.sh EDEN_DIR DISC_IMAGE WORK_DIR [SECONDS]   # this machine's GPU, no window (SDL offscreen + EGL)
tools/emu/run.sh      EDEN_DIR DISC_IMAGE WORK_DIR [SECONDS]   # in a container on Xvfb (software GL: very slow)
```

`EDEN_DIR` is an extracted Eden AppImage (`Eden.AppImage --appimage-extract`), and the emulator's virtual SD card is under `WORK_DIR`.
Pictures come from the game itself: `SMS_SHOTS=300,900` and `SMS_SHOT_DIR=sdmc:/switch/sms/shots` in that SD card's `switch/sms/env.txt` capture the game's frames (`SMS_WINDOW_SHOTS` the screen) as PPM.

Two diagnostic NROs come with it:

- `build/gx_selftest.nro`: the PC port's GX self-test (pixel checks of the renderer) on the Switch's GL stack; report in `gx_selftest.log`.
- `tools/glprobe/`: one tile per OpenGL feature the renderer uses, PASS/FAIL in `glprobe.log`; also compiles and draws a `probe.vs`/`probe.fs` pair found beside it.
  Build it with `cmake -S tools/glprobe -B build/glprobe -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake` in the devkitPro image.

## Status

Verified in Eden 0.2.1 on this machine:

- the program moves itself below 4 GiB and the low windows map (MEM1 lands at `0x08000000` there, since Eden loads the NRO at `0x80xxxxxx`);
- disc, memory card, audio (DSP HLE), threads and the OpenGL 4.3 context come up;
- the opening logos render, the same picture as the PC build (compared by eye at field 300), full screen at 1080p.

Not yet:

- **A console.** Nothing here has run on hardware: performance (four 1 GHz Cortex-A57 cores), memory in practice, controllers (SDL's Joy-Con/Pro Controller mapping) and docked/handheld switching are untested.
- **Further in the emulator.** Past the logos Eden stops with `PRET flow analysis is not implemented`: Mesa's nouveau compiles some shader as a subroutine call, which Nintendo's compiler never emits and Eden cannot translate.
  Which shader that is was not found yet; the console runs such code natively.
- Leaving to the homebrew menu instead of ending the process (the game's threads and the low windows would have to be torn down first).
- An icon and a homebrew-menu entry name beyond the defaults; the PC port's HD texture packs, HD cutscenes and Eclipse mod on the Switch.

## License

The PC port (`sms-port/`) is MIT licensed (its `LICENSE`); the decompilation and Mesa, SDL2 and libnx carry their own licenses.
