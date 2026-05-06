#include "C_EXTERN.H"

#include "DIRECTORIES.H"

#ifdef USE_MVIDEO_SMACKER
#include "AMBIANCE.H"
#include "INPUT.H"
#include "MESSAGE.H"
#include <AIL/VIDEO_AUDIO.H>
#include <AIL/VIDEO_AUDIO_RESAMPLE.H>
#include <SYSTEM/HQFILE.H>
#include <SYSTEM/HQR.H>

static int videoLogEnabled = 0;

void VideoLogEnable(int enable) {
    videoLogEnabled = (enable != 0) ? 1 : 0;
}

int VideoLogIsEnabled(void) {
    return videoLogEnabled;
}
#include <SVGA/DIRTYBOX.H>
#include <SVGA/SCREEN.H>
#include <SVGA/VIDEO.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include <SYSTEM/LOGPRINT.H>
#ifdef LBA2_TARGET_DREAMCAST
#include <SYSTEM/dc_sdl_compat.h>
#else
#include <SDL3/SDL.h>
#endif
#include <stdio.h>
#include <string.h>
#include "CONSOLE/CONSOLE.H"
#endif

char PathAcf[ADELINE_MAX_PATH];

char *ListAcf;
char *EndListAcf;

#ifdef LBA_EDITOR
#define SIZE_BUFFER_SMACK 1000000L
U8 *BufferSmack;
#else
U8 *BufferSmack; // initialisé dans MEM.CPP
#endif

//---------------------------------------------------------------------------

/***************************************************************************\
|*									   *|
|*				INITS					   *|
|*									   *|
\***************************************************************************/

//---------------------------------------------------------------------------
void InitAcf(void) {
    char tmpFilePath[ADELINE_MAX_PATH];

    GetMoviePath(PathAcf, ADELINE_MAX_PATH, VIDEO_HQR_NAME);
#ifndef USE_MVIDEO_NULL
    // The null video backend never plays FMV; the engine doesn't need video.hqr
    // to exist on disc. Skip the hard-fail check so DC builds (which omit the
    // 224 MB VIDEO/ folder from the CD image) don't TheEnd() here.
    if (!ExistsFileOrDir(PathAcf)) {
        TheEndCheckFile(PathAcf);
    }
#endif

    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    ListAcf = (char *)LoadMalloc_HQR(tmpFilePath, RESS_ACFLIST);
    if (!ListAcf) {
        TheEndCheckFile(tmpFilePath);
    }

    EndListAcf = ListAcf + LoadMallocFileSize;

#ifdef LBA_EDITOR
    // dispo Screen,ScreenAux,BufSpeak
    SmackRegisterMemory(Screen, (ModeDesiredX * ModeDesiredY + RECOVER_AREA) * 3);

    BufferSmack = (U8 *)Malloc(SIZE_BUFFER_SMACK);
    if (!BufferSmack)
        TheEnd(NOT_ENOUGH_MEM, "BufferSmack (InitAcf())");

    SmackRegisterMemory((void *)BufferSmack, SIZE_BUFFER_SMACK);
#else
    // TODO: Check if still useful when movie playback implemented
    // SmackRegisterMemory( MainBuffer, SizeOfMemAllocated ) ;
#endif
}

//---------------------------------------------------------------------------

char *PtrAcf;

char *GetAcfName(char *name) {
    char *ptr = name;
    char c;

    do /* saute cr/lf espace */
    {
        if (PtrAcf > EndListAcf) {
            PtrAcf--;
            return 0L;
        }

        c = *PtrAcf++;
    } while (c <= 32);

    do {
        *ptr++ = c;
        c = *PtrAcf++;
    } while (c > 32);

    if (PtrAcf > EndListAcf) {
        PtrAcf--;
        return 0L;
    }

    if (c == 26)
        PtrAcf--;

    *ptr++ = 0;
    return name;
}

//---------------------------------------------------------------------------
void ResetAcfList(void) {
    PtrAcf = ListAcf;
}

