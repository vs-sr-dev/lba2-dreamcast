# DEVLOG — Dreamcast port

A condensed log of what bit, what surprised, and what's worth knowing for
anyone porting a 1997 PC engine to Dreamcast / KOS in 2026. Every entry has
a fix that lives in the code; this is the *why* and the *gotcha*.

## Strategy

Chose **`2point21/lba2-classic-community`** as the base over [TwinEngine](https://github.com/2point21/TwinEngine).
TwinEngine is a from-scratch reimplementation; the community fork is the
**original Adeline 1997 source code** released by 2:21 in 2024, with x86
ASM blocks already validated-ported to C++ by upstream contributors and
SDL3-shaped backend boundaries already in place. Less reverse engineering,
more swapping out backends.

What we ended up replacing on DC:
- `LIB386/SVGA/SDL.CPP`        → `LIB386/SVGA/KOS.CPP` (KOS direct framebuffer + 565 blit)
- `LIB386/SYSTEM/*` (SDL3-using stack: `EVENTS`, `KEYBOARD`, `WINDOW`, `TIMER`, `MOUSE`, `STRING`, `DISPOS`, `AVAILMEM`)  → `LIB386/SYSTEM/KOS_BACKEND.CPP`
- `LIB386/AIL/SDL/*` (SDL audio + libsmacker + stb_vorbis glue) → `LIB386/AIL/KOS/{SOUND,STREAM,VIDEO_AUDIO}_KOS.CPP`
- `SOURCES/JOYSTICK.CPP`        → `SOURCES/JOYSTICK_DC_STUB.CPP` (Maple controller with bit-packed virtual scancodes)
- `SOURCES/CONSOLE/CONSOLE.CPP` → `SOURCES/CONSOLE/CONSOLE_DC_STUB.CPP` (no-op stubs; the dev console is not wired in yet)

Plus a `dc_sdl_compat.h` shim so that engine call sites in `PERSO.CPP`,
`PLAYACF.CPP`, `RES_DISCOVERY.CPP`, `DIRECTORIES.CPP` can keep their
`SDL_Log` / `SDL_GetTicks` / `SDL_Delay` syntax without dragging in SDL3.

## Hardware traps (SH4 / AICA / GD-ROM / VMU)

### SH4 has no hardware double

`double` in inner loops is software-emulated at ~50–200 cycles per op.
Desktop code that uses doubles tanks on Dreamcast. The libsmacker audio
resample loop (8-bit DPCM upsampled 2× into the AICA 16-bit ring) was
authored with `double` accumulators on the desktop side; on DC that
tanked the FMV frame budget. Switched to integer math (V2.7.2 fast path
in `VIDEO_AUDIO_RESAMPLE.CPP`).

**Lesson:** Audit any retro target for floating-point capability.
SH4 has hardware single-precision but no hardware double; PSP / PS1 / N64
are similarly asymmetric.

### libsmacker hot path is `_smk_huff16_lookup`, not `_smk_huff8_lookup`

Conventional wisdom from desktop profiles says audio is the hot path in
Smacker. On DC it isn't — the **video** decode walks four
huff16 trees with thousands of lookups per complex frame, and on SH4
the recursive `_smk_huff16_lookup` + extern bit-reader call dominates.
V2.7.7 inlined the bit-read and switched to an iterative tree walk;
worst-case complex-frame decode dropped from 73 ms to 29 ms.

**Lesson:** Don't trust desktop profiler results on retro targets.
The function-call overhead on a 200 MHz in-order SH4 changes which
optimization hill is actually steep.

### KOS stdio default buffer is tiny

KOS newlib for SH4 ships with a default stdio buffer around 1 KB.
libsmacker in `SMK_MODE_DISK` does an `fseek`+`fread` per frame, which
on a 1 KB buffer thrashes constantly. `setvbuf(fp, NULL, _IOFBF, 65536)`
on the libsmacker FILE* makes most chunk reads memcpy-fast and kills a
huge chunk of the V2.7 disk-streaming overhead.

### libsmacker `smk_open_memory` is unviable for long FMVs

`INTRO.SMK` is ~76 MB decompressed. The Dreamcast has 16 MB of main RAM.
The original V2.6 path tried `smk_open_memory(buffer, size)` on the
already-loaded HQR record — instant OOM. V2.7 switched to
`smk_open_filepointer(fp, SMK_MODE_DISK)` and decoded streaming.

We added a pre-check in `PLAYACF.CPP` that consults `HQF_ResSize` *before*
attempting the allocation, so the engine fails-soft on broken / oversized
records instead of running off into a 76 MB malloc that never returns.

### KOS `snd_stream` callback must fill `smp_req` fully

If a streaming decoder returns short (less than `smp_req` bytes), AICA
reads stale ring contents for the missing tail and you hear chunk
boundaries. Fix: the snd_stream callback must inner-loop the decoder
until `smp_req` is satisfied; pad with silence only on true EOF.

This bit us on the music start "triple stutter" symptom which is
*partially* fixed by the inner-loop pattern but **still reproduces** —
suspected AICA voice register state leak from the previous stream's tail.
Cosmetic, on the known-issues list.

### KOS `snd_stream` pump pattern (no threads)

LBA2 is single-threaded by design (the engine runs at 60 Hz with a fixed
frame budget; no worker threads). KOS supports this if you call
`snd_stream_poll(handle)` explicitly per frame. We pump three concurrent
streams (music / voice / video) by calling `AudioStreamPump()` from the
frame timer; the shared callback dispatches by handle to one of three
push-mode decoders.

### KOS `snd_sfx` never sign-flips 8-bit WAV

AICA reads 8-bit samples as **signed**, but `.WAV` files store 8-bit as
**unsigned** (zero is 0x80). Loading a SAMPLES.HQR `.WAV` raw into
snd_sfx produced the famous "raspy AM-radio distortion" on every SFX.
Fix: XOR each byte with 0x80 in the load path.

### KOS CDDA truncates at 1 s under concurrent file I/O

`cdrom_cdda_play` works fine at the menu (no concurrent reads) but dies
about 1 s in once gameplay starts loading scene HQRs. The single-head
GD-ROM can't seek between CDDA tracks and data sectors fast enough.
V2.5 punted the music to `stb_vorbis` on top of `snd_stream`: each OGG
file is read once into RAM, then decoded out of RAM with no further
GD-ROM contention. This is the path that survived to V2.9.6.

### Maple bus VMU lookup costs ~50–100 ms

A naive "is there a save here?" call into KOS' VMU file API costs 50–100 ms
even for a *missing* file. Calling that on every save-menu navigation
froze the cursor. Cache the VMU directory once after the first read and
short-circuit the per-cell lookup.

### VMU writes are 1–5 KB/s

VMU flash is single-block writes serialized over the Maple bus, ~1–5 KB/s
sustained. A 30 KB save block takes 6–7 seconds. The engine's
`AutoSaveGame` and `CurrentSaveGame` paths fire on every Start press —
unusable on DC. Both are short-circuited on the DC build; saves are
manual-only via the soft keyboard flow.

### `vmu_pkg`, not raw bytes

KOS validates the `vmu_pkg` CRC on read. Writing raw bytes "looks like"
it works (file appears on the VMU) but the BIOS won't recognize it as a
game save. Use `vmu_pkg_build` with the 16×32 monochrome icon and the
"LBA2 Save" descriptor.

## Software traps (engine + tooling)

### `LBA2.CFG` silently overrides compiled-in defaults

`GamepadKeysDefault[]` only takes effect on the very first run, before
`LBA2.CFG` exists. Once `LBA2.CFG` is written, every subsequent boot
reads the bindings from there and ignores the compiled-in array. This
made several iterations of "I changed the default mapping and nothing
moved" mysterious. On DC there's no remap UI, so we hardcode the bindings
in `INPUT.CPP` *after* the CFG load to bypass the CFG entirely.

### LBA stick is binarized — don't cross-mirror DPad and Stick

The engine's input bus is a `Word` of `I_*` bit flags — there is no
analog precision in the gameplay code. If you bind both DPad and Stick
to the same input bit, holding the stick + tapping the DPad causes the
bit to fire twice in adjacent frames, which the engine reads as a
double-tap. Invisible during early movement-only testing; surfaces
on dodge / spell shortcuts where double-fire skips frames.

### DC analog triggers fire BOTH `TRIGGER` and `SHOULDER` virtual scancodes

Pressing the L trigger reports `K_GAMEPAD_LTRIGGER` *and*
`K_GAMEPAD_LSHOULDER` simultaneously through KOS' Maple bus. If you
bind one to a movement direction and the other to a menu-opening action
that has a "wait for the input to clear" loop (`while (Input & I_JOY)`),
holding the trigger deadlocks the menu. V2.9.6 unbound L→I_DOWN to
break this; left it as L→I_COMPORTEMENT only.

### `CheckKey` must replicate SDL's bit-packed virtual scancodes

Naive `return TabKeys[key]` works for physical keyboard keys but breaks
for every gamepad bit ≥ 256, because SDL's gamepad codes are bit-packed
above the keyboard range. Copy the SDL ternary verbatim into the KOS
`CheckKey` implementation; the bit-pack contract is shared by both
backends.

## Performance: measure first, optimize second

V2.7.2 and V2.7.3 burned two iteration cycles on theory-driven fixes
("must be the IMA decoder", "must be palette sync") that *seemed*
plausible from desktop profiler experience and turned out to be wrong.
A single afternoon of `timer_us_gettime64()` instrumentation around the
PLAYACF inner loop showed `_smk_huff16_lookup` was actually the hot
path (V2.7.4 → V2.7.7).

**Lesson:** On retro targets, theory-driven optimization is wasted
iterations. Always pair speculative fixes with timing.

The instrumentation lives in the code today, gated behind
`-DLBA2_DEBUG_PERF=ON`. Off by default; flip on when chasing the next
hot spot or for the UART-side diagnostic on the real-HW blocker.

## Build & test scope

Built and tested on:
- **Flycast** (Windows host, OpenGL renderer): boots cleanly, runs the
  intro and the first transitions on the first island feature-complete.
- **Real Dreamcast** (CD-R burn): boots through the static splash and
  EA logo, then black screen post-EA-logo. Awaiting UART-USB serial
  capture for diagnosis. The boot path is breadcrumbed; once the cable
  arrives, the trace will narrow the blocker down to a single subsystem.

The full game has **not** been played through. Adopters should expect
surprises on later islands, vehicles, mini-games, and the ending FMV.

## Open issues

- Real-DC post-EA-logo black screen (high; awaiting UART).
- Music start triple-stutter (cosmetic; AICA voice register leak suspected).
- Soft-keyboard hint string spills off both screen margins (cosmetic).
- Engine completeness past first island is unverified.
