// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// LIB386/AIL/KOS/SOUND_KOS_BACKEND.CPP
// KallistiOS-native audio backend for Dreamcast.
//
// V2.1: snd_sfx-based SFX (≤ 65534 samples; longer ones used to drop silently).
// V2.3: cdrom_cdda_play music — works for menu/logo, truncates at 1s in-game
//       under concurrent HQR I/O. Now retained only as a fallback path; the
//       primary music path is V2.5 streamed OGG (no GD-ROM head contention).
// V2.4: long PlaySample → snd_stream voice channel via STREAM_KOS::PlayVoicePcm.
//       Catches dialog voices from MESSAGE.CPP::PlaySpeakVoc and any other
//       sample > 65534 samples. The engine-side buffer (BufSpeak) remains
//       valid for the lifetime of playback so we hand a pointer, no copy.
// V2.5: PlayCD/PlayStream → STREAM_KOS::StartMusicOgg. Engine-side track number
//       resolves to /cd/Music/<name>.ogg; OGG file is read once and decoded
//       from main RAM, eliminating the GD-ROM single-head conflict that
//       killed in-game CDDA in V2.3.
// V2.6: VIDEO_AUDIO contract handled by VIDEO_AUDIO_KOS.CPP (separate file).
//
// Per-voice runtime mutation (ChangeVolumePan, ChangePitchbend) on snd_sfx
// channels remains a no-op — sfxmgr exposes no live control and direct AICA
// register pokes are out of scope. Live volume on the streaming channels
// works via snd_stream_volume.

#include "AIL.H"

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/cdrom.h>
#include <dc/spu.h>
#include <dc/syscalls.h>

#include <SYSTEM/LIMITS.H>
#include <SYSTEM/LOGPRINT.H>

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_VOICES 16
#define DEFAULT_RATE 44100
#define PITCHBEND_NORMAL 4096
#define MAX_SFX_SAMPLES 65534

