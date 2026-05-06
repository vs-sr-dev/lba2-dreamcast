// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// LIB386/AIL/KOS/STREAM_KOS.CPP
// V2.4–V2.6 streaming framework for the KOS AICA backend.
//
// Three independent stream channels share one snd_stream callback that
// dispatches by handle:
//   CH_MUSIC  : looped OGG via stb_vorbis decoded into AICA on demand.
//               Replaces the V2.3 CDDA path (kills the in-game 1s-truncate
//               bug — no more concurrent GD-ROM head contention because
//               the OGG file is read once and decoded from main RAM).
//   CH_VOICE  : one-shot raw PCM from a caller-owned buffer (BufSpeak in
//               MESSAGE.CPP). Long voice files exceed snd_sfx's 65534-sample
//               limit; SOUND_KOS_BACKEND::PlaySample now reroutes anything
//               that doesn't fit through snd_sfx to PlayVoicePcm here.
//   CH_VIDEO  : push-mode PCM ring fed by libsmacker frame callbacks
//               (PLAYACF.CPP via VIDEO_AUDIO_KOS.CPP). Producer pushes,
//               consumer drains in the snd_stream callback.
//
// Pump model: AudioStreamPump() is called once per frame from
// LIB386/SYSTEM/KOS_BACKEND.CPP::ManageTime. Each active channel calls
// snd_stream_poll(hnd) to refill the AICA-side ring with fresh PCM.

#include "AIL.H"

#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>

#include <SYSTEM/LOGPRINT.H>

extern "C" {
#include "stb_vorbis.h"
}

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// AICA ring per stream. KOS requires a power-of-2 byte count.
#define STREAM_RING_BYTES 0x10000

// PCM bytes returned per snd_stream callback invocation. snd_stream_poll
// can request up to ~half-ring (~32 KB) in a single call — we MUST be able
// to fulfil that fully or AICA reads stale ring bytes for whatever we
// short-changed it on (audible as the "2–3 click stutter at start" the
// 4 KB chunk size produced). 32 KB exactly matches the typical KOS
// snd_stream half-buffer request size and lands well under one frame
// of SH4 work for both the OGG and PCM paths.
#define CB_CHUNK_BYTES 32768

#define CH_MUSIC 0
#define CH_VOICE 1
#define CH_VIDEO 2
#define CH_COUNT 3

// Push-mode FMV ring. 256 KB = ~3 s at 22050 Hz mono 8-bit, ~1.4 s
// at 22050 Hz stereo 16-bit. libsmacker prefills 25–30 frames worth
// (~1–2 s) before kicking ResumeVideoAudio so this is enough.
// Power of two so we can mask wrap.
#define VIDEO_RING_BYTES (256 * 1024)
#define VIDEO_RING_MASK  (VIDEO_RING_BYTES - 1)

struct StreamChannel {
    snd_stream_hnd_t hnd;
    int active;
    int paused;
    int freq;
    int channels;
    int bits;
    int volume;
    int loop;

    // OGG path (CH_MUSIC)
    stb_vorbis    *vorbis;
    unsigned char *vorbis_file_buf;  // entire .ogg slurped into main RAM
                                     // up front so the decode callback
                                     // never blocks on /cd/ I/O.
    int            vorbis_file_size;

    // Raw PCM path (CH_VOICE) — base may be caller-owned (BufSpeak) or
    // an internally-malloc'd decode buffer (e.g. IMA-ADPCM voice). When
    // pcm_owned is non-NULL, StopChannel frees it.
    const uint8_t *pcm_base;
    void   *pcm_owned;
    size_t pcm_size;
    size_t pcm_cursor;
    int    pcm_unsigned8; // bias-flip on the fly (AICA reads 8-bit signed)
    int    pcm_eof;        // sticky once cursor reaches pcm_size
    size_t pcm_drain_bytes; // bytes of silence handed to AICA after EOF
                             // — when this exceeds STREAM_RING_BYTES we know
                             // every byte of actual audio has rolled out of
                             // the AICA ring and the channel can be torn down.
                             // Counts in callback (== AICA-driven) so it's
                             // accurate; counting per-pump-call would race
                             // with the actual AICA drain rate.

