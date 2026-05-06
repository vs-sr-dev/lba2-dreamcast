// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// LIB386/AIL/KOS/VIDEO_AUDIO_KOS.CPP
// KOS implementation of the VIDEO_AUDIO.H contract used by PLAYACF.CPP.
// Thin wrapper over STREAM_KOS's CH_VIDEO push-mode channel.

#include <AIL/VIDEO_AUDIO.H>

extern "C" int  StartVideoAudioStream(int rate, int channels, int bits);
extern "C" void PushVideoAudioStream(const void *data, unsigned size);
extern "C" void ResumeVideoAudioStream(void);
extern "C" void PauseVideoAudioStream(void);
extern "C" void StopVideoAudioStream(void);

S32 StartVideoAudio(S32 freq, S32 channels, S32 is16bit) {
    return StartVideoAudioStream((int)freq, (int)channels, is16bit ? 16 : 8);
}

void ResumeVideoAudio(void) { ResumeVideoAudioStream(); }
void PauseVideoAudio(void)  { PauseVideoAudioStream();  }

void PushVideoAudio(const void *data, U32 sizeBytes) {
    PushVideoAudioStream(data, (unsigned)sizeBytes);
}

void StopVideoAudio(void) { StopVideoAudioStream(); }