//---------------------------------------------------------------------------
// TODO: New implementation was not tested yet
S32 GetNumAcf(const char *name) {
    S32 n = 0;

    const char *nameDotAt = strchr(name, '.');
    const size_t nameNoDotLen = (nameDotAt)
                                    ? (nameDotAt - name)
                                    : strlen(name);

    char getname[15];
    PtrAcf = ListAcf;
    while (GetAcfName(getname)) {
        const char *getNameDotAt = strchr(getname, '.');
        const size_t getNameNoDotLen = (getNameDotAt)
                                           ? (getNameDotAt - getname)
                                           : strlen(getname);

        const bool sameLen = (nameNoDotLen == getNameNoDotLen);
        if (sameLen && !strncasecmp(name, getname, nameNoDotLen)) {
            return n;
        }

        n++;
    }

    return -1;
}

//---------------------------------------------------------------------------

void PlayAllAcf(void) {
    S32 firsttime = TRUE;
    S32 n = 0;
    S32 var = FLAG_ACF;
    char getname[13];

    if (!ListAcf || !EndListAcf || ListAcf >= EndListAcf)
        return;

    PtrAcf = ListAcf;

#if defined(LBA_EDITOR) || defined(CDROM)
    FadeOutVolumeMusic();
#endif

    while (GetAcfName(getname)) {
#ifdef LBA_EDITOR
        if ((ListVarGame[var].Value & (1 << n))
#else
        if ((ListVarGame[var] & (1 << n))
#endif
                AND(strncasecmp(getname, "INTRO", 5))) // on ne rejoue pas l'intro
        {
            if (!FlagFade) {
                if (firsttime) {
                    FadeToBlackAndSamples(PtrPal);
                    firsttime = FALSE;
                } else
                    FadeToBlack(PtrPal);
            }

            FlagFade = TRUE;

            PlayAcf(getname);
            //			if( PlayAcf( getname ) == K_ESC )	break ;
        }

        n++;

        if (n >= 16) {
            var++;
            n = 0;
        }
    }

#if defined(LBA_EDITOR) || defined(CDROM)
    FadeInVolumeMusic();
#endif

    FirstTime = AFF_ALL_FLIP;
}

//---------------------------------------------------------------------------

//extern	void	CopyFrameDoubleXY( U8 *ptrsrc ) ;

/***************************************************************************\
|*									   *|
|*				MAIN					   *|
|*									   *|
\***************************************************************************/

#ifdef USE_MVIDEO_SMACKER
static void ReadNextVideoFrame(smk smkObject, U8 *buffer, U8 **pal) {
    const U32 width = 320;
    const U32 height = 200;
    const U32 screenWidth = 640;
    const U32 videoStartY = 40;
    U8 *frame = (U8 *)smk_get_video(smkObject);
    *pal = (U8 *)smk_get_palette(smkObject);
    memset(buffer, 0, screenWidth * 480);
    U8 *targetRow = buffer + videoStartY * screenWidth;
    for (U32 y = 0; y < height; y++) {
        const U8 *sourceRow = frame + y * width;
        for (U32 x = 0; x < width; x++) {
            targetRow[2 * x] = sourceRow[x];
            targetRow[2 * x + 1] = sourceRow[x];
        }
        memcpy(targetRow + screenWidth, targetRow, screenWidth);
        targetRow += screenWidth * 2;
    }
}

/* Push current frame's audio for requested tracks; mix if numTracks > 1. Resample to stream rate with duration-correct resampling. */
static void PushCurrentFrameVideoAudio(smk smkObject, const int *tracksToPlay, int numTracks, const U8 *a_d, const unsigned long *a_r, const U8 *a_c, double usf) {
    if (numTracks <= 0)
        return;
    const unsigned long kMaxFrameBytes = 128 * 1024;
    static U8 mixBuf[128 * 1024];
    unsigned long sizeBytes[7];
    const unsigned char *chunk[7];
    int first = tracksToPlay[0];
    unsigned long targetRate = a_r[first];
    if (targetRate == 0)
        return;
    int chans = (a_c[first] >= 2) ? 2 : 1;
    int is16 = (a_d[first] == 16) ? 1 : 0;
    /* Bytes per frame at stream rate (fallback when a frame has no audio) */
    double frameSec = (usf > 0.0) ? (usf / 1e6) : (1.0 / 30.0);
    unsigned long expectedBytes = (unsigned long)((double)targetRate * frameSec * (double)chans * (is16 ? 2.0 : 1.0));
    if (expectedBytes > kMaxFrameBytes)
        expectedBytes = kMaxFrameBytes;

    unsigned long maxSize = 0;
    for (int i = 0; i < numTracks; i++) {
        int t = tracksToPlay[i];
        sizeBytes[i] = smk_get_audio_size(smkObject, (unsigned char)t);
        chunk[i] = smk_get_audio(smkObject, (unsigned char)t);
        if (chunk[i] && sizeBytes[i] > 0) {
            unsigned long srcRate = GetEffectiveTrackRate(t, a_r, a_c, targetRate);
            unsigned long effectiveBytes = (srcRate == targetRate) ? sizeBytes[i] : (unsigned long)((double)sizeBytes[i] * (double)targetRate / (double)srcRate);
            if (effectiveBytes > maxSize)
                maxSize = effectiveBytes;
        }
    }
    /* Use chunk-based size so we push actual data; fall back to expected size if frame is silent */
    if (maxSize == 0)
        maxSize = expectedBytes;
    if (maxSize == 0)
        return;
    if (maxSize > kMaxFrameBytes)
        maxSize = kMaxFrameBytes;

    unsigned long mixSamples = maxSize / (is16 ? 2u : 1u);

    memset(mixBuf, is16 ? 0 : 128, (size_t)maxSize);
    int numAdded = 0;
    for (int i = 0; i < numTracks; i++) {
        if (!chunk[i] || sizeBytes[i] == 0)
            continue;
        ResampleAndAddToMix(GetEffectiveTrackRate(tracksToPlay[i], a_r, a_c, targetRate), targetRate, chunk[i], sizeBytes[i], mixBuf, mixSamples, is16);
        numAdded++;
    }
    /* Scale mix when multiple tracks were added to avoid clipping distortion (grain). Use rounding to reduce ticking from truncation. */
    if (numAdded > 1) {
        int half = numAdded / 2;
        if (is16) {
            short *mix = (short *)mixBuf;
            for (unsigned long j = 0; j < mixSamples; j++) {
                int s = (int)mix[j];
                s = (s + (s >= 0 ? half : -half)) / numAdded;
                if (s > 32767)
                    s = 32767;
                if (s < -32768)
                    s = -32768;
                mix[j] = (short)s;
            }
        } else {
            U8 *mix = mixBuf;
            for (unsigned long j = 0; j < mixSamples; j++) {
                int s = (int)(mix[j]) - 128;
                s = (s + (s >= 0 ? half : -half)) / numAdded + 128;
                if (s < 0)
                    s = 0;
                if (s > 255)
                    s = 255;
                mix[j] = (U8)s;
            }
        }
    }
    PushVideoAudio(mixBuf, (U32)maxSize);
}
#endif

//---------------------------------------------------------------------------

#ifdef LBA2_TARGET_DREAMCAST
// V2.7: open an HQR record as a streamable FILE* positioned at the first byte
// of raw record data (past the 10-byte COMPRESSED_HEADER). Used to disk-stream
// large SMKs (notably INTRO at ~76 MB decompressed) without allocating the
// full record in main RAM, which would OOM on the DC's ~12 MB user budget.
//
// Only works when CompressMethod == 0 (stored). SMK is already compressed by
// its own codec so HQR re-compression would have negligible benefit; in
// practice the FMV records are stored. If a record turns out to be LZ-compressed
// the helper returns NULL and PlayAcf falls back to the memory-mode path with
// the 8 MB PRE-skip ceiling.
//
// libsmacker's smk_open_filepointer captures chunk offsets via ftell() and
// seeks SEEK_SET, so a FILE* positioned mid-file at the SMK's first byte
// works correctly — chunk_offset values are absolute in the underlying file.
static FILE *OpenHqrRecordAsFile(const char *hqrPath, S32 index, U32 *outSize) {
    if (!hqrPath || index < 0) return NULL;
    FILE *fp = fopen(hqrPath, "rb");
    if (!fp) return NULL;

    S32 nbbloc = 0;
    if (fread(&nbbloc, 4, 1, fp) != 1) { fclose(fp); return NULL; }
    if ((index * 4) >= nbbloc) { fclose(fp); return NULL; }

    if (fseek(fp, index * 4, SEEK_SET) != 0) { fclose(fp); return NULL; }
    S32 recordOffset = 0;
    if (fread(&recordOffset, 4, 1, fp) != 1) { fclose(fp); return NULL; }
    if (recordOffset == 0) { fclose(fp); return NULL; }

    if (fseek(fp, recordOffset, SEEK_SET) != 0) { fclose(fp); return NULL; }
    COMPRESSED_HEADER hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return NULL; }

    if (hdr.CompressMethod != 0) {
        fclose(fp);
        return NULL;
    }

    // V2.7.1: enlarge the stdio buffer to smooth out per-frame fseek+fread
    // pressure on the GD-ROM. KOS's default BUFSIZ is 1 KB which means almost
    // every libsmacker chunk read crosses a buffer boundary and costs a real
    // CD seek. With a 64 KB buffer most chunks land within the prefetched
    // window and become memcpy. Frees up the 15 fps native cadence.
    static char s_smkIoBuffer[65536];
    setvbuf(fp, s_smkIoBuffer, _IOFBF, sizeof(s_smkIoBuffer));

    if (outSize) *outSize = hdr.SizeFile;
    return fp;
}

