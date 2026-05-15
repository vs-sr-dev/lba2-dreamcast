# Little Big Adventure 2 — Dreamcast port

A native [KallistiOS](https://github.com/KallistiOS/KallistiOS) port of the
**original 1997 Adeline source** of *Twinsen's Odyssey* / *Little Big Adventure 2*,
released by 2:21 in 2024 ([upstream `lba2-classic-community`](https://github.com/2point21/lba2-classic-community)).

> **Status: V0.9 — alpha.** The **first island is playable to completion**
> on Flycast (full story progression: all required scenes and transitions).
> The **second island loads cleanly** and initial exploration works. Optional
> side dialogs and zones were not exhaustively tested, but no regressions
> were encountered on validated content. Later islands, vehicles, mini-games,
> and the ending FMV remain unverified.

On Flycast: intro FMV, music, dialog voices, save/load, and the full DC
controller layout all work end-to-end across the validated scope. On real
Dreamcast hardware, boot reaches the EA logo and then stalls — first UART
capture has narrowed this to a render-side crash; see *Known issues* below.

## What works (validated on Flycast)

- **Boot to gameplay.** Engine boots, locale loads, Adeline+EA logos play, INTRO
  FMV plays, main menu, character creation, first scenes on the first island.
- **FMV.** `INTRO.SMK` plays at 30 fps stable via libsmacker `SMK_MODE_DISK`
  streaming through a 64 KB stdio buffer; video Huffman decode uses an
  iterative inlined fast path that drops worst-case complex-frame decode from
  73 ms to 29 ms on SH4.
- **Audio.** SFX via `snd_sfx`; long voice samples (IMA ADPCM dialog) routed
  to a dedicated `snd_stream` voice channel; music streamed from `Music/*.ogg`
  via `stb_vorbis` on a third `snd_stream` channel (no GD-ROM head contention
  with concurrent file I/O — a regression that hit the V2.3 CDDA path).
- **Save / load.** `vmu_pkg` saves on `/vmu/a1/`, recognized by the BIOS as
  *"LBA2 Save"*. Manual saves only (auto-save was disabled on DC because VMU
  flash writes at 1–5 KB/s freeze the engine for 6–7 s on every menu open).
- **Controls.** Full DC pad layout including a soft keyboard for save names.
  See [CONTROLS-DC.md](CONTROLS-DC.md).

## Known issues

- **Real-Dreamcast blocker (high priority, under diagnosis).** On real
  hardware the screen goes black after the EA logo. First UART capture
  obtained: boot reaches `KOS-AICA: snd_init OK`, then crashes with a
  **Data address error (read)** during the first scene's render pass.
  `addr2line` resolves the crash site to engine animation/object code
  (`GereAnimAction` called from `DrawCube`), suggesting a wild pointer
  reaching the `GET_S16`/`GET_S8` macros that walk packed scene data —
  consistent with a load-side failure earlier (HQR allocation, asset
  path, alignment) that doesn't fault on load but blows up on first
  dereference. Flycast tolerates it; real DC doesn't. More UART traces
  (with additional `LBA2_DC_TRACE` breadcrumbs in the scene-load path)
  needed to narrow it further.
- **Looping/continuous SFX persist past their source.** Some SFX that
  should stop when their in-game source stops (notably the moving-platform
  sound) keep playing until the scene changes. The engine likely fires
  `StopSample(handle)` with the playback handle when the source stops,
  but the KOS audio backend's handle-to-`snd_sfx`-channel mapping appears
  not to honor that contract correctly. Audible on Flycast.
- **Music start triple-stutter.** Every new music track starts with a brief
  three-step stutter before settling. Reproducible on Flycast and real DC;
  suspected AICA voice-register state leak from the previous stream end.
  Cosmetic, not blocking.
- **Soft-keyboard hint overflow.** The input hint line (*"<- ->: cycle  A: add
  …"*) spills off both screen margins. Cosmetic.
- **Untested past second island.** No full playthrough has been performed.
  Later islands, vehicles, mini-games, and the ending FMV have not been
  validated; expect surprises.

See [DEVLOG-DC.md](DEVLOG-DC.md) for the porting journey, traps, and
technical notes.

## Building

The Dreamcast build uses the [`simulant-dc`](https://hub.docker.com/r/kazade/simulant-dc) Docker image
(KOS toolchain pinned, `mkdcdisc` available). Docker Desktop or any working
`docker` on `PATH` is required.

```bash
# 1. ELF
./build-dc.sh                 # cmake configure + make -j8
./build-dc.sh clean           # rm -rf build-dc/
./build-dc.sh shell           # interactive shell in the container

# 2. Disc image (after the ELF builds)
LBA2_ASSETS_DIR=/path/to/retail/Common ./build-cdi.sh
```

`./build-dc.sh` writes the ELF to `build-dc/SOURCES/lba2.elf` (about 6 MB).
`./build-cdi.sh` packages the ELF together with the retail asset directory
into `lba2.cdi` (~525 MB; comfortably under the 700 MB CD-R cap).

To rebuild with debug/perf instrumentation enabled (boot trace breadcrumbs,
per-frame timing, libsmacker μs counters, once-per-stream KOS-AICA logs):

```bash
docker run --rm -v "$(pwd):/src" simulant-dc bash -c '
  source /opt/toolchains/dc/kos/environ.sh &&
  cmake -B build-dc \
    -DCMAKE_TOOLCHAIN_FILE=/src/cmake/dreamcast-kos.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLBA2_DEBUG_PERF=ON \
    -G "Unix Makefiles" -S /src &&
  make -C build-dc -j8'
```

## Retail assets

The retail LBA2 game data is **not** included in this repository and is **not
redistributable** — it is copyrighted by Adeline / 2:21. You must provide your
own legitimate copy. The GOG release of *"Twinsen's Little Big Adventure 2 Classic"*
is known to work; point `LBA2_ASSETS_DIR` at its `Common/` directory.

`build-cdi.sh` reads from that directory and only ships into the CDI:

| In retail        | On disc                      |
|------------------|------------------------------|
| `*.HQR/OBL/ILE/CFG/PAL` (root) | `/` |
| `Music/*.ogg`    | `/Music/`                    |
| `VOX/*.vox`      | `/vox/`                      |
| `VIDEO/*.hqr`    | `/video/`                    |

No other paths or special filenames are required.

## Running

- **Flycast.** Open `lba2.cdi`. Recommended: emulated VGA mode if your
  Flycast renderer supports it; the engine outputs 640×480.
- **Real Dreamcast.** Burn `lba2.cdi` to CD-R, or boot via `dreamcast-tool`
  over BBA / serial — but be aware the post-EA-logo blocker has not yet been
  diagnosed (see *Known issues*).

## License

This DC port inherits the upstream's GPL-2.0-only license — see [LICENSE](LICENSE).
Files authored from scratch for the Dreamcast backend (the `LIB386/AIL/KOS/`,
`LIB386/SVGA/KOS.CPP`, `LIB386/SYSTEM/KOS_BACKEND.CPP`, `cmake/dreamcast-kos.cmake`,
the `*_DC_STUB.CPP` shims, and the `build-*.sh` scripts) carry an SPDX
`GPL-2.0-only` header. libsmacker patches stay under its original LGPL-2.1+.

The retail LBA2 game data remains the property of Adeline / 2:21 and is not
covered by this license.