#ifdef __cplusplus
extern "C" {
#endif

// STREAM_KOS exports — see LIB386/AIL/KOS/STREAM_KOS.CPP
int  StartMusicOgg(const char *path, int loop, int volume_0_255);
void StopMusicOgg(void);
int  IsMusicOggPlaying(void);
void PauseMusicOgg(void);
void ResumeMusicOgg(void);
void SetMusicOggVolume(int vol_0_255);

int  IsMusicOggDone(void);

int  PlayVoicePcm(const void *pcm_data, int pcm_size,
                  int rate, int bits, int channels,
                  int volume_0_255, int unsigned8, int take_ownership);
void StopVoicePcm(void);
int  IsVoicePcmPlaying(void);

extern volatile U32 TimerSystemHR;

struct KosVoice {
    sfxhnd_t sfxhnd;
    int chn;
    U32 enginehandle;
    U32 startTimer;
    U8 volume;
    U8 pan;
    U8 active;
    U8 looping;
};

static KosVoice voices[MAX_VOICES];
static U32 nextHandle = 0x10000;
static int sampleVolume = 127;
static int samplesPaused = 0;
static int sfxLogEnabled = 0;
static int inverseStereo = 0;

// Voice-stream tracking (V2.4). Long PlaySample(> 65534 samples) is routed
// to STREAM_KOS::PlayVoicePcm on the dedicated CH_VOICE channel. We track
// the engine-side handle here so IsSamplePlaying / StopOneSample can find
// it. SPEAK_SAMPLE is the only stable engine ID for dialog voice but the
// engine also calls IsSamplePlaying with whatever handle PlaySample
// returned, so we honor both.
static U32 voiceStreamEngineHandle = 0;
static U32 voiceStreamUserKey = 0;

S32 Sample_Driver_Enabled = FALSE;
S32 UseWaveMixer = FALSE;

// ──────────────────────────────────────────────────────────────────────────────
// Helpers

// Parse RIFF/WAVE header, locate fmt + data chunks. Returns 1 on success.
// On success populates format fields AND outDataOffset/outDataSize (byte range
// of the PCM data section inside `buf`). Caller still owns `buf`.
//
// IMPORTANT: LBA2 voice files (.VOX records) have a non-standard first byte.
// MESSAGE.CPP::PlaySpeakVoc reads the original FlagNextVoc byte at [0] then
// overwrites it with 'W' before passing the buffer to PlaySample. The result
// is "?IFF<size>WAVE..." with the leading 'R' replaced — the rest of the
// header and data are perfectly normal PCM WAV. Accept any first byte as
// long as bytes [1..3] = "IFF" and [8..11] = "WAVE", matching the SDL
// backend's SkipWavHeaderIfPresent permissiveness. Also clamp bits/rate/ch
// and derive bits from blockAlign for fmtTag=1 (handles WAVs with a 0 or
// otherwise-bogus bits-per-sample field).
static int ParseWav(const U8 *buf, S32 len, int *rate, int *bits, int *ch,
                    int *fmtTag, int *outBlkAlign,
                    S32 *outDataOffset, S32 *outDataSize) {
    if (len < 44) return 0;
    bool riffOk =
        (memcmp(buf, "RIFF", 4) == 0) ||
        (buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F'); // VOX [0]='W'
    if (!riffOk || memcmp(buf + 8, "WAVE", 4) != 0) return 0;
    int gotFmt = 0;
    int blkAlign = 0;
    S32 pos = 12;
    while (pos + 8 <= len) {
        S32 cs = (S32)(buf[pos + 4] | (buf[pos + 5] << 8) |
                       (buf[pos + 6] << 16) | (buf[pos + 7] << 24));
        if (memcmp(buf + pos, "fmt ", 4) == 0 && cs >= 16) {
            const U8 *f = buf + pos + 8;
            *fmtTag = f[0] | (f[1] << 8);
            *ch     = f[2] | (f[3] << 8);
            *rate   = f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24);
            blkAlign = f[12] | (f[13] << 8);
            *bits   = f[14] | (f[15] << 8);
            // Clamp/repair bogus values the way SDL backend does.
            if (*rate <= 0 || *rate > 192000) *rate = DEFAULT_RATE;
            if (*ch <= 0 || *ch > 2)          *ch = 1;
            if (*fmtTag == 1) {
                if (blkAlign > 0 && *ch > 0)
                    *bits = (blkAlign * 8) / *ch;
                if (*bits != 8 && *bits != 16) *bits = 16;
            }
            gotFmt = 1;
        } else if (memcmp(buf + pos, "data", 4) == 0 && pos + 8 + cs <= len) {
            *outDataOffset = pos + 8;
            *outDataSize = cs;
            *outBlkAlign = blkAlign;
            return gotFmt;
        }
        pos += 8 + ((cs + 1) & ~1);
    }
    return 0;
}

// IMA ADPCM decoder. Ported verbatim from the SDL backend's DecodeImaAdpcm
// in LIB386/AIL/SDL/SAMPLE.CPP — same algorithm, same step/index tables.
// Returns a malloc()'d S16 PCM buffer (caller must free) and writes the
// number of decoded samples into *outSamples; NULL on failure.
//
// LBA2 .VOX dialog data is shipped as IMA ADPCM (fmtTag=17) at 22050 Hz
// mono with blockAlign typically 256 bytes (505 samples per block, 1
// header byte + many nibble pairs). Pure PCM voice files don't exist in
// the original assets — every dialog goes through this codec.
static const S16 kImaStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767};
static const int kImaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static S16 *DecodeImaAdpcm(const U8 *adpcmData, S32 adpcmSize,
                           S32 blockAlign, S32 *outSamples) {
    if (blockAlign < 8 || adpcmSize < blockAlign) {
        *outSamples = 0;
        return NULL;
    }
    S32 samplesPerBlock = (blockAlign - 4) * 2 + 1;
    S32 numBlocks = adpcmSize / blockAlign;
    S32 remainder = adpcmSize % blockAlign;
    S32 totalSamples = numBlocks * samplesPerBlock;
    if (remainder >= 8)
        totalSamples += (remainder - 4) * 2 + 1;
    S16 *pcm = (S16 *)malloc((size_t)totalSamples * sizeof(S16));
    if (!pcm) {
        *outSamples = 0;
        return NULL;
    }
    S32 outIdx = 0;
    const U8 *src = adpcmData;
    S32 bytesLeft = adpcmSize;
    while (bytesLeft >= 8) {
        S32 blockSize = (bytesLeft >= blockAlign) ? blockAlign : bytesLeft;
        S16 predictor = (S16)(src[0] | (src[1] << 8));
        int stepIndex = src[2];
        if (stepIndex < 0)  stepIndex = 0;
        if (stepIndex > 88) stepIndex = 88;
        pcm[outIdx++] = predictor;
        for (S32 i = 4; i < blockSize; i++) {
            for (int nibIdx = 0; nibIdx < 2 && outIdx < totalSamples; nibIdx++) {
                int nibble = (nibIdx == 0) ? (src[i] & 0x0F) : ((src[i] >> 4) & 0x0F);
                int step = kImaStepTable[stepIndex];
                int diff = step >> 3;
                if (nibble & 1) diff += step >> 2;
                if (nibble & 2) diff += step >> 1;
                if (nibble & 4) diff += step;
                if (nibble & 8) diff = -diff;
                int sample = (int)predictor + diff;
                if (sample > 32767)  sample = 32767;
                if (sample < -32768) sample = -32768;
                predictor = (S16)sample;
                pcm[outIdx++] = predictor;
                stepIndex += kImaIndexTable[nibble];
                if (stepIndex < 0)  stepIndex = 0;
                if (stepIndex > 88) stepIndex = 88;
            }
        }
        src += blockSize;
        bytesLeft -= blockSize;
    }
    *outSamples = outIdx;
    return pcm;
}