    // Push ring (CH_VIDEO)
    uint8_t *push_ring;
    volatile size_t push_w;
    volatile size_t push_r;

    // Stable buffer the callback hands back to AICA (32-byte aligned).
    alignas(32) uint8_t cb_buf[CB_CHUNK_BYTES];
};

static StreamChannel chans[CH_COUNT];
static int stream_initialized = 0;
// Dedicated silence buffer for the "no channel matches hnd" fallback path.
// Earlier versions reused chans[0].cb_buf which trashed music PCM mid-flight
// whenever a stale callback (e.g. a torn-down voice) fired.
alignas(32) static uint8_t silence_buf[CB_CHUNK_BYTES];

// ─────────────────────────────────────────────────────────────────────────
// snd_stream callback — common dispatch for all three channels.

static void *StreamCallback(snd_stream_hnd_t hnd, int smp_req, int *smp_recv) {
    StreamChannel *c = NULL;
    for (int i = 0; i < CH_COUNT; i++) {
        if (chans[i].hnd == hnd && chans[i].active) { c = &chans[i]; break; }
    }
    int cap = (smp_req > CB_CHUNK_BYTES) ? CB_CHUNK_BYTES : smp_req;
    cap &= ~31; // 32-byte align
    if (cap <= 0 || !c) {
        // Stale callback (e.g. a voice ring still draining after we tore it
        // down). Hand back the dedicated silence buffer — never reuse a live
        // channel's cb_buf, AICA could be reading it while we memset.
        memset(silence_buf, 0, CB_CHUNK_BYTES);
        *smp_recv = CB_CHUNK_BYTES;
        return silence_buf;
    }

    int produced = 0;

    if (c->vorbis) {
        // OGG: 16-bit interleaved. KOS's snd_stream_fill calls our callback
        // exactly once and writes only `got_bytes` into the AICA ring; any
        // shortfall stays as stale data that AICA will play as garbage.
        // stb_vorbis_get_samples_short_interleaved is allowed to return
        // fewer samples than requested (short packets, first frame after
        // open, decoder-internal buffering). So we LOOP it inside the
        // callback until we've filled the full `cap` bytes or hit EOF.
        // This is what eliminated the residual "2–3 microstutter at start
        // of every track" — the first prefill call asked for 32 KB but
        // got back stb_vorbis's first ~2 KB packet; the other 30 KB of
        // ring stayed at zero/stale and AICA played that as silence/click.
        int totalShorts = 0;
        int wantShorts  = cap / 2;       // 16-bit
        int eofThisCall = 0;
        while (totalShorts < wantShorts) {
            short *out = (short *)c->cb_buf + totalShorts;
            int remShorts = wantShorts - totalShorts;
            // stb_vorbis wants total-shorts (interleaved samples × channels).
            int got = stb_vorbis_get_samples_short_interleaved(
                c->vorbis, c->channels, out, remShorts);
            if (got <= 0) {
                if (c->loop) {
                    stb_vorbis_seek_start(c->vorbis);
                    continue; // try again from the top
                }
                eofThisCall = 1;
                break;
            }
            totalShorts += got * c->channels;
        }
        produced = totalShorts * 2;
        if (produced < cap) {
            // Tail-padding only happens if we hit non-loop EOF mid-chunk.
            memset(c->cb_buf + produced, 0, cap - produced);
            produced = cap;
        }
        if (eofThisCall) {
            // Non-looping music ran out: flag EOF so IsMusicOggPlaying
            // returns false and the engine's NextMusic chain (jingles
            // queued via cube transitions) starts the next track.
            c->pcm_eof = 1;
            c->pcm_drain_bytes += (size_t)cap;
        }
    } else if (c->push_ring) {
        size_t avail = c->push_w - c->push_r;
        size_t take  = (avail < (size_t)cap) ? avail : (size_t)cap;
        if (take > 0) {
            size_t r = c->push_r & VIDEO_RING_MASK;
            if (r + take <= VIDEO_RING_BYTES) {
                memcpy(c->cb_buf, c->push_ring + r, take);
            } else {
                size_t first = VIDEO_RING_BYTES - r;
                memcpy(c->cb_buf, c->push_ring + r, first);
                memcpy(c->cb_buf + first, c->push_ring, take - first);
            }
            c->push_r += take;
        }
        if (take < (size_t)cap) {
            // Underrun: pad with silence appropriate for the format.
            memset(c->cb_buf + take, (c->bits == 8) ? 0 : 0, cap - take);
        }
        produced = cap;
    } else if (c->pcm_base) {
        size_t remaining = (c->pcm_cursor < c->pcm_size)
                               ? c->pcm_size - c->pcm_cursor : 0;
        size_t take = (remaining < (size_t)cap) ? remaining : (size_t)cap;
        if (take > 0) {
            memcpy(c->cb_buf, c->pcm_base + c->pcm_cursor, take);
            if (c->pcm_unsigned8) {
                for (size_t i = 0; i < take; i++) c->cb_buf[i] ^= 0x80;
            }
            c->pcm_cursor += take;
        }
        if (take < (size_t)cap) {
            memset(c->cb_buf + take, 0, cap - take);
            if (c->loop) {
                c->pcm_cursor = 0;
            } else {
                c->pcm_cursor = c->pcm_size;
                c->pcm_eof = 1;
            }
        }
        if (c->pcm_eof) {
            // Account every byte that lands in the AICA ring after EOF —
            // pump uses this (not call count) to schedule teardown only
            // once the ring has fully drained out the last real audio.
            c->pcm_drain_bytes += (size_t)cap;
        }
        produced = cap;
    } else {
        memset(c->cb_buf, 0, cap);
        produced = cap;
    }

    *smp_recv = produced;
    return c->cb_buf;
}

