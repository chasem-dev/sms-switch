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
[GitHub Actions](.github/workflows/build.yml) builds the same way on every push, `sms.nro` and (with [eclipse.yml](.github/workflows/eclipse.yml)) `sms-eclipse.nro`; pushing a `v*` tag (or running the workflow by hand with a version) publishes a release with `sms-switch.zip` (both NROs), `sms.nro`, `sms-eclipse.nro` and `gx_selftest.nro`.
Releases carry NROs only: no game data, no art (libnx's icon), and no NSPs, which need a console's keys (build them yourself, below).
The first build compiles about 600 game files; later builds only rebuild what changed.

| Command | What it does |
| --- | --- |
| `./build.sh` | configure (first time) and build `build/sms.nro` (and `build/gx_selftest.nro`) |
| `./build.sh --clean` | delete `build/` first |
| `JOBS=2 ./build.sh` | limit parallel compiler jobs |
| `SMS_SWITCH_NATIVE=1 ./build.sh` | use an installed devkitPro (`$DEVKITPRO`) instead of Docker |
| `SMS_SWITCH_DISC=/path/GMSE01.iso ./build.sh` | also build `build/sms-standalone.nro` with that image inside (see below); remembered for later builds, `SMS_SWITCH_DISC=` stops it |
| `SMS_SWITCH_ECLIPSE=1 ./build.sh` | also build `build/eclipse/sms-eclipse.nro`, Super Mario Eclipse (see below); remembered, `SMS_SWITCH_ECLIPSE=0` stops it |
| `SMS_SWITCH_ECLIPSE_DISC=/path/eclipse.iso ./build.sh` | that, and `build/eclipse/sms-eclipse-standalone.nro` with the Eclipse image inside; remembered, `SMS_SWITCH_ECLIPSE_DISC=` stops it |

The icon is SteamGridDB's box art for the game (`SMS_SWITCH_ICON=steamgriddb`, the default for local builds), fetched at build time and never stored here; `disc` uses the memory-card Mario from your disc, `default` libnx's icon (what releases use), or give a 256x256 JPEG.

Builds use ccache (in the `sms-switch-build` image, devkitA64 plus ccache, which `build.sh` makes the first time; the cache is `~/.cache/sms-switch/ccache`).
On a 4-core i5-6600K a first build takes about 3 minutes, a rebuild from scratch from the cache about 30 seconds, and a one-file change about 10 seconds.

### Installed on the HOME menu (NSP)

`SMS_SWITCH_KEYS=/path/to/prod.keys ./build.sh` also packs `build/nsp/sms-switch.nsp` ([installing it](#install-on-the-home-menu-nsp-with-dbi)): an installable title with its own HOME menu icon that starts the game with a game's full memory, no R-hold needed.
Its program is [nx-hbloader](https://github.com/switchbrew/nx-hbloader) (`third_party/`, the homebrew menu's own loader) set to start `sdmc:/switch/sms/sms-standalone.nro` (with `SMS_SWITCH_DISC`) or `sdmc:/switch/sms/sms.nro`; `SMS_SWITCH_NSP_NRO=sdmc:/...` picks another.
Install it with Goldleaf or DBI; the NRO stays on the SD card, so updating the game is copying a new NRO.
With the Eclipse build (below) it also packs `build/nsp/sms-eclipse.nsp`, a second title ("Super Mario Eclipse", its own icon) that starts `sdmc:/switch/sms/sms-eclipse-standalone.nro` (with `SMS_SWITCH_ECLIPSE_DISC`) or `sdmc:/switch/sms-eclipse/sms-eclipse.nro`; `SMS_SWITCH_ECLIPSE_NSP_NRO=sdmc:/...` picks another.

- Only the keys' path is remembered (`.switch-keys`); they are mounted read-only into the build and never leave your machine.
- The icon is `SMS_SWITCH_ICON` (below), for the NRO and the NSP alike.

### With your disc image inside (personal use)

`SMS_SWITCH_DISC=/path/to/your.iso ./build.sh` also builds `build/sms-standalone.nro`: the same program with the image packed into its RomFS (the NRO's own read-only file system), so the SD card needs only that one file in `sdmc:/switch/sms/`.
It contains the game: keep it to yourself, never upload or share it (CI and releases never build it).
Re-packing the image adds a few seconds to builds that change the program.

### Super Mario Eclipse (`sms-eclipse.nro`)

`SMS_SWITCH_ECLIPSE=1 ./build.sh` also builds `build/eclipse/sms-eclipse.nro`: the PC port's [Super Mario Eclipse build](sms-port/docs/ECLIPSE.md), in which the mod's code (Eclipse, BetterSunshineEngine and BetterSunshineMoveset, fetched at pinned revisions on the first build into `build/eclipse/eclipse-src/`) is compiled with clang for AArch64 and linked into the game.
It is a second program, in its own build tree (`build/eclipse/`, with the same compiler cache), and needs the build image's clang (`tools/docker/Dockerfile`).
It plays the Eclipse disc (`GMSE04`), which you make from your own Sunshine image with the mod's patch (`python3 sms-port/tools/mods/get.py eclipse`, see `ECLIPSE.md`).
It lives in its own folder, `sdmc:/switch/sms-eclipse/` (disc image, `settings.txt`, `env.txt`, saves and shader cache as for `sms.nro`, below), so the two games never share a memory card or settings.
`SMS_SWITCH_ECLIPSE_DISC=/path/to/eclipse.iso` also packs that image into `build/eclipse/sms-eclipse-standalone.nro`, as `SMS_SWITCH_DISC` does for Sunshine (personal use only).
Its icon is the Eclipse disc's memory-card icon, or `SMS_SWITCH_ECLIPSE_ICON=/path/to/icon.jpg` (256x256, for the NRO and its NSP; remembered, `SMS_SWITCH_ECLIPSE_ICON=` stops it).

## Install and play

1. Download `sms-switch.zip` from the [latest release](https://github.com/chasem-dev/sms-switch/releases) and extract it to the root of the SD card (it holds `switch/sms/sms.nro`, and `switch/sms-eclipse/sms-eclipse.nro` for Super Mario Eclipse, below), or copy your own `build/sms.nro` to `sdmc:/switch/sms/sms.nro`.
2. Put the disc image beside it: `sdmc:/switch/sms/` (or `sdmc:/switch/sms/rom/`), as `.iso`, `.gcm` or Dolphin `.ciso`, any file name.
3. Start it from the homebrew menu **through a game**: on the HOME menu hold R while starting any game (installed or cartridge) until the homebrew menu appears, then pick it. Nothing is installed; homebrew started that way gets the game's memory. Or [install it on the HOME menu](#install-on-the-home-menu-nsp-with-dbi), with no R-hold.
   Started from the Album (applet mode) it only shows a message: on a console it had 86 MiB left there, and Mesa could not create the window surface.

Everything else lives in the same folder:

| File | |
| --- | --- |
| `settings.txt`, `bindings.txt` | the PC port's settings and key bindings (see its README) |
| `env.txt` | `NAME=value` lines set as environment variables, for the PC port's switches (`SMS_SKIP_MOVIES=1`, `SMS_OVERLAY=1`, `SMS_FRAME_RATE=30`, ...) |
| `save/sms-port/card-a/` | the memory card |
| `cache/` | the shader cache |
| `sms.log` | the log of the last run (or the nxlink console, when started with `nxlink -s`) |

The game fills the screen (720p handheld, 1080p docked) in 16:9 widescreen at 60 fps, with the gameplay HUD at the screen's edges and the camera stick inverted on both axes; `settings.txt` changes any of that (`widescreen = off` for the original 4:3, `widescreen_hud = centre`, `frame_rate = 30`, `camera_invert_x = 0`, `camera_invert_y = 0`).
Button prompts in the game's text show the Switch's buttons (`button_prompts = gamecube` for the game's own); the images are `prompts/switch.png` beside the NRO, written there on the first start, and can be replaced (eight 64x64 icons in a row: A B X Y Z L R C-stick, here A B X Y R ZL ZR and the right stick; `tools/make_prompts.py` draws the default).
A B X Y are the Switch buttons labelled so, as the prompts show them (devkitPro's SDL had mapped them by position, putting the GameCube's A on the Switch's B); `SDL_GAMECONTROLLERCONFIG` in `env.txt` replaces that mapping, and `bindings.txt` rebinds buttons as on the PC.
Leaving through HOME ends the game as closing any game does; it does not return to the homebrew menu.

## Install on the HOME menu (NSP, with DBI)

An NSP puts the game on the HOME menu with its own icon, so it starts like any installed game, with no R-hold.
It is a small launcher: the game itself stays the NRO on the SD card.
Releases include no NSPs, because making one takes your own console's keys; you build them yourself, once.

You need a Switch running Atmosphère with sigpatches (a self-made NSP is not signed by Nintendo), [DBI](https://github.com/rashevskyv/dbi) or Goldleaf, and a computer with Docker and git.

1. **Dump your keys.** In Hekate, run the Lockpick_RCM payload and dump the keys; it writes `sdmc:/switch/prod.keys`. Copy that file to your computer, and keep it private.
2. **Build the NSPs:**
   ```sh
   git clone --recursive https://github.com/chasem-dev/sms-switch.git
   cd sms-switch
   SMS_SWITCH_KEYS=/path/to/prod.keys SMS_SWITCH_ECLIPSE=1 ./build.sh
   ```
   That makes `build/nsp/sms-switch.nsp` (Super Mario Sunshine) and `build/nsp/sms-eclipse.nsp` (Super Mario Eclipse); leave out `SMS_SWITCH_ECLIPSE=1` for Sunshine alone.
3. **Put the games on the SD card**, as in [Install and play](#install-and-play) (the NSPs start these files):

   | Game | Folder | Files |
   | --- | --- | --- |
   | Super Mario Sunshine | `sdmc:/switch/sms/` | `sms.nro` and your GMSE01 disc image |
   | Super Mario Eclipse | `sdmc:/switch/sms-eclipse/` | `sms-eclipse.nro` and your GMSE04 Eclipse disc image |

4. **Copy the `.nsp` files** to the SD card (anywhere, for example its root).
5. **Install them with DBI:** open DBI from the homebrew menu, browse the SD card, select each `.nsp` and install it. Delete the `.nsp` files afterwards if you like.
6. **Play:** start "Super Mario Sunshine" or "Super Mario Eclipse" from the HOME menu.

To update a game, copy its new NRO over the old one; the installed NSP keeps working.
Build and install the NSP again only to change its icon or the file it starts (see [Installed on the HOME menu](#installed-on-the-home-menu-nsp)).

## Controls

Joy-Con and Pro Controllers work as the GameCube controller (the PC port's mapping: A/B/X/Y, + = Start, R = Z, ZL/ZR = L/R, sticks, d-pad).

| | |
| --- | --- |
| **L3 + R3** (press both sticks) | show or hide the performance overlay (frame rate, where each frame's time goes) |
| **−** with the overlay shown | the warp menu (stages and episodes) |

`SMS_OVERLAY=1` in `env.txt` shows the overlay from the start.

## Performance

Gameplay runs at 60 fps (`frame_rate = 30` in `settings.txt` for the GameCube's rate, half the work).
Each frame is the game's own code, the GL driver's work for its draws (Mesa, on a thread of its own) and the GPU's; the console's A57 cores are slow enough that the first two decide the frame rate.
What the Switch build does about it:

- **The swap is queued, not waited for** (`SMS_GX_ASYNC_PRESENT`, `patches/sms-port/0007`): the game's thread queues the frame's present and swap for the GL thread and starts the next frame, so its game code runs while the GL thread and the GPU finish the last one.
  The game's thread used to wait for the swap, so the game, the driver and the GPU took turns every frame.
  `SMS_GX_ASYNC_PRESENT=0` in `env.txt` brings the wait back, to compare.
- **Threads** (`switch/src/performance.cpp`): the game's thread starts on core 0, the GL thread on core 1 and the rest on core 2 or 0, each free to move to any of the three; the texture pack's background work runs below the game's priority, since Horizon does not share a core between threads of equal priority by time.
  The CPU and GPU run at the console's own clocks; the port never raises them.
- **Widescreen** draws are placed without scanning their vertices over and over (`patches/sms-port/0006`, from the Android build).

To see where a frame's time goes, show the overlay (L3 + R3): `game`, `GX`, `present`, `swap` and `idle` are the game thread's milliseconds per frame, and `GL thread: busy` the GL thread's (`swapping` is the part of it in `SDL_GL_SwapWindow`, which waits for the display and the GPU).
With the swap queued, `swap` on the game thread is its wait for the GL thread to finish the last frame: high there means the GL thread is the limit, high `game` that the game's own code is.
`SMS_TIMING_LOG=1` in `env.txt` writes the same breakdown to `sms.log` every 2 seconds without the overlay, and `SMS_GX_STATS=120` adds draws, uploads and GL calls per frame.

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

## HD textures

The PC port's texture packs work here too (`sdmc:/switch/sms/mods/textures/`), but a pack made for PCs has textures far larger than the Switch can hold: the UHD pack's 157 textures of 4096-16384 pixels are 2.1 GiB of its 3 GiB, up to 256 MiB of GPU memory each.
`tools/texpack/make-switch-pack.sh PACK_DIR OUT_DIR [MAX]` makes the Switch's copy: those cut to at most 2048 pixels (their own mip levels where they have them, else decoded, halved and encoded again with [bc7enc](https://github.com/richgel999/bc7enc_rdo)), everything else as it is. The UHD pack becomes 1.3 GiB in about 90 seconds.
Copy OUT_DIR's contents to `sdmc:/switch/sms/mods/textures/`, or pack them into `sms-standalone.nro` with `SMS_SWITCH_TEXTURES=OUT_DIR ./build.sh` (with `SMS_SWITCH_DISC`; remembered like it): one 2.9 GB file copies over USB (MTP) in about 4 minutes, where its 2400 files one by one take hours. A pack on the SD card is read first, then the packed one; Super Mario Eclipse, without one of its own, reads the one in `sms-standalone.nro` (beside it, or in `sdmc:/switch/sms/`), so the card holds a single copy; `SMS_TEXTURE_PACKS=0` turns them off for a comparison, and at most 1 GiB is kept in memory (`SMS_TEXTURE_PACK_MB`).

## Shader cache

devkitPro's Mesa is built without its disk shader cache, so with it every area compiles its shaders again on each visit (2.7-4.4 s per load on a console).
`tools/mesa/build-mesa.sh` builds the same Mesa with the cache on into `build/mesa/` (Docker, a few minutes), and `./build.sh` links it from then on (the CMake log says `Mesa with the shader cache (<id>)`).
With it, compiled programs are kept in `sdmc:/switch/sms/cache/` and each stage's programs are made from there while it loads.

`switch/shader-cache/gx-programs-64.bin` is a full cache: every program the game is known to draw with (made in Eden with `SMS_GX_SHADER_WARMUP_MS=1000000 SMS_GX_SHADER_WARM_DRAW=0`, plus those a console session added).
Copy it to `sdmc:/switch/sms/cache/sms-port/` and even the first visit to an area loads from it.
It only fits the Mesa build named in `switch/shader-cache/mesa-cache-id`; with any other the game notices and starts the cache over.
Release builds use the installed Mesa for now, which has no program binaries, so they neither use nor need it.

## Quick test loop (nxlink)

To try a build or an option without copying anything to the SD card:

1. Once: `sms-standalone.nro` (or a disc image) in `sdmc:/switch/sms/`.
2. On the Switch: open the homebrew menu through a game (hold R), press **Y** (network loader).
3. Here: `./build.sh && tools/nxlink.sh SMS_GX_SCALE=2` sends the 12 MB `build/sms.nro` over Wi-Fi to `sdmc:/switch/sms/sms.nro`, starts it with those options, and shows its log as it runs.

`NAME=value` arguments act as `env.txt` lines; a plain `sms.nro` there takes its disc from `sms-standalone.nro` beside it.

With [sys-botbase](https://github.com/olliz0r/sys-botbase) on the console (it presses buttons and takes screenshots over Wi-Fi), `tools/switch-dev.py` needs no hands at all: it closes what runs, opens the network loader from the HOME menu's selected game (holding R through the user picker), sends the NRO, then connects to the program itself (`switch/src/dev_link.cpp`, port 28773) for the `--file`s to go beside it, its log in `logs/switch/` and `--shots` window captures as PNGs, so nothing on the PC has to accept a connection:

```sh
tools/switch-dev.py run build/sms.nro --path sms/sms-dkgl.nro --file build/dkgl-programs.pack \
    --secs 90 --shots 15 SMS_SWITCH_RENDERER=deko3d
tools/switch-dev.py shot now.jpg
```

Use the sys-botbase release of the console's firmware era: builds from v2.4 on carry a debug-flags capability that firmware before 19.0.0 refuses (`0xFC01`), so for 16.x that is v2.31. `tools/botstart/` checks whether it runs, and with `NSP=` installs another build, from the homebrew menu's network loader.

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
- An icon and a homebrew-menu entry name beyond the defaults; the PC port's HD cutscenes on the Switch.
- **Super Mario Eclipse** is barely tested. In Eden its three modules load (846 patches) and BetterSunshineEngine's first-boot settings screen draws, as on the PC, but it went no further there in two and a half minutes (Eden logs shader-decoder asserts). The first console run crashed opening the window (BetterSunshineEngine's `itoa` in place of the C library's, fixed since); the fixed build has not been run on a console yet.

## License

The PC port (`sms-port/`) is MIT licensed (its `LICENSE`); the decompilation and Mesa, SDL2 and libnx carry their own licenses.
