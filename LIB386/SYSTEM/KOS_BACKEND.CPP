// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// KOS-native backend replacing SDL3-using SYSTEM files on Dreamcast.
// Provides symbols normally exported by:
//   AVAILMEM.CPP DISPOS.CPP EVENTS.CPP KEYBOARD.CPP MOUSE.CPP
//   STRING.CPP TIMER.CPP WINDOW.CPP
//
// V1 scope: enough to LINK and run the game loop. Visual output goes through
// LIB386/SVGA/KOS.CPP (separate file). Input is stubbed; KOS Maple bus wiring
// belongs in V2.

#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/AVAILMEM.H>
#include <SYSTEM/DISPOS.H>
#include <SYSTEM/EVENTS.H>
#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/MOUSE.H>
#include <SYSTEM/STRING.H>
#include <SYSTEM/TIMER.H>
#include <SYSTEM/WINDOW.H>

#include <kos.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// libc gap fillers — utime() lives in <utime.h> on POSIX but KOS newlib for
// SH4 doesn't provide it. FILES.CPP::Touch is the only caller; saves don't
// strictly need mtime updates on DC.
// =============================================================================
struct utimbuf;
int utime(const char *path, const struct utimbuf *times) {
    (void)path; (void)times;
    return 0;
}

// =============================================================================
// AVAILMEM.CPP replacement
// =============================================================================
U64 AvailableMem() {
    // Dreamcast has 16 MiB of system RAM; KOS reserves some for the kernel.
    // Returning the total is consistent with the SDL backend semantics
    // (SDL_GetSystemRAM also returns total, not available).
    return 16ULL * 1024 * 1024;
}

// =============================================================================
// DISPOS.CPP replacement
// =============================================================================
#define OS_DOS   0
#define OS_WIN31 1
#define OS_WIN95 2
#define OS_NT    3

U32 OSType = OS_NT; // CPU.ASM still references this; harmless on SH4.

void DisplayOS() {
    LogPrintf("Platform: Dreamcast (KallistiOS)\n");
}

// =============================================================================
// TIMER.CPP replacement
// =============================================================================
volatile U32 TimerSystemHR;
volatile U32 TimerRefHR;
volatile S32 CmptFrame;
volatile S32 NbFramePerSecond;
volatile S32 TimerLock;
volatile S32 CmptMemoTimerRef;
static U32 MemoTimerRefHR;
U32 LastTime;
U32 LastEvaluate;

static U32 KosTicks() {
    return (U32)timer_ms_gettime64();
}

void InitTimer() {
    LastTime = LastEvaluate = KosTicks();
    ManageTime();
}

void EndTimer() {
    // Empty
}

void LockTimer() {
    ManageTime();
    ++TimerLock;
}

void UnlockTimer() {
    if (TimerLock > 0) {
        ManageTime();
        --TimerLock;
    }
}

void SaveTimer() {
    if (!CmptMemoTimerRef++) {
        ManageTime();
        MemoTimerRefHR = TimerRefHR;
    }
}

void RestoreTimer() {
    if (CmptMemoTimerRef) {
        if (!--CmptMemoTimerRef) {
            ManageTime();
            TimerRefHR = MemoTimerRefHR;
        }
    }
}

void SetTimerHR(U32 time) {
    LockTimer();
    TimerRefHR = time;
    UnlockTimer();
}

// Forward decl of the AIL/KOS streaming pump. Lives in
// LIB386/AIL/KOS/STREAM_KOS.CPP. We don't pull a header to keep the SYSTEM
// layer free of AIL-specific types — the function is C-linkage and weak in
// practice (only emitted when SOUND_BACKEND=kos). When SOUND_BACKEND=null
// or "miles" the link will fail; that's intentional, the pump is part of
// the kos sound contract.
extern "C" void AudioStreamPump(void);

void ManageTime() {
    TimerSystemHR = KosTicks();

    if (!TimerLock) {
        TimerRefHR += TimerSystemHR - LastTime;
    }

    LastTime = TimerSystemHR;

    S32 elapsed = TimerSystemHR - LastEvaluate;
    if (elapsed >= 1000) {
        NbFramePerSecond = (CmptFrame * 1000 + elapsed / 2) / elapsed;
        CmptFrame = 0;
        LastEvaluate += (elapsed / 1000) * 1000;
    }

    // Refill any active AICA streams with fresh PCM. snd_stream needs an
    // explicit poll — there's no kernel-side autopump in KOS. ManageTime
    // is the engine's per-frame heartbeat (called from MyGetInput,
    // PlayAcf inner loop, sample driver, etc.) so this is the right hook.
    AudioStreamPump();
}