// Convert raw PCM data into the AICA signed-sample convention and hand it to
// snd_sfx_load_raw_buf, which copies into AICA RAM and frees its own DMA
// staging. Returns SFXHND_INVALID on any failure.
//
// Why this exists: KOS sfxmgr does NOT bias-flip 8-bit unsigned WAV data when
// loading. AICA hardware reads 8-bit samples as signed (-128..127); WAV PCM
// 8-bit is unsigned (0..255 with 128=center). Loading the raw bytes makes
// silence read as -128 and the entire waveform appears clipped/inverted —
// every SFX comes out as "raspy AM-radio". Fix: XOR 0x80 over the data into
// a temporary aligned buffer, then load.
//
// 16-bit PCM WAV is already signed and matches AICA — pass through.
static sfxhnd_t LoadPcmAica(const U8 *data, S32 dataSize, int rate, int bits, int channels) {
    if (dataSize <= 0 || rate < 1000 || (bits != 8 && bits != 16) ||
        channels < 1 || channels > 2) {
        return SFXHND_INVALID;
    }
    // Pad to 32-byte multiple per channel (AICA DMA alignment requirement).
    S32 padded = (dataSize + 31) & ~31;
    void *tmp = aligned_alloc(32, padded);
    if (!tmp) return SFXHND_INVALID;
    memcpy(tmp, data, dataSize);
    // Pad with silence: 0x80 in unsigned8 = 0 signed; 0 in 16-bit = signed 0.
    if (padded > dataSize) {
        memset((U8 *)tmp + dataSize, (bits == 8) ? 0x80 : 0, padded - dataSize);
    }
    if (bits == 8) {
        U8 *p = (U8 *)tmp;
        for (S32 i = 0; i < padded; i++) p[i] ^= 0x80;
    }
    sfxhnd_t hnd = snd_sfx_load_raw_buf((char *)tmp, padded, rate, bits, channels);
    free(tmp);
    return hnd;
}

static int FindSlotFromHandle(U32 sample) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active && voices[i].enginehandle == sample) return i;
    }
    return -1;
}

// Reap any one-shot voices that AICA reports as no longer playing. Called
// before allocating a new slot in PlaySample so we don't run out of voices.
static void ReapFinishedVoices(void) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) continue;
        if (voices[i].looping) continue;
        if (!snd_is_playing(voices[i].chn)) {
            snd_sfx_unload(voices[i].sfxhnd);
            voices[i].sfxhnd = SFXHND_INVALID;
            voices[i].active = 0;
        }
    }
}

static int FreeSlot(void) {
    for (int i = 0; i < MAX_VOICES; i++) if (!voices[i].active) return i;
    return -1;
}

// ──────────────────────────────────────────────────────────────────────────────
// Common

void InitAIL() {
    // snd_init lives in InitSampleDriver — InitAIL is the global hook but the
    // real driver bringup happens once the engine asks for samples.
}

// ──────────────────────────────────────────────────────────────────────────────
// MIDI — engine only ever calls InitMidiDriver; nothing else hits MIDI APIs.

S32 InitMidiDriver(char *driver_name) {
    (void)driver_name;
    return TRUE;
}

// ──────────────────────────────────────────────────────────────────────────────
// Sample API

S32 InitSampleDriver(char *driver_name) {
    (void)driver_name;
    if (Sample_Driver_Enabled) return TRUE;

    if (snd_init() < 0) {
        LogPuts("KOS-AICA: snd_init() failed; SFX disabled");
        return FALSE;
    }

    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < MAX_VOICES; i++) voices[i].sfxhnd = SFXHND_INVALID;
    Sample_Driver_Enabled = TRUE;
    LogPrintf("KOS-AICA: snd_init OK, %lu KB AICA sample RAM available\n",
              (unsigned long)(snd_mem_available() / 1024));
    return TRUE;
}

void SetMasterVolumeSample(S32 volume) { sampleVolume = volume; }
S32 FadeOutSamples(S32 delay)          { (void)delay; return 0; }
S32 FadeInSamples(S32 delay)           { return delay; }
void InverseStereoSample(S32 inverse)  { inverseStereo = inverse ? 1 : 0; }
void SfxLogEnable(int enable)          { sfxLogEnabled = enable; }
int SfxLogIsEnabled(void)              { return sfxLogEnabled; }
S32 GetSamplesPausedCount(void)        { return samplesPaused; }