// ─────────────────────────────────────────────────────────────────────────
// Lifecycle

static int EnsureInit(void) {
    if (stream_initialized) return 1;
    if (snd_stream_init() < 0) {
        LogPuts("KOS-AICA: snd_stream_init() failed; streaming disabled");
        return 0;
    }
    memset(chans, 0, sizeof(chans));
    for (int i = 0; i < CH_COUNT; i++) chans[i].hnd = SND_STREAM_INVALID;
    stream_initialized = 1;
    return 1;
}

static void StopChannel(StreamChannel *c) {
    // Only touch snd_stream for handles we actually allocated. C++ zero-init
    // leaves hnd == 0 (which is a valid stream slot index, NOT the invalid
    // sentinel), so use `active` as the source-of-truth flag.
    if (c->active) {
        snd_stream_stop(c->hnd);
        snd_stream_destroy(c->hnd);
    }
    c->hnd = SND_STREAM_INVALID;
    if (c->vorbis) {
        stb_vorbis_close(c->vorbis);
        c->vorbis = NULL;
    }
    if (c->vorbis_file_buf) {
        free(c->vorbis_file_buf);
        c->vorbis_file_buf = NULL;
        c->vorbis_file_size = 0;
    }
    if (c->push_ring) {
        free(c->push_ring);
        c->push_ring = NULL;
    }
    if (c->pcm_owned) {
        free(c->pcm_owned);
        c->pcm_owned = NULL;
    }
    c->push_w = c->push_r = 0;
    c->pcm_base = NULL;
    c->pcm_size = 0;
    c->pcm_cursor = 0;
    c->active = 0;
    c->paused = 0;
}

// ─────────────────────────────────────────────────────────────────────────
// MUSIC — looping OGG file

