# sm64coopdx for the PlayStation Vita

A port of [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx) to the PS Vita / PS TV.

**Status: experimental, but it runs.** Tested on a real PS Vita: it boots, renders, plays sound
and loads Lua mods and DynOS packs from the memory card. Expect bugs.

What has been seen working on hardware:

* Loading, the menus and in-game rendering through vitaGL.
* Audio through the system's main output.
* Mods from `ux0:data/sm64coopdx/mods` (character packs, large gameplay mods).
* 30 FPS holds in large levels such as Bob-omb Battlefield. 60 FPS is reached in smaller scenes
  but not in large ones, and heavy mods cost a lot of CPU time on top.

Not verified on hardware yet:

* Direct connection netplay (hosting and joining).
* The touch corner bindings and the most recent renderer speedups.
* PS TV, external controllers.

## What you need on the console

* A Vita or PS TV with HENkaku/Ensō and VitaShell.
* `libshacccg.suprx` in `ur0:data/` (the runtime shader compiler that every vitaGL homebrew
  needs). If you do not have it yet, search for "extract libshacccg.suprx" (ShaRKBR33D or the
  manual PSM Runtime method); most Vita homebrew setups already have it.
* Your own copy of the US version of Super Mario 64 (`.z64`, MD5 `20b854b239203baf6c961b850a4a51a2`).

## Installing

1. Install `sm64coopdx.vpk` with VitaShell.
2. Copy the ROM to `ux0:data/sm64coopdx/` (any file name ending in `.z64` works).
   The folder is created the first time the game is started.
3. Start the game. The first boot compiles and caches its shaders
   (`ux0:data/sm64coopdx/shader_cache`), so expect short hitches the first time something new
   is drawn. They are gone on later runs.

Everything the game writes lives in `ux0:data/sm64coopdx/`:

| File / folder       | Purpose                                                        |
|---------------------|----------------------------------------------------------------|
| `sm64config.txt`    | settings                                                       |
| `sm64_save_file.bin`| save data                                                      |
| `mods/`             | extra mods, same format as on PC                               |
| `dynos/packs/`      | extra DynOS packs                                              |
| `vita_log.txt`      | log of the current run (`vita_log_prev.txt`: the run before)   |
| `fatal_error.txt`   | message of the last fatal error, if any                        |

## Controls

| Vita            | Game                                   |
|-----------------|----------------------------------------|
| Left stick      | move                                   |
| Right stick     | camera (C buttons)                     |
| Cross           | A (jump)                               |
| Square          | B (punch)                              |
| Circle          | X                                      |
| Triangle        | Y                                      |
| L               | Z (crouch)                             |
| R               | R (camera mode)                        |
| D-pad           | D-pad                                  |
| Start           | pause                                  |
| Select          | player list                            |
| Touch screen    | mouse pointer in the menus             |

The four corners of the front touch screen and the four quadrants of the rear touch pad are
eight extra buttons ("Front TL/TR/BL/BR", "Rear TL/TR/BL/BR"). They are not bound to anything
by default; assign them in Options → Controls like any other button (select a bind slot, then
touch the corner).

Everything can be rebound in Options → Controls. Text fields (player name, IP address, chat)
open the system's on screen keyboard.

## Playing online

* **Direct connection** is supported: host on the Vita or join any sm64coopdx host by IP address.
  The Vita only speaks IPv4.
* **CoopNet** (the public lobby list) is not available yet, see below.
* The Discord integration and the auto updater are desktop features and are left out.

## Differences from the PC version

