// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// V1 stub of the debug console for Dreamcast.
// The full console (CONSOLE.CPP + CONSOLE_STATE.CPP) drags in SDL3 for input
// and screenshot saving — we don't need it for V1 milestone "Twinsen visible".
// All public Console_* symbols are provided as no-ops here so the engine links.

#include "CONSOLE.H"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  Console_IsOpen(void) { return 0; }
void Console_Toggle(void) {}
void Console_Close(void) {}
void Console_Update(void) {}
void Console_Print(const char *fmt, ...) { (void)fmt; }
void Console_PrePresent(U32 *frameBuffer, U32 pitch, U32 width, U32 height, const U8 *palette_rgb) {
    (void)frameBuffer; (void)pitch; (void)width; (void)height; (void)palette_rgb;
}
int  Console_FeedEvent(const void *sdlEvent) { (void)sdlEvent; return 0; }
void Console_SetToggleScancode(int scancode) { (void)scancode; }
int  Console_GetToggleScancode(void) { return 0; }
int  Console_ShouldSuppressInputThisFrame(void) { return 0; }
void Console_SetSlideShowActive(int active) { (void)active; }

void Console_RequestScreenshot(const char *path, int use_png) { (void)path; (void)use_png; }
int  Console_HasPendingScreenshot(char *path_out, unsigned int path_max, int *out_is_png) {
    (void)path_out; (void)path_max; (void)out_is_png; return 0;
}
void Console_ClearPendingScreenshot(void) {}

void Console_RegisterCommand(const char *name, Console_CmdFn callback, const char *help) {
    (void)name; (void)callback; (void)help;
}
void Console_RegisterCommandEx(const char *name, Console_CmdFn callback, const char *help, const char *usage, const char *context) {
    (void)name; (void)callback; (void)help; (void)usage; (void)context;
}
void Console_RegisterCvar(const char *name, void *ptr, Console_CvarType type, const char *help) {
    (void)name; (void)ptr; (void)type; (void)help;
}
// Console_RegisterAll lives in CONSOLE_CMD.CPP (always built); no stub needed here.

void Console_Execute(const char *line) { (void)line; }
void Console_SetLineSinkForTests(Console_LineSinkFn sink) { (void)sink; }

#ifdef __cplusplus
}
#endif
