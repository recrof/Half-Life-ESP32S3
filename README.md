# Half-Life for ESP32-S3

Work in progress: a port of Half-Life ([Xash3D FWGS](https://github.com/FWGS/xash3d-fwgs) engine,
[hlsdk-portable](https://github.com/FWGS/hlsdk-portable) game code, software renderer) to the
Seeed Wio Tracker L2 Pro (ESP32-S3, 8 MB PSRAM, 16 MB flash, 320x240 screen).

The engine and game sources are the `esp32s3` branches of
[recrof/xash3d-fwgs](https://github.com/recrof/xash3d-fwgs) and
[recrof/hlsdk-portable](https://github.com/recrof/hlsdk-portable), included as submodules.

No game data is included: you need your own copy of Half-Life (the `valve` folder, e.g. from
Steam).

## Getting the sources

```sh
git clone https://github.com/recrof/Half-Life-ESP32S3.git
cd Half-Life-ESP32S3
tools/setup.sh
```

`tools/setup.sh` fetches the submodules (only the third party parts of the engine the build
uses) and applies `patches/*.patch` to them.

## ESP32-S3 build

Needs ESP-IDF 5.5. `tools/idf_env.sh` uses an ESP-IDF already set up in the shell (`export.sh`),
or else the one installed by PlatformIO (`~/.platformio`).

```sh
. tools/idf_env.sh
idf.py set-target esp32s3     # first time only
idf.py build
tools/flash.sh                # builds and flashes (PORT=/dev/cu.usbmodemXXX if several boards)
```

Game data:

1. Copy the Half-Life `valve` folder to the root of a FAT formatted SD card (`/valve/liblist.gam`
   must exist) and put it in the board.
2. Optional but recommended (much faster model loading): build the model pack from the same data
   and write it to flash:

   ```sh
   python3 tools/mkassets.py /path/to/valve gamedata/assets.bin
   tools/flash.sh assets
   ```

Files can also be copied to the SD card over USB while the firmware runs: `tools/sdput.py`.
The serial console takes engine commands (`map c1a0`, `cpu`, `hlmem`...).

## Layout

| Path | |
|---|---|
| `sources/*.txt` | source files of each part (`xash/`, `hlsdk/`, `port/` prefixes), shared by all builds |
| `host/` | macOS/Linux build with SDL2: one binary with everything linked in, as on the ESP32 |
| `host/exports/*.txt` | functions each library exports to the engine (looked up by name) |
| `port/` | replacements for parts left out (voice chat, bzip2, RevEmu) and console input |
| `patches/` | changes to the engine's nested submodules (applied by `tools/setup.sh`) |
| `main/` | ESP32 firmware: boards, display, input, audio, serial console |
| `gamedata/valve` | Half-Life data for the host build (not in the repository) |

The engine loads its libraries (`filesystem_stdio`, `ref_soft`, `menu`, `client`, `server`) by
name from tables (`engine/platform/misc/lib_static.c`). Each library is linked into one
relocatable object that only keeps its `lib_<name>_exports` table global, so the libraries can
contain functions with the same names.

## Host build

Needs SDL2 (`sdl2-config`) and a C/C++ compiler.

```sh
make -C host -j8
mkdir -p gamedata && ln -s /path/to/valve gamedata/valve
cd gamedata
tail -f cmds | SDL_VIDEO_HIGHDPI_DISABLED=1 XASH3D_BASEDIR=$PWD ../host/hlhost -dev 2 -windowed -width 320 -height 240 +map c1a0
```

Lines appended to `gamedata/cmds` are console commands (`echo memlist >> cmds`,
`echo screenshot >> cmds`).

## Memory

Baseline with `XASH_LOW_MEMORY=2` on `c1a0` at 320x240: 30.6 MB in engine memory pools plus
8.3 MB of static data, for about 7.5 MB available on the ESP32-S3.