void HandleEventsTimer(const void *event) {
    (void)event;
    // No window focus events on DC.
}

// =============================================================================
// WINDOW.CPP replacement
// =============================================================================
bool AppActive = true; // DC has no concept of background; always active.
static bool windowSystemInitialized = false;
static U32 windowSurfaceResX = 0;
static U32 windowSurfaceResY = 0;
static bool windowFullscreenEnabled = true; // DC is always fullscreen on TV.

bool InitWindow(const char *title) {
    (void)title;
    windowSystemInitialized = true;
    return true;
}

void EndWindow() {
    if (windowSystemInitialized) {
        DestroyWindowSurface();
        windowSystemInitialized = false;
    }
}

bool IsWindowInitialized() {
    return windowSystemInitialized;
}

bool CreateWindowSurface(U32 resX, U32 resY) {
    windowSurfaceResX = resX;
    windowSurfaceResY = resY;
    return true; // PVR2 init lives in LIB386/SVGA/KOS.CPP::CreateVideoSurface
}

void DestroyWindowSurface() {
    windowSurfaceResX = 0;
    windowSurfaceResY = 0;
}

// PresentRendererFrame / LockRendererTexture / UnlockAndPresentRendererTexture
// are implemented in LIB386/SVGA/KOS.CPP, which owns the PVR2 paletted texture.

void WindowToSurfaceCoords(S32 windowX, S32 windowY, S32 *surfaceX, S32 *surfaceY) {
    // 1:1 mapping on DC; no resizable window.
    if (surfaceX) *surfaceX = windowX;
    if (surfaceY) *surfaceY = windowY;
}

void SetWindowFullscreen(bool fullscreen) {
    windowFullscreenEnabled = fullscreen; // No-op on DC; always fullscreen.
}

bool GetWindowFullscreen() {
    return windowFullscreenEnabled;
}

void ManageWindow() {
    // Empty
}

void HandleEventsWindow(const void *event) {
    (void)event;
}

// =============================================================================
// EVENTS.CPP replacement
// =============================================================================
static EventFilterFn s_eventFilter = NULL;

bool InitEvents() {
    return true;
}

void EndEvents() {
    s_eventFilter = NULL;
}

void ManageEvents() {
    // KOS event pumping is implicit (Maple driver, scheduler).
    // Nothing to do here per-frame for V1.
}

void SetEventFilter(EventFilterFn fn) {
    s_eventFilter = fn;
}

// =============================================================================
// KEYBOARD.CPP replacement
// =============================================================================
S32 AsciiMode;
S32 Key;
U8 TabKeys[TABKEYS_NUM_KEYS];

void InitKeyboard() {
    memset(TabKeys, 0, sizeof(TabKeys));
    AsciiMode = 0;
    Key = 0;
}

void EndKeyboard() {
    // Empty
}

S32 GetAscii() {
    // V1: no keyboard input. V2 will read from KOS Maple keyboard.
    return 0;
}

S32 CheckKey(U32 key) {
    // Mirrors KEYBOARD.CPP (SDL backend): physical scancodes occupy TabKeys[0..255],
    // virtual gamepad scancodes (>= 256) are bit-packed at TabKeys[256-32 + (key>>3)],
    // bit (key & 7). Without this fan-out the gamepad bindings consumed by
    // ApplyGamepadBindings() (Start, Y, X, triggers, RStick, DPad spells) silently
    // read 0 — only the K_ENTER/K_ESC/arrow physical-key mirror in ManageKeyboard
    // would still work.
    if (key < 256) {
        return TabKeys[key];
    }
    U32 idx = 256 - 32 + (key >> 3);
    if (idx >= TABKEYS_NUM_KEYS) return 0;
    return TabKeys[idx] & (1 << (key & 7));
}

void GetKeyScancodeName(char *string, U8 strMaxLen, U32 scanCode) {
    if (!string || strMaxLen == 0) return;
    snprintf(string, strMaxLen, "K%lu", (unsigned long)scanCode);
}