extern "C" int StartMusicOgg(const char *path, int loop, int volume_0_255) {
    if (!EnsureInit()) return 0;
    StreamChannel *c = &chans[CH_MUSIC];
    StopChannel(c);

    // Slurp the entire OGG into main RAM up front. .ogg files for LBA2 are
    // 70 KB – 3 MB; even the largest ambient track fits comfortably in DC's
    // 16 MB. Decoding from memory eliminates GD-ROM seek/read latency
    // during the snd_stream callback — without this stb_vorbis hits the
    // disc on every page-fault inside the decoder, AICA underruns waiting,
    // and the user hears the residual "music starts with stutter" we had
    // after fixing the callback chunk size.
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LogPrintf("KOS-AICA: fopen(%s) failed\n", path);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0 || file_size > 8 * 1024 * 1024) {
        LogPrintf("KOS-AICA: unreasonable .ogg size %ld at %s\n", file_size, path);
        fclose(fp);
        return 0;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)file_size);
    if (!buf) {
        LogPrintf("KOS-AICA: malloc(%ld) for OGG failed\n", file_size);
        fclose(fp);
        return 0;
    }
    if (fread(buf, 1, (size_t)file_size, fp) != (size_t)file_size) {
        LogPrintf("KOS-AICA: short read on %s\n", path);
        free(buf);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(buf, (int)file_size, &err, NULL);
    if (!v) {
        LogPrintf("KOS-AICA: stb_vorbis_open_memory(%s) err=%d\n", path, err);
        free(buf);
        return 0;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate < 8000) {
        stb_vorbis_close(v);
        free(buf);
        return 0;
    }

    c->hnd = snd_stream_alloc(StreamCallback, STREAM_RING_BYTES);
    if (c->hnd == SND_STREAM_INVALID) {
        stb_vorbis_close(v);
        free(buf);
        LogPuts("KOS-AICA: snd_stream_alloc failed (music)");
        return 0;
    }
    c->vorbis           = v;
    c->vorbis_file_buf  = buf;
    c->vorbis_file_size = (int)file_size;
    c->pcm_eof          = 0;
    c->pcm_drain_bytes  = 0;
#ifdef LBA2_DEBUG_PERF
    LogPrintf("KOS-AICA: StartMusicOgg %s rate=%d ch=%d loop=%d vol=%d\n",
              path, (int)info.sample_rate, (int)info.channels, loop, volume_0_255);
#endif
    c->freq     = (int)info.sample_rate;
    c->channels = (int)info.channels;
    c->bits     = 16;
    c->loop     = loop;
    c->volume   = volume_0_255;
    c->active   = 1;
    c->paused   = 0;

    snd_stream_volume(c->hnd, volume_0_255);
    snd_stream_start(c->hnd, c->freq, c->channels >= 2 ? 1 : 0);
    return 1;
}

extern "C" void StopMusicOgg(void)        { StopChannel(&chans[CH_MUSIC]); }
extern "C" int  IsMusicOggPlaying(void) {
    StreamChannel *c = &chans[CH_MUSIC];
    // Return false on EOF (non-looping jingle ran out) so the engine's
    // GetMusic() reads 0, NextMusic queue triggers, jingle chain advances.
    return c->active && !c->pcm_eof && !c->paused;
}
// "Done" = the channel is fully finished (torn down or EOF) — distinct
// from IsMusicOggPlaying which also returns false on PAUSED. Used by the
// SOUND_KOS_BACKEND StreamName() / IsCDPlaying() lazy-clear path to know
// when it's safe to forget the previously-playing track. Pause must NOT
// trigger that clear or ResumeMusic loses track of what to resume.
extern "C" int  IsMusicOggDone(void) {
    StreamChannel *c = &chans[CH_MUSIC];
    return !c->active || c->pcm_eof;
}