U32 PlaySample(void *buffer, S32 sizeBytes, U32 userhandle, S32 pitchbend,
               S32 repeat, S32 volume, S32 pan) {
    if (!Sample_Driver_Enabled || !buffer || sizeBytes <= 0) return 0xFFFFFFFF;

    int srcRate = DEFAULT_RATE;
    int srcBits = 8;
    int srcCh = 1;
    int fmtTag = 1;
    int blkAlign = 0;
    S32 dataOff = 0, dataSize = 0;
    int isWav = ParseWav((const U8 *)buffer, sizeBytes, &srcRate, &srcBits,
                         &srcCh, &fmtTag, &blkAlign, &dataOff, &dataSize);

    // Trace every PlaySample on DC so we can correlate symptom → path taken.
    // Includes pitchbend (engine "frequence" arg, 4096 = neutral) — when
    // engine LM_NEW_SAMPLE / LM_PARM_SAMPLE script ops set
    // ParmSampleFrequence != 0, HQ_MixSample overrides the call's frequence
    // and we end up playing the sample at non-neutral rate (e.g. 2048 =
    // half-rate = octave down = "ovattato"). That's by-design 1997 engine
    // behavior, not a backend bug — the SDL backend produces the same
    // pitch shift via its software step-resampling.
    {
        S32 effFreq = (pitchbend > 0) ? pitchbend : PITCHBEND_NORMAL;
        S32 finalHz = (S32)(((S64)srcRate * effFreq) / PITCHBEND_NORMAL);
        if (sfxLogEnabled)
            LogPrintf("KOS-AICA: PlaySample uh=0x%08X size=%d isWav=%d rate=%d bits=%d ch=%d fmt=%d blk=%d pb=%d->%dHz\n",
                      (unsigned)userhandle, (int)sizeBytes, isWav,
                      srcRate, srcBits, srcCh, fmtTag, blkAlign,
                      (int)pitchbend, (int)finalHz);
    }

    // ── IMA ADPCM (fmtTag=17): LBA2 dialog voice files. Decode to 16-bit
    // signed PCM and route to the streaming voice channel. snd_sfx 65534
    // sample cap gets exceeded by anything longer than ~3 s anyway.
    if (isWav && fmtTag == 17 && blkAlign >= 8) {
        S32 decodedSamples = 0;
        S16 *decoded = DecodeImaAdpcm((const U8 *)buffer + dataOff,
                                      dataSize, blkAlign, &decodedSamples);
        if (!decoded || decodedSamples <= 0) {
            free(decoded);
            LogPrintf("KOS-AICA: IMA decode failed (size=%d blk=%d)\n",
                      (int)dataSize, blkAlign);
            return 0xFFFFFFFF;
        }
        int kVol = (volume * sampleVolume * 255) / (127 * 127);
        if (kVol < 0) kVol = 0; if (kVol > 255) kVol = 255;

        // Always stream voices through CH_VOICE — even short ones. snd_sfx
        // could handle <65534-sample decodes but using one stable code path
        // keeps lifetime / format handling simpler and matches the user's
        // expectation that "dialog uses the voice channel".
        if (PlayVoicePcm(decoded, decodedSamples * 2,
                         srcRate, /*bits=*/16, srcCh,
                         kVol, /*unsigned8=*/0, /*take_ownership=*/1)) {
            voiceStreamEngineHandle = ((U32)0xFE << 24) | (nextHandle++ & 0x00FFFFFF);
            voiceStreamUserKey      = userhandle;
            return voiceStreamEngineHandle;
        }
        // PlayVoicePcm took ownership and freed `decoded` on failure.
        return 0xFFFFFFFF;
    }

    // ── V2.4: route long samples to the streaming voice channel ────────────
    // Engine-side mixer handles all dialog voices through PlaySample.
    // snd_sfx caps at 65534 samples (~1.5 s at 44.1 kHz mono, ~3 s at 22 kHz)
    // — way too short for dialog. Route any long PCM to PlayVoicePcm which
    // streams it via snd_stream from the caller's buffer. The buffer
    // (BufSpeak in MESSAGE.CPP) is engine-stable for the playback lifetime.
    {
        S32 sampleCount;
        const U8 *pcmBase;
        S32 pcmSize;
        int useRate, useBits, useCh, isUnsigned8;
        if (isWav && fmtTag == 1) {
            sampleCount = dataSize / (srcCh * (srcBits / 8));
            pcmBase     = (const U8 *)buffer + dataOff;
            pcmSize     = dataSize;
            useRate     = srcRate;
            useBits     = srcBits;
            useCh       = srcCh;
            isUnsigned8 = (srcBits == 8) ? 1 : 0;
        } else if (!isWav) {
            // Raw 8-bit unsigned PCM at default rate, single mono channel.
            sampleCount = sizeBytes;
            pcmBase     = (const U8 *)buffer;
            pcmSize     = sizeBytes;
            useRate     = DEFAULT_RATE;
            useBits     = 8;
            useCh       = 1;
            isUnsigned8 = 1;
        } else {
            sampleCount = -1; // non-PCM WAV, will fail below
            pcmBase = NULL; pcmSize = 0;
            useRate = useBits = useCh = isUnsigned8 = 0;
        }

        if (pcmBase && sampleCount > MAX_SFX_SAMPLES) {
            int kVol = (volume * sampleVolume * 255) / (127 * 127);
            if (kVol < 0) kVol = 0; if (kVol > 255) kVol = 255;
            // PCM voice — caller (engine BufSpeak) retains lifetime,
            // we don't take ownership.
            if (PlayVoicePcm(pcmBase, pcmSize, useRate, useBits, useCh,
                             kVol, isUnsigned8, /*take_ownership=*/0)) {
                voiceStreamEngineHandle = ((U32)0xFE << 24) | (nextHandle++ & 0x00FFFFFF);
                voiceStreamUserKey      = userhandle;
                if (sfxLogEnabled) {
                    LogPrintf("KOS-AICA: PlaySample stream-voice %d samples rate=%d bits=%d ch=%d\n",
                              (int)sampleCount, useRate, useBits, useCh);
                }
                return voiceStreamEngineHandle;
            }
            // PlayVoicePcm failed — fall through to snd_sfx where it'll
            // hit the size cap and fail loudly in the log.
        }
    }
    // ── End V2.4 voice routing ─────────────────────────────────────────────

    ReapFinishedVoices();
    int slot = FreeSlot();
    if (slot < 0) {
        if (sfxLogEnabled) LogPuts("KOS-AICA: PlaySample no free voice");
        return 0xFFFFFFFF;
    }

    sfxhnd_t hnd = SFXHND_INVALID;
    const U8 *buf = (const U8 *)buffer;
    if (isWav) {
        if (fmtTag != 1) {
            // ADPCM IMA, μ-law, or other non-PCM tags. snd_sfx can't take these
            // without preprocessing. Skip silently in V2.1 — we'll add an IMA
            // decoder if SAMPLES.HQR turns out to use it heavily.
            if (sfxLogEnabled)
                LogPrintf("KOS-AICA: WAV fmtTag=%d unsupported, skipping\n", fmtTag);
            return 0xFFFFFFFF;
        }
        S32 sampleCount = dataSize / (srcCh * (srcBits / 8));
        if (sampleCount > MAX_SFX_SAMPLES) {
            // Should have been caught by the streaming-voice branch above;
            // we land here only if PlayVoicePcm errored out. Log and fail.
            if (sfxLogEnabled)
                LogPrintf("KOS-AICA: WAV %d samples > 65534, voice-stream failed\n",
                          (int)sampleCount);
            return 0xFFFFFFFF;
        }
        hnd = LoadPcmAica(buf + dataOff, dataSize, srcRate, srcBits, srcCh);
    } else {
        // Raw 8-bit unsigned PCM at default rate (matches SDL backend fallback).
        if (sizeBytes > MAX_SFX_SAMPLES) {
            if (sfxLogEnabled)
                LogPrintf("KOS-AICA: raw %d bytes > 65534, voice-stream failed\n",
                          (int)sizeBytes);
            return 0xFFFFFFFF;
        }
        hnd = LoadPcmAica(buf, sizeBytes, srcRate, 8, 1);
    }

    if (hnd == SFXHND_INVALID) return 0xFFFFFFFF;

    // Engine pan: 0..127 (64 = center). KOS pan: 0..255 (128 = center).
    int kPan = pan * 2;
    if (kPan < 0) kPan = 0;
    if (kPan > 255) kPan = 255;
    if (inverseStereo) kPan = 255 - kPan;

    // Engine volume 0..127 × master 0..127 → KOS 0..255.
    int kVol = (volume * sampleVolume * 255) / (127 * 127);
    if (kVol < 0) kVol = 0;
    if (kVol > 255) kVol = 255;

    // Engine pitchbend 4096 = native rate. Target Hz = srcRate * pb / 4096.
    int pb = (pitchbend > 0) ? pitchbend : PITCHBEND_NORMAL;
    int kFreq = (int)(((int64_t)srcRate * pb) / PITCHBEND_NORMAL);
    if (kFreq < 1000) kFreq = 1000;
    if (kFreq > 96000) kFreq = 96000;

    // Engine repeat: 0 = loop forever, ≥1 = play that many times. AICA only
    // does loop-once (KOS sfxmgr `loop` flag is bool). For repeat>1 the engine
    // will retrigger; for now only honor 0=loop, anything else = play once.
    int kLoop = (repeat == 0) ? 1 : 0;

    sfx_play_data_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.chn = -1;
    pd.idx = hnd;
    pd.vol = kVol;
    pd.pan = kPan;
    pd.loop = kLoop;
    pd.freq = kFreq;
    pd.loopstart = 0;
    pd.loopend = 0;
    int chn = snd_sfx_play_ex(&pd);
    if (chn < 0) {
        snd_sfx_unload(hnd);
        if (sfxLogEnabled) LogPuts("KOS-AICA: snd_sfx_play_ex returned -1");
        return 0xFFFFFFFF;
    }

    KosVoice &V = voices[slot];
    V.sfxhnd = hnd;
    V.chn = chn;
    V.enginehandle = ((U32)(slot & 0xFF) << 24) | (nextHandle++ & 0x00FFFFFF);
    V.startTimer = TimerSystemHR ? TimerSystemHR : 1;
    V.volume = (U8)volume;
    V.pan = (U8)pan;
    V.active = 1;
    V.looping = (U8)kLoop;

    if (sfxLogEnabled) {
        LogPrintf("KOS-AICA: PlaySample slot=%d chn=%d rate=%d freq=%d sz=%d loop=%d v=%d p=%d\n",
                  slot, chn, srcRate, kFreq, (int)sizeBytes, kLoop, kVol, kPan);
    }
    return V.enginehandle;
}

