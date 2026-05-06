#include <AIL/VIDEO_AUDIO_RESAMPLE.H>

void ResampleAndAddToMix(unsigned long srcRate, unsigned long targetRate,
                         const void *srcPtr, unsigned long srcBytes,
                         void *mixPtr, unsigned long mixSamples, int is16) {
    if (srcRate == 0 || targetRate == 0 || mixSamples == 0)
        return;
    unsigned long srcSamples = srcBytes / (is16 ? 2u : 1u);
    if (srcSamples == 0)
        return;

    if (srcRate == targetRate) {
        unsigned long n = (srcSamples < mixSamples) ? srcSamples : mixSamples;
        if (is16) {
            const short *src = (const short *)srcPtr;
            short *mix = (short *)mixPtr;
            for (unsigned long j = 0; j < n; j++) {
                int s = (int)mix[j] + (int)src[j];
                if (s > 32767)
                    s = 32767;
                if (s < -32768)
                    s = -32768;
                mix[j] = (short)s;
            }
        } else {
            const U8 *src = (const U8 *)srcPtr;
            U8 *mix = (U8 *)mixPtr;
            for (unsigned long j = 0; j < n; j++) {
                int s = (int)(mix[j]) - 128 + (int)(src[j]) - 128;
                if (s > 127)
                    s = 127;
                if (s < -128)
                    s = -128;
                mix[j] = (U8)(s + 128);
            }
        }
        return;
    }

    /* V2.7.2: fast integer-only 2x stretch path. This is the LBA2 intro/final
     * voice case (mono SMK voice 22050 piped into a stereo 22050 mix buffer
     * via the GetEffectiveTrackRate hack that fakes srcRate=11025). The
     * generic linear-interp path below uses double-precision math, which on
     * SH4 is software-emulated by libgcc and costs ~50-200 cycles per op —
     * adding up to ~15 ms per frame on Dreamcast and visibly stuttering the
     * 15 fps SMK as soon as voice (i.e. non-silent voice chunks) kicks in.
     * Integer 2x stretch produces identical output for this case at near-zero
     * CPU cost: even-positions copy src directly, odd-positions are the
     * average of two consecutive src samples (same as linear interp at 0.5). */
    if (targetRate == 2u * srcRate) {
        if (is16) {
            const short *src = (const short *)srcPtr;
            short *mix = (short *)mixPtr;
            unsigned long pairs = mixSamples / 2u;
            if (pairs > srcSamples) pairs = srcSamples;
            for (unsigned long k = 0; k < pairs; k++) {
                int s0 = (int)src[k];
                int s1 = (k + 1 < srcSamples) ? (int)src[k + 1] : s0;
                int even = (int)mix[2u * k] + s0;
                int odd  = (int)mix[2u * k + 1u] + ((s0 + s1) >> 1);
                if (even >  32767) even =  32767;
                if (even < -32768) even = -32768;
                if (odd  >  32767) odd  =  32767;
                if (odd  < -32768) odd  = -32768;
                mix[2u * k]      = (short)even;
                mix[2u * k + 1u] = (short)odd;
            }
        } else {
            const U8 *src = (const U8 *)srcPtr;
            U8 *mix = (U8 *)mixPtr;
            unsigned long pairs = mixSamples / 2u;
            if (pairs > srcSamples) pairs = srcSamples;
            for (unsigned long k = 0; k < pairs; k++) {
                int s0 = (int)src[k] - 128;
                int s1 = ((k + 1 < srcSamples) ? (int)src[k + 1] : (int)src[k]) - 128;
                int even = (int)mix[2u * k] - 128 + s0;
                int odd  = (int)mix[2u * k + 1u] - 128 + ((s0 + s1) >> 1);
                if (even >  127) even =  127;
                if (even < -128) even = -128;
                if (odd  >  127) odd  =  127;
                if (odd  < -128) odd  = -128;
                mix[2u * k]      = (U8)(even + 128);
                mix[2u * k + 1u] = (U8)(odd  + 128);
            }
        }
        return;
    }

    /* Generic resample with linear interpolation. Uses float (SH4 hardware
     * single-precision FPU, ~2 cycles/op) instead of double (~50-200 cycles/op
     * software-emulated on SH4). Hit only for non-2x rate ratios; LBA2's intro
     * doesn't take this path post-V2.7.2 but other future SMK content might. */
    if (is16) {
        const short *src = (const short *)srcPtr;
        short *mix = (short *)mixPtr;
        float ratio = (float)srcSamples / (float)mixSamples;
        for (unsigned long k = 0; k < mixSamples; k++) {
            float idx = (float)k * ratio;
            unsigned long idx0 = (unsigned long)idx;
            if (idx0 >= srcSamples)
                idx0 = srcSamples - 1;
            unsigned long idx1 = idx0 + 1;
            if (idx1 >= srcSamples)
                idx1 = srcSamples - 1;
            float frac = idx - (float)idx0;
            int sample = (int)((1.0f - frac) * (float)src[idx0] + frac * (float)src[idx1]);
            int s = (int)mix[k] + sample;
            if (s > 32767)
                s = 32767;
            if (s < -32768)
                s = -32768;
            mix[k] = (short)s;
        }
    } else {
        const U8 *src = (const U8 *)srcPtr;
        U8 *mix = (U8 *)mixPtr;
        float ratio = (float)srcSamples / (float)mixSamples;
        for (unsigned long k = 0; k < mixSamples; k++) {
            float idx = (float)k * ratio;
            unsigned long idx0 = (unsigned long)idx;
            if (idx0 >= srcSamples)
                idx0 = srcSamples - 1;
            unsigned long idx1 = idx0 + 1;
            if (idx1 >= srcSamples)
                idx1 = srcSamples - 1;
            float frac = idx - (float)idx0;
            int sample = (int)((1.0f - frac) * ((int)(src[idx0]) - 128) + frac * ((int)(src[idx1]) - 128));
            int s = (int)(mix[k]) - 128 + sample;
            if (s > 127)
                s = 127;
            if (s < -128)
                s = -128;
            mix[k] = (U8)(s + 128);
        }
    }
}

unsigned long GetEffectiveTrackRate(int t, const unsigned long *a_r,
                                    const U8 *a_c, unsigned long targetRate) {
    unsigned long r = a_r[t];
    if (t >= 1 && t <= 3 && r == 22050 && (a_c[t] == 1) && targetRate == 22050)
        return 11025u;
    return r;
}