// V2.7.1: parallel-music helper REMOVED.
// First V2.7 build assumed (incorrectly) that the SMK INTRO had voice-only
// (case B) and tried to PlayMusic(0)→track1.ogg in parallel. The runtime
// diagnostic showed instead the SMK has track 0 = stereo 22kHz music baked
// (case A), AND the parallel track1.ogg malloc OOMed at 4.58 MB on top of
// the disk-streaming SMK + engine state. Both reasons say not to do it:
// playing both would double the music; loading the OGG won't fit in RAM.
// track1.ogg is kept on the CDI as a (higher-quality) standalone asset for
// possible future use, but PlayAcf relies on the SMK's baked track 0.
#endif

S32 PlayAcf(const char *name) {
#ifdef USE_MVIDEO_SMACKER
    {
        static int s_playacf_playing = 0;
        if (s_playacf_playing) {
            /* Re-entry guard: avoid same video starting repeatedly (e.g. track/life script every frame). */
            return 0;
        }
        s_playacf_playing = 1;
        LBA2_DC_TRACE("[DC-TRACE] PlayAcf(%s) entry\n", name ? name : "(null)");

        S32 n = GetNumAcf(name);
        if (n == -1) {
            if (VideoLogIsEnabled())
                SDL_Log("[VIDEO] PlayAcf: video name '%s' not found in ACF list", name);
            s_playacf_playing = 0;
            return 0;
        }

        ListVarGame[FLAG_ACF + (n / 16)] |= (S16)(1 << (n & 15));

        StopMusic();
        HQ_PauseSamples();

        /* Save current screen and palette so we can restore after playback (fix black screen). */
        const U32 logSize = (U32)(ModeDesiredX * ModeDesiredY);
        const U32 palSize = 768;
        U8 *savedLog = (U8 *)malloc(logSize + palSize);
        if (!savedLog) {
            if (VideoLogIsEnabled())
                SDL_Log("[VIDEO] PlayAcf: malloc failed for screen save");
            HQ_ResumeSamples();
            s_playacf_playing = 0;
            return 0;
        }
        U8 *savedPal = savedLog + logSize;
        if (Log && PtrPal) {
            memcpy(savedLog, Log, logSize);
            memcpy(savedPal, PtrPal, palSize);
        }

        U8 *decompbuf = NULL;
        U32 size = 0;
        smk smkObject = NULL;

#ifdef LBA2_TARGET_DREAMCAST
        // V2.7: disk-streaming path — try smk_open_filepointer with SMK_MODE_DISK
        // first to avoid allocating the full decompressed record in main RAM.
        // INTRO.SMK is ~76 MB decompressed which OOMs DC; previous V2.6.1 ceiling
        // soft-skipped any record > 8 MB. This path bypasses the ceiling entirely
        // when the record is uncompressed (CompressMethod == 0, the standard
        // case for SMK in HQR). On compressed records the helper returns NULL
        // and we fall back to the V2.6.1 ceiling + memory-mode path below.
        FILE *streamFp = NULL;
        U32 streamRawSize = 0;
        streamFp = OpenHqrRecordAsFile(PathAcf, n, &streamRawSize);
        if (streamFp) {
            smkObject = smk_open_filepointer(streamFp, SMK_MODE_DISK);
            if (smkObject) {
                SDL_Log("[VIDEO] PlayAcf: disk-streaming '%s' index %d raw=%u bytes\n",
                        name, (int)n, (unsigned)streamRawSize);
            } else {
                SDL_Log("[VIDEO] PlayAcf: smk_open_filepointer failed for '%s' index %d, falling back\n",
                        name, (int)n);
                fclose(streamFp);
                streamFp = NULL;
            }
        }
#endif

#ifdef LBA2_TARGET_DREAMCAST
        // Memory-mode fallback (used when disk-streaming is unavailable, e.g.
        // for compressed HQR records, or on non-DC builds). Keeps the V2.6.1
        // 8 MB PRE-skip ceiling so OOM doesn't crash the kernel.
        if (!smkObject) {
            S32 hqrSize = HQF_ResSize(PathAcf, n);
            if (hqrSize <= 0 || hqrSize > 8 * 1024 * 1024) {
                SDL_Log("[VIDEO] PlayAcf: PRE-skip FMV %d (HQF_ResSize=%d bytes; >8MB ceiling, no disk-stream)\n",
                        (int)n, (int)hqrSize);
                if (savedLog) free(savedLog);
                HQ_ResumeSamples();
                s_playacf_playing = 0;
                return 0;
            }
        }
#endif

        if (!smkObject) {
            decompbuf = (U8 *)LoadMalloc_HQR(PathAcf, n);
            size = LoadMallocFileSize;
        }
        if (!smkObject && !size) {
#ifdef LBA2_TARGET_DREAMCAST
            // Belt-and-suspenders: even with the pre-check above, if anything
            // goes wrong inside LoadMalloc_HQR (HQR header malformed, fread
            // short, etc.) we fail soft. Should never trigger now.
            SDL_Log("[VIDEO] PlayAcf: post-load skip FMV %d (LoadMalloc_HQR returned 0)\n",
                    (int)n);
            if (savedLog) free(savedLog);
            HQ_ResumeSamples();
            s_playacf_playing = 0;
            return 0;
#else
            if (VideoLogIsEnabled())
                SDL_Log("[VIDEO] PlayAcf: LoadMalloc_HQR failed for index %d", (int)n);
            TheEnd(PROGRAM_OK, MessageNoCD);
#endif
        }

        if (!smkObject) {
            smkObject = smk_open_memory(decompbuf, (unsigned long)size);
        }
        if (!smkObject) {
#ifdef LBA2_TARGET_DREAMCAST
            // Same fail-soft policy as above for malformed/unsupported .smk.
            SDL_Log("[VIDEO] PlayAcf: smk_open failed for index %d size %u\n",
                    (int)n, (unsigned)size);
            if (decompbuf) Free(decompbuf);
            if (savedLog) free(savedLog);
            HQ_ResumeSamples();
            s_playacf_playing = 0;
            return 0;
#else
            if (VideoLogIsEnabled())
                SDL_Log("[VIDEO] PlayAcf: smk_open_memory failed for index %d (size %u)", (int)n, (unsigned)size);
            Free(decompbuf);
            TheEnd(PROGRAM_OK, MessageNoCD);
#endif
        }

        SetBlackPal();
        BoxReset();
        Cls();
        BoxUpdate();
        InitWaitNoKey();
        InitWaitNoInput(I_JOY | I_FIRE | I_MENUS);

        unsigned long w = 0, h = 0;
        unsigned long f = 0;
        double usf = 0.0;
        smk_info_all(smkObject, NULL, &f, &usf);
        smk_info_video(smkObject, &w, &h, NULL);
        if (w != 320 || h != 200) {
            smk_close(smkObject);
#ifdef LBA2_TARGET_DREAMCAST
            if (streamFp) fclose(streamFp);
#endif
            if (decompbuf) Free(decompbuf);
            TheEnd(PROGRAM_FAIL, "Unsupported video format (320x200 only)");
        }

        U8 a_t = 0, a_c[7] = {0}, a_d[7] = {0};
        unsigned long a_r[7] = {0};
        smk_info_audio(smkObject, &a_t, a_c, a_d, a_r);

#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
        // V2.7 diag: log which audio tracks the SMK actually exposes. We need
        // this to confirm whether the music is baked in track 0 (older
        // assumption) or only voice tracks 1/2/3 are present and music must
        // come from a parallel OGG (track1.ogg in our asset rip).
        SDL_Log("[VIDEO-DIAG] PlayAcf '%s': SMK a_t mask=0x%02X video=%lux%lu frames=%lu usf=%.2f",
                name, (unsigned)a_t, w, h, f, usf);
        for (int dt = 0; dt < 7; dt++) {
            int present = (a_t & (1 << dt)) ? 1 : 0;
            SDL_Log("[VIDEO-DIAG]   track%d present=%d rate=%lu depth=%u chans=%u",
                    dt, present, a_r[dt], (unsigned)a_d[dt], (unsigned)a_c[dt]);
        }
#endif

        S32 smackflags = SMK_AUDIO_TRACK_0;
        if (!strncasecmp(name, "INTRO", 5) && FlagSpeak) {
            switch (LanguageCD) {
            case 0:
                smackflags |= SMK_AUDIO_TRACK_3;
                break;
            case 1:
                smackflags |= SMK_AUDIO_TRACK_1;
                break;
            case 2:
                smackflags |= SMK_AUDIO_TRACK_2;
                break;
            default:
                smackflags |= SMK_AUDIO_TRACK_3;
                break;
            }
        }

        /* .smk uses track 0 = music, tracks 1/2/3 = FR/DE/EN voice; we mix all requested tracks into one stream so intro has both music and voice. */
        int tracksToPlay[7];
        int numTracks = 0;
        for (int i = 0; i < 7; i++) {
            if ((a_t & (1 << i)) && (smackflags & (1 << i)))
                tracksToPlay[numTracks++] = i;
        }
        if (numTracks > 0) {
            int first = tracksToPlay[0];
            S32 chans = (a_c[first] >= 2) ? 2 : 1;
            S32 is16 = (a_d[first] == 16) ? 1 : 0;
            if (!StartVideoAudio((S32)(U32)a_r[first], chans, is16)) {
                /* non-fatal: continue without video audio */
                numTracks = 0;
            }
        }

        if (VideoLogIsEnabled()) {
            if (!strncasecmp(name, "INTRO", 5) && numTracks > 0) {
                static const char *voiceLang[] = {"FR", "DE", "EN"};
                const char *lang = (LanguageCD >= 0 && LanguageCD <= 2) ? voiceLang[LanguageCD] : "FR";
                if (numTracks > 1) {
                    SDL_Log("[VIDEO] PlayAcf: playing '%s' index %d from %s (LanguageCD=%d, voice=%s track %d)",
                            name, (int)n, PathAcf, LanguageCD, lang, tracksToPlay[1]);
                } else {
                    SDL_Log("[VIDEO] PlayAcf: playing '%s' index %d from %s (LanguageCD=%d, voice=off)",
                            name, (int)n, PathAcf, LanguageCD);
                }
            } else {
                SDL_Log("[VIDEO] PlayAcf: playing '%s' index %d from %s", name, (int)n, PathAcf);
            }
        }

        smk_enable_all(smkObject, SMK_VIDEO_TRACK | a_t);
        smk_first(smkObject);

        U8 *dest = (U8 *)malloc(640 * 480);
        if (!dest) {
            StopVideoAudio();
            smk_close(smkObject);
#ifdef LBA2_TARGET_DREAMCAST
            if (streamFp) fclose(streamFp);
#endif
            if (decompbuf) Free(decompbuf);
            if (savedLog)
                free(savedLog);
            HQ_ResumeSamples();
            s_playacf_playing = 0;
            return 0;
        }

        /* Frame interval in ms from file (usf = microseconds per frame) for accurate pacing */
        U32 frameMs = (U32)(usf / 1000.0);
        if (frameMs == 0)
            frameMs = 67; /* ~15 fps */

        /* Pre-fill audio buffer before starting playback (like lba2-classic-ida's SoLoud deque).
     * Stream is created paused; we fill it then resume so the device never underruns.
     * Formula: ~2s for long videos (60+ fps), ~1s for medium (30+), ~0.5s for short; avoids
     * underrun at start while not delaying playback too much on brief clips. */
        unsigned long prebufFrames = 0;
        if (numTracks > 0 && f > 8u) {
            prebufFrames = (f > 60u) ? 30u : (f > 30u) ? 25u
                                         : (f > 15u)   ? 15u
                                                       : (unsigned long)(f / 2);
            if (prebufFrames > f)
                prebufFrames = f;
            for (unsigned long p = 0; p < prebufFrames; p++) {
                PushCurrentFrameVideoAudio(smkObject, tracksToPlay, numTracks, a_d, a_r, a_c, usf);
                smk_next(smkObject);
            }
            smk_first(smkObject);
            ResumeVideoAudio();
        } else if (numTracks > 0) {
            ResumeVideoAudio();
        }

        U8 *pal = NULL;
        U32 videoStartTicks = 0;
        static int s_wasConsoleOpen = 0;
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
        // V2.7.4 timing instrumentation: per-frame breakdown logged every 30
        // frames with max + total. Reveals which phase eats the budget.
        U32 dbgPushSum=0, dbgPushMax=0;
        U32 dbgReadSum=0, dbgReadMax=0;
        U32 dbgCopySum=0, dbgCopyMax=0;
        U32 dbgBoxSum=0,  dbgBoxMax=0;
        U32 dbgNextSum=0, dbgNextMax=0;
        U32 dbgFrames=0;
        // V2.7.5: pull libsmacker-internal microsecond counters so we can
        // split smk_next time into IO (fseek+fread) vs decode (huff trees +
        // DPCM unpack + video chunk).
        extern unsigned long smk_dbg_io_us;
        extern unsigned long smk_dbg_dec_us;
        extern unsigned long smk_dbg_skip_seek;
        unsigned long dbgIoUsBaseline  = smk_dbg_io_us;
        unsigned long dbgDecUsBaseline = smk_dbg_dec_us;
        unsigned long dbgSkipBaseline  = smk_dbg_skip_seek;
#endif
        for (unsigned long c = 0; c < f; c++) {
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
            U32 t0 = SDL_GetTicks();
#endif
            /* Push this frame's audio (skip for c < prebufFrames, already in stream from pre-fill) */
            if (numTracks > 0 && c >= prebufFrames) {
                PushCurrentFrameVideoAudio(smkObject, tracksToPlay, numTracks, a_d, a_r, a_c, usf);
            }
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
            U32 t1 = SDL_GetTicks();
#endif
            ReadNextVideoFrame(smkObject, dest, &pal);
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
            U32 t2 = SDL_GetTicks();
#endif
            CopyBlock(0, 0, 640, 480, dest, 0, 0, Log);
            if (pal)
                PaletteSync(pal);
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
            U32 t3 = SDL_GetTicks();
#endif
            BoxStaticAdd(0, 0, (S32)(ModeDesiredX - 1), (S32)(ModeDesiredY - 1));
            BoxUpdate();
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
            U32 t4 = SDL_GetTicks();
            U32 dPush = t1 - t0;
            U32 dRead = t2 - t1;
            U32 dCopy = t3 - t2;
            U32 dBox  = t4 - t3;
            dbgPushSum += dPush; if (dPush > dbgPushMax) dbgPushMax = dPush;
            dbgReadSum += dRead; if (dRead > dbgReadMax) dbgReadMax = dRead;
            dbgCopySum += dCopy; if (dCopy > dbgCopyMax) dbgCopyMax = dCopy;
            dbgBoxSum  += dBox;  if (dBox  > dbgBoxMax)  dbgBoxMax  = dBox;
#endif

            /* Pace to real time with SDL so speed is correct and sleep reduces stutter */
            if (c == 0)
                videoStartTicks = SDL_GetTicks();
            U32 now = SDL_GetTicks();
            U32 nextAt = videoStartTicks + (c + 1) * frameMs;
            for (;;) {
                MyGetInput();
                ManageTime();
                if (MyKey == K_ESC || (Input & I_MENUS))
                    goto fin_play;
                /* Pause video while console is open: keep presenting current frame until they close it.
         * Pause video audio so the buffer doesn't drain and cause a gap when console closes. */
                if (Console_IsOpen()) {
                    if (!s_wasConsoleOpen && numTracks > 0) {
                        PauseVideoAudio();
                        if (VideoLogIsEnabled())
                            SDL_Log("[VIDEO] Audio paused (console open)");
                        s_wasConsoleOpen = 1;
                    }
                    BoxStaticAdd(0, 0, (S32)(ModeDesiredX - 1), (S32)(ModeDesiredY - 1));
                    BoxUpdate();
                    continue;
                }
                if (s_wasConsoleOpen && numTracks > 0) {
                    ResumeVideoAudio();
                    if (VideoLogIsEnabled())
                        SDL_Log("[VIDEO] Audio resumed (console closed)");
                    s_wasConsoleOpen = 0;
                }
                now = SDL_GetTicks();
                if (now >= nextAt)
                    break;
                SDL_Delay(1); /* sleep 1ms to avoid busy-spin, re-check input each time */
            }

#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
            U32 tn0 = SDL_GetTicks();
#endif
            smk_next(smkObject);
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
            U32 tn1 = SDL_GetTicks();
            U32 dNext = tn1 - tn0;
            dbgNextSum += dNext; if (dNext > dbgNextMax) dbgNextMax = dNext;
            dbgFrames++;
            if (dbgFrames >= 30u) {
                unsigned long ioDeltaUs  = smk_dbg_io_us  - dbgIoUsBaseline;
                unsigned long decDeltaUs = smk_dbg_dec_us - dbgDecUsBaseline;
                unsigned long skipDelta  = smk_dbg_skip_seek - dbgSkipBaseline;
                SDL_Log("[VID-PERF] frame %lu: avg(ms) push=%u read=%u copy=%u box=%u next=%u | max next=%u | smk-internal avg(us) io=%lu dec=%lu | seek-skips=%lu/30\n",
                        c,
                        (unsigned)(dbgPushSum/dbgFrames),
                        (unsigned)(dbgReadSum/dbgFrames),
                        (unsigned)(dbgCopySum/dbgFrames),
                        (unsigned)(dbgBoxSum/dbgFrames),
                        (unsigned)(dbgNextSum/dbgFrames),
                        (unsigned)dbgNextMax,
                        ioDeltaUs / dbgFrames,
                        decDeltaUs / dbgFrames,
                        skipDelta);
                dbgPushSum=dbgReadSum=dbgCopySum=dbgBoxSum=dbgNextSum=0;
                dbgPushMax=dbgReadMax=dbgCopyMax=dbgBoxMax=dbgNextMax=0;
                dbgFrames=0;
                dbgIoUsBaseline  = smk_dbg_io_us;
                dbgDecUsBaseline = smk_dbg_dec_us;
                dbgSkipBaseline  = smk_dbg_skip_seek;
            }
#endif
        }

    fin_play:
        s_wasConsoleOpen = 0;
        StopVideoAudio();
        smk_close(smkObject);
#ifdef LBA2_TARGET_DREAMCAST
        if (streamFp) fclose(streamFp);
#endif
        free(dest);
        if (decompbuf) Free(decompbuf);

        /* Restore screen and palette so we don't leave a black screen. */
        if (savedLog && Log && PtrPal) {
            memcpy(Log, savedLog, logSize);
            memcpy(PtrPal, savedLog + logSize, palSize);
            PaletteSync(PtrPal);
        }
        if (savedLog)
            free(savedLog);

        BoxStaticFullflip();
        HQ_ResumeSamples();
        InitWaitNoKey();
        InitWaitNoInput(I_MENUS);
        s_playacf_playing = 0;
        return MyKey;
    }
#else
    fprintf(stderr, "PlayAcf: '%s' skipped (video backend is null; build with -DMVIDEO_BACKEND=smacker for video)\n", name);
    return 0;
#endif
}

//---------------------------------------------------------------------------