void ChangePitchbendSample(U32 sample, S32 pitchbend) {
    (void)sample; (void)pitchbend;
    // sfxmgr exposes no runtime freq change; AICA reg poke deferred to V2.3.
}

void ChangeVolumePanSample(U32 sample, S32 volume, S32 pan) {
    int s = FindSlotFromHandle(sample);
    if (s < 0) return;
    voices[s].volume = (U8)volume;
    voices[s].pan = (U8)pan;
    // Same as pitch: live volume/pan deferred to V2.3.
}

// True if `sample` is the engine handle that PlaySample returned for the
// active stream-voice (or the userhandle key the engine passed in).
static int IsVoiceStreamHandle(U32 sample) {
    if (!voiceStreamEngineHandle) return 0;
    return (sample == voiceStreamEngineHandle) ||
           (voiceStreamUserKey && sample == voiceStreamUserKey);
}

void StopOneSample(U32 sample) {
    if (IsVoiceStreamHandle(sample)) {
        StopVoicePcm();
        voiceStreamEngineHandle = 0;
        voiceStreamUserKey = 0;
        return;
    }
    int s = FindSlotFromHandle(sample);
    if (s < 0) return;
    snd_sfx_stop(voices[s].chn);
    snd_sfx_unload(voices[s].sfxhnd);
    voices[s].sfxhnd = SFXHND_INVALID;
    voices[s].active = 0;
}