extern "C" void PauseMusicOgg(void) {
    StreamChannel *c = &chans[CH_MUSIC];
    if (c->active && !c->paused) {
        snd_stream_stop(c->hnd);
        c->paused = 1;
    }
}
extern "C" void ResumeMusicOgg(void) {
    StreamChannel *c = &chans[CH_MUSIC];
    if (c->active && c->paused) {
        snd_stream_start(c->hnd, c->freq, c->channels >= 2 ? 1 : 0);
        snd_stream_volume(c->hnd, c->volume);
            c->paused = 0;
    }
}
// Volume setter must NOT call snd_stream_volume on a never-allocated channel
// — `chans[].hnd` is C++ zero-initialized (i.e. 0, which is the FIRST valid
// snd_stream handle on KOS), so testing `hnd != SND_STREAM_INVALID` is not
// enough. Gate strictly on `c->active` which only becomes 1 after a
// successful snd_stream_alloc in StartMusicOgg.
extern "C" void SetMusicOggVolume(int vol_0_255) {
    StreamChannel *c = &chans[CH_MUSIC];
    c->volume = vol_0_255;
    if (c->active) snd_stream_volume(c->hnd, vol_0_255);
}

// ─────────────────────────────────────────────────────────────────────────
// VOICE — raw PCM, one-shot. Caller owns pcm_data for the lifetime.

extern "C" int PlayVoicePcm(const void *pcm_data, int pcm_size,
                            int rate, int bits, int channels,
                            int volume_0_255, int unsigned8,
                            int take_ownership) {
    if (!EnsureInit()) {
        if (take_ownership) free((void *)pcm_data);
        return 0;
    }
    if (!pcm_data || pcm_size <= 0 || rate < 1000 ||
        channels < 1 || channels > 2 || (bits != 8 && bits != 16)) {
        if (take_ownership) free((void *)pcm_data);
        return 0;
    }

    StreamChannel *c = &chans[CH_VOICE];
    StopChannel(c);

    c->hnd = snd_stream_alloc(StreamCallback, STREAM_RING_BYTES);
    if (c->hnd == SND_STREAM_INVALID) {
        LogPuts("KOS-AICA: snd_stream_alloc failed (voice)");
        if (take_ownership) free((void *)pcm_data);
        return 0;
    }
    c->pcm_base       = (const uint8_t *)pcm_data;
    c->pcm_owned      = take_ownership ? (void *)pcm_data : NULL;
    c->pcm_size       = (size_t)pcm_size;
    c->pcm_cursor     = 0;
    c->pcm_unsigned8  = (bits == 8) ? (unsigned8 ? 1 : 0) : 0;
    c->pcm_eof        = 0;
    c->pcm_drain_bytes = 0;
#ifdef LBA2_DEBUG_PERF
    LogPrintf("KOS-AICA: voice rate=%d bits=%d ch=%d size=%d unsigned8=%d owned=%d\n",
              rate, bits, channels, pcm_size, unsigned8, take_ownership);
#endif
    c->freq           = rate;
    c->channels       = channels;
    c->bits           = bits;
    c->loop           = 0;
    c->volume         = volume_0_255;
    c->active         = 1;
    c->paused         = 0;

    snd_stream_volume(c->hnd, volume_0_255);
    snd_stream_start(c->hnd, rate, channels >= 2 ? 1 : 0);
    return 1;
}

extern "C" void StopVoicePcm(void)        { StopChannel(&chans[CH_VOICE]); }
extern "C" int  IsVoicePcmPlaying(void) {
    // Report false as soon as the cursor reaches EOF. The actual AICA ring
    // takes another ~370ms to drain to zeroes (see pump teardown logic),
    // but the engine's IsSamplePlaying contract is "is there more PCM to
    // dispatch?" which is what the cursor tracks. Lying-true here would
    // hang dialog UI for an extra ring-drain cycle.
    StreamChannel *c = &chans[CH_VOICE];
    return c->active && !c->pcm_eof && !c->paused;
}