* Renders at 960x544 through [vitaGL](https://github.com/Rinnegatamante/vitaGL) with Cg shaders
  (`src/pc/vita/gfx_vitagl.c`). Three point texture filtering falls back to bilinear.
* The frame rate defaults to a fixed 30 FPS without interpolation. It can be raised in
  Options → Display, at a real CPU cost.
* MSAA can be set to 2x or 4x in Options → Display (applies after a restart).
* Textures larger than 1024 pixels are downscaled when they are uploaded.
* Mod audio (miniaudio) is mixed into the game's own audio stream.
* The menus use a fixed x0.7 scale when the scale option is set to Auto.
* The game can use about 128 MiB for its own data (Lua, DynOS, mods); very heavy mod setups that
  work on a PC may run out of memory.

## Known gaps / to do

* CoopNet (public lobbies) is missing: libjuice and coopnet have to be ported to the Vita first.
* Performance: the CPU-side display list processing (`src/pc/gfx/gfx_pc.c`) is the bottleneck,
  the GPU is mostly idle. Large levels do not reach 60 FPS, and audio stutters whenever the game
  drops below full speed.
* Startup takes long with many mods installed (about a minute and a half with a large collection).
* Netplay is untested, see the status above.
* No gyro and no PS TV multi controller support.
* When reporting a problem, please attach `ux0:data/sm64coopdx/vita_log.txt`.

## Building

The Vita target is part of the normal Makefile: `make TARGET_VITA=1` produces
`build/us_vita/sm64coopdx.vpk`.

Requirements:

* [VitaSDK](https://vitasdk.org) with `VITASDK` set and `$VITASDK/bin` in the `PATH`
* the VitaSDK packages `sdl2_vitagl` and `zlib` (`vdpm install sdl2_vitagl zlib`),
  this pulls in vitaGL, vitaShaRK, libmathneon and taiHEN as well
* a host toolchain for the asset tools: `gcc`, `g++`, GNU `make`, `python3` and the usual
  POSIX shell utilities (the same as for the regular PC build)

Recommended: replace the packaged vitaGL with one that has the shader cache, compiler logs and
scratch memory enabled, the port is set up to use all three:

```sh
git clone https://github.com/Rinnegatamante/vitaGL
cd vitaGL
make -j8 HAVE_SHADER_CACHE=1 HAVE_SHARK_LOG=1 USE_SCRATCH_MEMORY=1 NO_DEBUG=1 install
```

Then, in this repository:

```sh
make TARGET_VITA=1 -j8
```

Useful make variables: `PYTHON=/path/to/python3`, `VITA_TITLEID=SMCX00001`,
`VITA_TITLE="SM64 Coop DX"`, `DEBUG=1`.

### Building on Windows without MSYS2/WSL

The port was developed on Windows with the native VitaSDK toolchain
(`vitasdk-x86_64-w64-mingw32-*.tar.bz2` from <https://github.com/vitasdk/autobuilds/releases>)
and [w64devkit](https://github.com/skeeto/w64devkit) providing `gcc`, `make` and a POSIX shell.
Make sure the repository is checked out with LF line endings
(`git config core.autocrlf false && git config core.eol lf` before cloning or re-checking out).

### How the port is put together

| Piece                         | Where                                                         |
|-------------------------------|---------------------------------------------------------------|
| Build target and VPK packaging| `Makefile` (`TARGET_VITA`), `tools/vita/mkvpk.py`, `tools/vita/mklivearea.py` |
| Platform glue, memory budget  | `src/pc/vita/vita_platform.c`                                 |
| Renderer                      | `src/pc/vita/gfx_vitagl.c`                                    |
| Window, input                 | the regular SDL2 backends (`gfx_sdl.c`, `controller_sdl.c`) on top of SDL2's Vita port |
| Audio output                  | `src/pc/vita/audio_vita.c`: 32 kHz resampled to 48 kHz on the system's MAIN port (SDL2 would use the BGM port, which stayed silent on hardware) |
| Direct connection networking  | `src/pc/vita/socket_vita.c` (IPv4)                            |
| ROM lookup                    | `src/pc/rom_checker.cpp`                                      |
| Sound bank loading            | `src/audio/internal.h` (`AUDIO_FILE_PTR`), `src/audio/load.c` |
| Lua                           | built from source in `lib/lua/src` (Lua 5.3.6)                |

Things worth knowing when working on it:

* `arm-vita-eabi-gcc` uses one byte enums by default. The game is compiled with
  `-fno-short-enums` so that enums are ints like on every other platform (the network protocol
  and the Lua bindings depend on it). The system libraries are not, so do not pass structs
  containing enums across that boundary without checking them.
* The bundled sound bank data has a 64-bit layout. `AUDIO_FILE_PTR` keeps the structs that are
  mapped onto it 64-bit shaped on the 32-bit Vita.
* VitaSDK's `pthread_attr_init()` defaults to a 4 KiB stack. It is wrapped at link time
  (`__wrap_pthread_attr_init`) to use `_pthread_stack_default_user` instead.
* The program has to be linked with `-pthread` (the whole pthread library): libstdc++ only holds
  weak references to it, which silently turn into no-ops otherwise.

### Debugging aids

Files in `ux0:data/sm64coopdx/` that change the behaviour of the game:

* `debug_no_shaders.txt`: shader compiler errors are no longer fatal, nothing is drawn and the
  generated shader sources are written to the log. This allows running the game logic in Vita3K,
  which can not load `libshacccg.suprx` unless you copy it over from a real console
  (to `ur0/data/` inside the emulator's data folder).
* `debug_exit_after.txt`: contains a number of seconds after which the game exits cleanly.
* `debug_autohost.txt`: skips the main menu and hosts a game right away.
* `debug_warp.txt`: contains `<seconds> <level number>`, warps there once (9 = Bob-omb Battlefield).

Every ten seconds the log gets a block of statistics: memory, audio (peak level, queue, output
blocks, underruns), `perf:` (milliseconds per 30 Hz tick spent in game logic, Lua, audio,
network, rendering, buffer swap, idling) and two `gfx` lines that break the rendering time of a
frame down further (vitaGL draw submission, vertex transform, triangle setup, matrices,
interpolation, event pump). The vertex/triangle/matrix times are sampled (one call in eight).

Crash dumps (`ux0:data/psp2core-*.psp2dmp`) can be decoded with the bundled script, which
prints the faulting thread, its registers and the return addresses found on its stack:

```sh
python3 tools/vita/parse_core.py psp2core-xxxx.psp2dmp build/us_vita/sm64coopdx.elf
```

The ELF has to be the one the crashing VPK was built from.

A note on a class of bug that only shows up on real hardware: 32-bit ARM faults on NEON and
`ldrd`/`vldr` accesses that are less aligned than the type promises, which x86 and Vita3K both
tolerate. Anything that hands out memory for `Gfx`, `Mtx`, `Vtx` or lights has to return 8 byte
aligned pointers (see `POOL_ALIGNMENT` in `src/game/memory.c`).