void StopSamples(void) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) continue;
        snd_sfx_stop(voices[i].chn);
        snd_sfx_unload(voices[i].sfxhnd);
        voices[i].sfxhnd = SFXHND_INVALID;
        voices[i].active = 0;
    }
    StopVoicePcm();
    voiceStreamEngineHandle = 0;
    voiceStreamUserKey = 0;
    samplesPaused = 0;
}

void PauseSamples(void) {
    // sfxmgr has no per-channel pause. Track only the engine's ref-count;
    // a future revision could halt+resume via direct AICA register writes.
    samplesPaused++;
}

void ResumeSamples(void) {
    if (samplesPaused > 0) samplesPaused--;
}

U32 IsSamplePlaying(U32 sample) {
    if (IsVoiceStreamHandle(sample)) {
        if (IsVoicePcmPlaying()) return voiceStreamEngineHandle;
        // Voice finished — drop tracking so the next PlaySample reuses the slot.
        voiceStreamEngineHandle = 0;
        voiceStreamUserKey = 0;
        return FALSE;
    }
    int s = FindSlotFromHandle(sample);
    if (s < 0) return FALSE;
    if (!snd_is_playing(voices[s].chn) && !voices[s].looping) {
        snd_sfx_unload(voices[s].sfxhnd);
        voices[s].sfxhnd = SFXHND_INVALID;
        voices[s].active = 0;
        return FALSE;
    }
    return voices[s].startTimer;
}