void ManageKeyboard() {
    // Clear physical scancodes (TabKeys[0..255]); the gamepad virtual region
    // (TabKeys[256..]) is owned by JOYSTICK_DC_STUB::UpdateJoystick.
    memset(TabKeys, 0, 256);

    // Physical-scancode safety mirror for default LBA2.CFG ergonomics. Now that
    // the gamepad bit-packed CheckKey works (so ApplyGamepadBindings consumes
    // the K_GAMEPAD_* bindings directly), B→ESC and Start→ESC have been removed.
    // V2.8: A→RETURN+SPACE mirror also removed — A is now exclusively I_CAMERA
    // (engine slot 22) per the V2.8 remap. Confirm/dialog-advance on intro and
    // save screens routes through JoystickMenuAction which maps A|START→K_ENTER
    // independently of TabKeys, so removing this mirror doesn't break menus.
    // Arrow mirror stays for the legacy keyboard-arrow code paths (DPad LEFT/
    // RIGHT used as Twinsen rotation; UP/DOWN unused but mirrored for free).
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (cont) {
        cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
        if (st) {
            const uint32_t b = st->buttons;

            if (b & CONT_DPAD_UP)    TabKeys[82] = 1; // UP
            if (b & CONT_DPAD_DOWN)  TabKeys[81] = 1; // DOWN
            if (b & CONT_DPAD_LEFT)  TabKeys[80] = 1; // LEFT
            if (b & CONT_DPAD_RIGHT) TabKeys[79] = 1; // RIGHT
        }
    }
}

void HandleEventsKeyboard(const void *event) {
    (void)event;
}

// =============================================================================
// MOUSE.CPP replacement (stubbed; DC has no mouse in V1)
// =============================================================================
bool FlagMouse = false;
volatile S32 Click = 0;
volatile U32 MouseX = 0;
volatile U32 MouseY = 0;
S32 MouseXDep = 0;
S32 MouseYDep = 0;
volatile S32 MouseWheelYAccum = 0;
S32 MouseSpriteGraphicNum = 0;
const void *MouseSpriteGraphic = NULL;

bool MouseSpriteShouldDraw(void) { return false; }
bool MouseConsumeMotionFlag(void) { return false; }