// ─────────────────────────────────────────────────────────────────────────
// VIDEO — push-mode PCM ring fed by libsmacker per-frame.

extern "C" int StartVideoAudioStream(int rate, int channels, int bits) {
    if (!EnsureInit()) return 0;
    StreamChannel *c = &chans[CH_VIDEO];
    StopChannel(c);

    c->push_ring = (uint8_t *)aligned_alloc(32, VIDEO_RING_BYTES);
    if (!c->push_ring) return 0;
    memset(c->push_ring, (bits == 8) ? 0x80 : 0, VIDEO_RING_BYTES);
    c->push_w = c->push_r = 0;

    c->hnd = snd_stream_alloc(StreamCallback, STREAM_RING_BYTES);
    if (c->hnd == SND_STREAM_INVALID) {
        free(c->push_ring); c->push_ring = NULL; return 0;
    }
    c->freq     = rate;
    c->channels = channels;
    c->bits     = bits;
    c->volume   = 240;
    c->loop     = 0;
    c->active   = 1;
    // Start paused — PLAYACF prefills frames then calls ResumeVideoAudio.
    c->paused   = 1;
    return 1;
}

extern "C" void PushVideoAudioStream(const void *data, unsigned size) {
    StreamChannel *c = &chans[CH_VIDEO];
    if (!c->push_ring || !data || !size) return;

    // Drop silently if the producer outruns the consumer (FMV continues
    // — better than a stall, and only happens if pause sequence misfires).
    size_t avail_space = VIDEO_RING_BYTES - (c->push_w - c->push_r);
    if (size > avail_space) size = (unsigned)avail_space;
    if (size == 0) return;

    size_t w = c->push_w & VIDEO_RING_MASK;
    if (w + size <= VIDEO_RING_BYTES) {
        memcpy(c->push_ring + w, data, size);
    } else {
        size_t first = VIDEO_RING_BYTES - w;
        memcpy(c->push_ring + w, data, first);
        memcpy(c->push_ring, (const uint8_t *)data + first, size - first);
    }
    c->push_w += size;
}

extern "C" void ResumeVideoAudioStream(void) {
    StreamChannel *c = &chans[CH_VIDEO];
    if (c->active && c->paused) {
        snd_stream_volume(c->hnd, c->volume);
        snd_stream_start(c->hnd, c->freq, c->channels >= 2 ? 1 : 0);
            c->paused = 0;
    }
}
extern "C" void PauseVideoAudioStream(void) {
    StreamChannel *c = &chans[CH_VIDEO];
    if (c->active && !c->paused) {
        snd_stream_stop(c->hnd);
        c->paused = 1;
    }
}
extern "C" void StopVideoAudioStream(void) {
    StopChannel(&chans[CH_VIDEO]);
}

// ─────────────────────────────────────────────────────────────────────────
// Pump — call once per frame from ManageTime.

extern "C" void AudioStreamPump(void) {
    if (!stream_initialized) return;
    for (int i = 0; i < CH_COUNT; i++) {
        StreamChannel *c = &chans[i];
        if (!c->active || c->paused) continue;

        // Voice/music EOF teardown. After a non-looping stream's source
        // runs out, the callback keeps feeding silence so AICA doesn't
        // loop the last buffered chunk. Once the total silence pushed
        // exceeds the AICA ring size we know all actual audio has rolled
        // out of the ring and we can free the channel. (Counting
        // AICA-callback bytes, not pump-call cycles — the latter races
        // with how often AICA actually drains the ring.) Applied to
        // CH_VOICE (always one-shot) AND CH_MUSIC when loop=0 (jingles).
        if ((i == CH_VOICE || i == CH_MUSIC) && c->pcm_eof &&
            c->pcm_drain_bytes >= STREAM_RING_BYTES + CB_CHUNK_BYTES) {
            StopChannel(c);
            continue;
        }

        snd_stream_poll(c->hnd);
    }
}