// ──────────────────────────────────────────────────────────────────────────────
// Stream + CD — V2.5: streamed OGG via STREAM_KOS, single CH_MUSIC channel.
//
// Both PlayStream (jingles, called from MUSIC.CPP::PlayJingle) and PlayCD
// (CDROM music, called from MUSIC.CPP::PlayMusic via the TrackCDUS override)
// share one stream slot — only one music piece plays at a time anyway, and
// jingles overlay/replace the long ambient music in the engine's design.
//
// PlayStream is invoked with a path like "/cd/Music/JADPCM01.WAV". The
// .WAV extension is a historical artifact; on disc we ship .OGG (encoded
// from the GOG source files we already have decoded to WAV in cdda-wav/).
// The path resolver below strips the suffix, lower-cases the basename,
// and probes /cd/Music/<name>.ogg.
//
// The V2.3 cdrom_cdda_play path remains in the file intentionally: it is
// not called from PlayCD anymore but `cdrom_init` still runs in OpenCD so
// the GD-ROM driver is in a known state for future asset reads.

static char streamName[ADELINE_MAX_PATH] = "";
static S32  streamVolume = 0;
static S32  cdVolume     = 0;
static S32  cdLastTrack  = 0;

// Engine-track → OGG basename map. Mirrors the Common/Music/ files we already
// have on the host: track1..track6, jadpcm01..jadpcm18, logadpcm. Engine
// values 2..26 follow the original-1997-US disc layout the engine assumes.
static const char *kEngineToOggName(S32 track) {
    switch (track) {
        case 2:  return "track1";
        case 3:  return "track2";
        case 4:  return "track3";
        case 5:  return "track4";
        case 6:  return "track5";
        case 7:  return "jadpcm01";
        case 8:  return "track6";
        case 9:  return "jadpcm02"; case 10: return "jadpcm03";
        case 11: return "jadpcm04"; case 12: return "jadpcm05";
        case 13: return "jadpcm06"; case 14: return "jadpcm07";
        case 15: return "jadpcm08"; case 16: return "jadpcm09";
        case 17: return "jadpcm10"; case 18: return "jadpcm11";
        case 19: return "jadpcm12"; case 20: return "jadpcm13";
        case 21: return "jadpcm14"; case 22: return "jadpcm15";
        case 23: return "jadpcm16"; case 24: return "jadpcm17";
        case 25: return "jadpcm18";
        case 26: return "logadpcm";
        default: return NULL;
    }
}

// Volume mapping helper. Engine uses 0..127 for both CD and stream; AICA
// (snd_stream_volume) wants 0..255.
static int Vol127To255(int v) {
    if (v <= 0) return 0;
    int k = v * 255 / 127;
    return (k > 255) ? 255 : k;
}