void InitMouse() {}
void EndMouse() {}
void ShowMouse(bool show) { (void)show; }
void ShowMouseAfterFirstMotion(void) {}
void SetMousePos(U32 x, U32 y) { (void)x; (void)y; }
void SetMouseBox(U32 x0, U32 y0, U32 x1, U32 y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void ClearMouseBox() {}
void ManageMouse() {}
void HandleEventsMouse(const void *event) { (void)event; }

S32 ConsumeMouseWheelY(void) {
    S32 v = MouseWheelYAccum;
    MouseWheelYAccum = 0;
    return v;
}

// =============================================================================
// STRING.CPP replacement
// =============================================================================
char *ade_strcasestr(const char *haystack, const char *needle) {
    size_t length = strlen(needle);
    while (*haystack) {
        if (strncasecmp(haystack, needle, length) == 0) {
            return (char *)haystack;
        }
        ++haystack;
    }
    return NULL;
}

// CP850 conversion uses the same mapping table as the SDL build.
// Lifted verbatim from STRING.CPP since no SDL dependency is involved.
U8 UnicodeToCp850(U32 codePoint) {
    if (codePoint < 0x80) return (U8)codePoint;

    switch (codePoint) {
    case 0x00A0: return 0xFF;
    case 0x00A1: return 0xAD;
    case 0x00A2: return 0xBD;
    case 0x00A3: return 0x9C;
    case 0x00A4: return 0xCF;
    case 0x00A5: return 0xBE;
    case 0x00A6: return 0xDD;
    case 0x00A7: return 0xF5;
    case 0x00A8: return 0xF9;
    case 0x00A9: return 0xB8;
    case 0x00AA: return 0xA6;
    case 0x00AB: return 0xAE;
    case 0x00AC: return 0xAA;
    case 0x00AD: return 0xF0;
    case 0x00AE: return 0xA9;
    case 0x00AF: return 0xEE;
    case 0x00B0: return 0xF8;
    case 0x00B1: return 0xF1;
    case 0x00B2: return 0xFD;
    case 0x00B3: return 0xFC;
    case 0x00B4: return 0xEF;
    case 0x00B5: return 0xE6;
    case 0x00B6: return 0xF4;
    case 0x00B7: return 0xFA;
    case 0x00B8: return 0xF7;
    case 0x00B9: return 0xFB;
    case 0x00BA: return 0xA7;
    case 0x00BB: return 0xAF;
    case 0x00BC: return 0xAC;
    case 0x00BD: return 0xAB;
    case 0x00BE: return 0xF3;
    case 0x00BF: return 0xA8;
    case 0x00C0: return 0xB7;
    case 0x00C1: return 0xB5;
    case 0x00C2: return 0xB6;
    case 0x00C3: return 0xC7;
    case 0x00C4: return 0x8E;
    case 0x00C5: return 0x8F;
    case 0x00C6: return 0x92;
    case 0x00C7: return 0x80;
    case 0x00C8: return 0xD4;
    case 0x00C9: return 0x90;
    case 0x00CA: return 0xD2;
    case 0x00CB: return 0xD3;
    case 0x00CC: return 0xDE;
    case 0x00CD: return 0xD6;
    case 0x00CE: return 0xD7;
    case 0x00CF: return 0xD8;
    case 0x00D0: return 0xD1;
    case 0x00D1: return 0xA5;
    case 0x00D2: return 0xE3;
    case 0x00D3: return 0xE0;
    case 0x00D4: return 0xE2;
    case 0x00D5: return 0xE5;
    case 0x00D6: return 0x99;
    case 0x00D7: return 0x9E;
    case 0x00D8: return 0x9D;
    case 0x00D9: return 0xEB;
    case 0x00DA: return 0xE9;
    case 0x00DB: return 0xEA;
    case 0x00DC: return 0x9A;
    case 0x00DD: return 0xED;
    case 0x00DE: return 0xE8;
    case 0x00DF: return 0xE1;
    case 0x00E0: return 0x85;
    case 0x00E1: return 0xA0;
    case 0x00E2: return 0x83;
    case 0x00E3: return 0xC6;
    case 0x00E4: return 0x84;
    case 0x00E5: return 0x86;
    case 0x00E6: return 0x91;
    case 0x00E7: return 0x87;
    case 0x00E8: return 0x8A;
    case 0x00E9: return 0x82;
    case 0x00EA: return 0x88;
    case 0x00EB: return 0x89;
    case 0x00EC: return 0x8D;
    case 0x00ED: return 0xA1;
    case 0x00EE: return 0x8C;
    case 0x00EF: return 0x8B;
    case 0x00F0: return 0xD0;
    case 0x00F1: return 0xA4;
    case 0x00F2: return 0x95;
    case 0x00F3: return 0xA2;
    case 0x00F4: return 0x93;
    case 0x00F5: return 0xE4;
    case 0x00F6: return 0x94;
    case 0x00F7: return 0xF6;
    case 0x00F8: return 0x9B;
    case 0x00F9: return 0x97;
    case 0x00FA: return 0xA3;
    case 0x00FB: return 0x96;
    case 0x00FC: return 0x81;
    case 0x00FD: return 0xEC;
    case 0x00FE: return 0xE7;
    case 0x00FF: return 0x98;
    default: return '?';
    }
}

void CopyUtf8ToCp850(char *dst, size_t dstSize, const char *src) {
    if (!dst || dstSize == 0 || !src) return;

    size_t di = 0;
    while (*src && di + 1 < dstSize) {
        U32 cp;
        U8 c = (U8)*src;
        if (c < 0x80) {
            cp = c;
            ++src;
        } else if ((c & 0xE0) == 0xC0) {
            cp = (c & 0x1F) << 6;
            ++src;
            if ((*src & 0xC0) == 0x80) {
                cp |= (*src & 0x3F);
                ++src;
            } else {
                cp = '?';
            }
        } else if ((c & 0xF0) == 0xE0) {
            cp = (c & 0x0F) << 12;
            ++src;
            if ((*src & 0xC0) == 0x80) {
                cp |= (*src & 0x3F) << 6;
                ++src;
                if ((*src & 0xC0) == 0x80) {
                    cp |= (*src & 0x3F);
                    ++src;
                } else {
                    cp = '?';
                }
            } else {
                cp = '?';
            }
        } else {
            cp = '?';
            ++src;
        }
        dst[di++] = (char)UnicodeToCp850(cp);
    }
    dst[di] = '\0';
}

void FormatUtf8ToCp850(char *dst, size_t dstSize, const char *fmt, ...) {
    if (!dst || dstSize == 0) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    CopyUtf8ToCp850(dst, dstSize, buf);
}

// =============================================================================
#ifdef __cplusplus
}
#endif
