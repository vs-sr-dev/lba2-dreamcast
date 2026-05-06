// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// KOS-native SVGA presentation backend for Dreamcast.
// Replaces LIB386/SVGA/SDL.CPP. Uses KOS direct framebuffer mode (vid_set_mode +
// vram_s) for V1 — software rasterizer in pol_work writes 8bpp into Log, here
// we convert via palette LUT to 565 and blit into vram. PVR2 hardware path
// will replace this in V2.

#include <SVGA/SDL.H>
#include <SVGA/SCREEN.H>
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/WINDOW.H>

#include <dc/video.h>
#include <kos.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
#define PAL_MAX_COLORS 256

// --- Public state ------------------------------------------------------------
void *Phys;
U32 ModeResX;
U32 ModeResY;
U32 TabOffPhysLine[ADELINE_MAX_Y_RES];

// --- Private state -----------------------------------------------------------
static S32 screenLockCount = 0;
static bool frameDirty = false;
static PrePresentFn s_prePresent = NULL;
static U16 paletteLUT_565[PAL_MAX_COLORS];
static U8 paletteRgb[PAL_MAX_COLORS * 3];

// --- Private functions -------------------------------------------------------
static void PresentFrame();

static inline U16 Rgb565(U8 r, U8 g, U8 b) {
    return (U16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// --- Initialization ----------------------------------------------------------
bool InitVideo() {
    if (!IsWindowInitialized()) {
        LogPrintf("Error: Unable to initialize Video subsystem.\n"
                  "\tWindow subsystem was not initialized beforehand.\n");
        return false;
    }
    // 640x480 16bpp RGB565 — KOS direct framebuffer scanout.
    vid_set_mode(DM_640x480, PM_RGB565);
    return true;
}

void EndVideo() {
    // Empty — KOS handles teardown.
}

// --- Interface ---------------------------------------------------------------
bool CreateVideoSurface(U32 resX, U32 resY) {
    ModeResX = resX;
    ModeResY = resY;

    Phys = malloc((size_t)resX * resY);
    if (!Phys) {
        LogPrintf("Error: Unable to allocate Phys buffer (%lux%lu).\n",
                  (unsigned long)resX, (unsigned long)resY);
        return false;
    }

    S32 off = 0;
    for (S32 i = 0; i < ADELINE_MAX_Y_RES; i++) {
        TabOffPhysLine[i] = off;
        off += VideoSurfacePitch();
    }

    memset(paletteLUT_565, 0, sizeof(paletteLUT_565));
    memset(paletteRgb, 0, sizeof(paletteRgb));

    return true;
}

U32 VideoSurfacePitch() {
    return ModeResX;
}

void LockVideoSurface() {
    ++screenLockCount;
}

void UnlockVideoSurface() {
    if (screenLockCount > 0) {
        --screenLockCount;
    }

    if (screenLockCount == 0 && frameDirty) {
        PresentFrame();
        frameDirty = false;
    }
}

void WaitVideoSync() {
    vid_waitvbl();
}

void SetVideoPalette(const U8 src[], S32 startIdx, S32 count) {
    if (startIdx < 0) startIdx = 0;
    for (S32 i = startIdx; i < count && i < PAL_MAX_COLORS; ++i) {
        U8 r = src[0];
        U8 g = src[1];
        U8 b = src[2];
        paletteLUT_565[i] = Rgb565(r, g, b);
        paletteRgb[i * 3 + 0] = r;
        paletteRgb[i * 3 + 1] = g;
        paletteRgb[i * 3 + 2] = b;
        src += 3;
    }
}

void SetVideoPaletteCol(S32 colorIdx, U8 r, U8 g, U8 b) {
    if (colorIdx < 0 || colorIdx >= PAL_MAX_COLORS) return;
    paletteLUT_565[colorIdx] = Rgb565(r, g, b);
    paletteRgb[colorIdx * 3 + 0] = r;
    paletteRgb[colorIdx * 3 + 1] = g;
    paletteRgb[colorIdx * 3 + 2] = b;
}

void SetVideoPaletteSync(const U8 src[]) {
    SetVideoPalette(src, 0, PAL_MAX_COLORS);
}

void CopyVideoArea(void *dst, const void *src, const U32 tabOffDst[],
                   const T_BOX *area) {
    assert((src != Phys) && "Phys as 'src' is not supported");
    assert((src != dst) && "Copy to same video memory is not supported");

    if (dst == Phys) {
        frameDirty = true;
        return;
    }

    U8 *dstMem = (U8 *)dst;
    U8 *srcMem = (U8 *)src;
    U32 pitch = tabOffDst[1];

    for (int y = area->y0; y < area->y1; ++y) {
        U8 *srcRow = srcMem + (y * pitch) + area->x0;
        U8 *dstRow = dstMem + (y * pitch) + area->x0;
        memcpy(dstRow, srcRow, area->x1 - area->x0);
    }
}

void SetPrePresentCallback(PrePresentFn fn) {
    s_prePresent = fn;
}

static void PresentFrame() {
    assert(Log != NULL);

    // PrePresent callback (e.g. console overlay) expects ARGB. V1 stubs the
    // console out so this is normally NULL on DC; if a future overlay needs
    // a buffer, we render to a scratch ARGB area first. For now skip.
    // TODO V2: ARGB scratch buffer for PrePresent callbacks if needed.

    U8 *logMem = (U8 *)Log;
    volatile U16 *vram = vram_s;

    const U32 viewW = ModeDesiredX;
    const U32 viewH = ModeDesiredY;
    const U32 fbW = 640; // vram pitch in pixels for DM_640x480

    // Center the engine viewport in the 640x480 framebuffer.
    const U32 offX = (fbW > viewW) ? (fbW - viewW) / 2 : 0;
    const U32 offY = (480 > viewH) ? (480 - viewH) / 2 : 0;

    for (U32 y = 0; y < viewH; ++y) {
        const U8 *srcRow = logMem + y * viewW;
        volatile U16 *dstRow = vram + (y + offY) * fbW + offX;
        for (U32 x = 0; x < viewW; ++x) {
            dstRow[x] = paletteLUT_565[srcRow[x]];
        }
    }
}

void HandleEventsVideo(const void *event) {
    (void)event;
}

// =============================================================================
#ifdef __cplusplus
}
#endif