// Probe for /cd/Music/<basename>.ogg. Returns 1 and writes resolved path on
// hit, 0 on miss. Tries both lowercase and original-case basenames so a
// case-sensitive ISO9660 mount doesn't lose us.
static int ResolveOggOnDisc(const char *basename, char *out, int outSize) {
    static const char *prefixes[] = { "/cd/Music/", "/cd/music/", "/cd/MUSIC/" };
    char lc[64];
    int  n = 0;
    while (basename[n] && n < (int)sizeof(lc) - 1) {
        char c = basename[n];
        lc[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        n++;
    }
    lc[n] = 0;

    for (size_t i = 0; i < sizeof(prefixes)/sizeof(prefixes[0]); i++) {
        snprintf(out, outSize, "%s%s.ogg", prefixes[i], lc);
        FILE *f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
        snprintf(out, outSize, "%s%s.OGG", prefixes[i], lc);
        f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
        snprintf(out, outSize, "%s%s.ogg", prefixes[i], basename);
        f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
    }
    return 0;
}

// Take a path the engine handed us (e.g. ".../music/JADPCM01.WAV") and
// extract just the basename without extension. Used by PlayStream.
static void ExtractBasenameNoExt(const char *path, char *out, int outSize) {
    const char *sep = strrchr(path, '/');
    const char *bsep = strrchr(path, '\\');
    if (bsep && (!sep || bsep > sep)) sep = bsep;
    const char *base = sep ? sep + 1 : path;
    int n = 0;
    while (base[n] && base[n] != '.' && n < outSize - 1) { out[n] = base[n]; n++; }
    out[n] = 0;
}

void OpenStream() { /* snd_stream_init lives in STREAM_KOS, deferred until first use */ }

void PlayStream(char *name) {
    if (!name || !name[0]) return;
    char base[64];
    ExtractBasenameNoExt(name, base, sizeof(base));
    char ogg[ADELINE_MAX_PATH];
    if (!ResolveOggOnDisc(base, ogg, sizeof(ogg))) {
        LogPrintf("KOS-AICA: PlayStream — no /cd/Music/%s.ogg\n", base);
        streamName[0] = 0;
        return;
    }
    int kVol = Vol127To255(streamVolume > 0 ? streamVolume : cdVolume);
#ifdef LBA2_DEBUG_PERF
    LogPrintf("KOS-AICA: PlayStream engine_name='%s' -> %s\n", name, ogg);
#endif
    // Jingles play ONCE (loop=0). When the OGG ends, IsMusicOggPlaying()
    // returns false, GetMusic() returns 0, and engine MUSIC.CPP's
    // NextMusic / CheckNextMusic mechanism queues the next track from
    // the cube transition. This restores the 1997 chain behavior
    // (e.g. jadpcm15 → 16 → 17 → 18 in Citadel Island outdoor) that
    // the V2.3 "loop everything" workaround had broken.
    if (StartMusicOgg(ogg, /*loop=*/0, kVol)) {
        strncpy(streamName, name, sizeof(streamName) - 1);
        streamName[sizeof(streamName) - 1] = 0;
    } else {
        streamName[0] = 0;
    }
}

void ChangeVolumeStream(S32 volume) {
    streamVolume = volume;
    SetMusicOggVolume(Vol127To255(volume));
}
S32  GetVolumeStream()  { return streamVolume; }
void StopStream()       { StopMusicOgg(); streamName[0] = 0; }
void PauseStream()      { PauseMusicOgg(); }
void ResumeStream()     { ResumeMusicOgg(); }
S32  IsStreamPlaying()  { return IsMusicOggPlaying() ? TRUE : FALSE; }
char *StreamName() {
    // Lazy-clear once the underlying stream is genuinely done (EOF or
    // torn down — paused doesn't count, ResumeMusic relies on us to
    // remember the path). Without this clear, GetMusic() in MUSIC.CPP
    // keeps reading the stale jingle name after a play-once OGG ends,
    // and the NextMusic queue stalls forever (we saw this as "after
    // jadpcm15 finishes nothing else plays" in the Citadel chain).
    if (IsMusicOggDone()) streamName[0] = 0;
    return streamName;
}

const char *OpenCD(char *volume_name) {
    (void)volume_name;
    // Init the CDROM driver so file reads from /cd/ keep working — we
    // don't use cdrom_cdda_play anymore (V2.5 streams OGG via snd_stream)
    // but cdrom_init() is still required for ISO9660 mount to behave.
    cdrom_init();
    return "D";
}

void PlayCD(S32 track) {
    const char *ogg_basename = kEngineToOggName(track);
    if (!ogg_basename) return;
    char path[ADELINE_MAX_PATH];
    if (!ResolveOggOnDisc(ogg_basename, path, sizeof(path))) {
        LogPrintf("KOS-AICA: PlayCD — no /cd/Music/%s.ogg (engine track %d)\n",
                  ogg_basename, (int)track);
        return;
    }
    int kVol = Vol127To255(cdVolume);
#ifdef LBA2_DEBUG_PERF
    LogPrintf("KOS-AICA: PlayCD engine_track=%d -> %s\n", (int)track, path);
#endif
    if (StartMusicOgg(path, /*loop=*/1, kVol)) {
        cdLastTrack = track;
        // Mirror the engine's StreamName/IsStreamPlaying contract — some
        // gameplay code (PlayJingle: "is the same thing already playing?")
        // checks StreamName. Set it to the resolved path so re-PlayCD with
        // the same track is a noop.
        strncpy(streamName, path, sizeof(streamName) - 1);
        streamName[sizeof(streamName) - 1] = 0;
    }
}

void ChangeVolumeCD(S32 volume) {
    cdVolume = volume;
    if (cdLastTrack) SetMusicOggVolume(Vol127To255(volume));
}
S32 GetVolumeCD() { return cdVolume; }

void StopCD() {
    if (!cdLastTrack) return;
    StopMusicOgg();
    cdLastTrack = 0;
    streamName[0] = 0;
}

void PauseCD()  { if (cdLastTrack) PauseMusicOgg();  }
void ResumeCD() { if (cdLastTrack) ResumeMusicOgg(); }

S32 IsCDPlaying() {
    // Trust our own bookkeeping until the stream is genuinely done. Use
    // IsMusicOggDone (active && eof) NOT IsMusicOggPlaying — the latter
    // returns false on PAUSED too, which would prematurely clear cdLastTrack
    // during fade-out / pre-FMV pause and break ResumeMusic.
    if (!cdLastTrack) return FALSE;
    if (IsMusicOggDone()) {
        cdLastTrack = 0;
        streamName[0] = 0;
        return FALSE;
    }
    return cdLastTrack;
}

#ifdef __cplusplus
}
#endif
