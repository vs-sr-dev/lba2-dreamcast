// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Lightweight shim that lets SOURCES files (PERSO, PLAYACF, RES_DISCOVERY,
// DIRECTORIES) keep their `SDL_*` call-site syntax on Dreamcast without
// dragging in SDL3 headers. Each shim maps to KOS / libc equivalents.
//
// On non-DC platforms this header is a no-op pass-through to <SDL3/SDL.h>.
#pragma once

#ifdef LBA2_TARGET_DREAMCAST

#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/LOGPRINT.H>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <kos.h>
#include <arch/timer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef U32 SDL_Keymod;
#define SDL_KMOD_NONE 0u
#define SDL_KMOD_ALT  0u

static inline int SDL_Init(unsigned int flags) { (void)flags; return 1; }
static inline void SDL_Quit(void) {}
static inline int SDL_InitSubSystem(unsigned int flags) { (void)flags; return 1; }
static inline void SDL_QuitSubSystem(unsigned int flags) { (void)flags; }
static inline const char *SDL_GetError(void) { return ""; }

static inline U32 SDL_GetTicks(void) { return (U32)timer_ms_gettime64(); }
static inline void SDL_Delay(U32 ms) { thd_sleep((int)ms); }

static inline SDL_Keymod SDL_GetModState(void) { return SDL_KMOD_NONE; }

static inline char *SDL_GetBasePath(void) {
    char *p = (char *)malloc(8);
    if (p) strcpy(p, "/cd/");
    return p;
}
static inline char *SDL_GetCurrentDirectory(void) {
    char *p = (char *)malloc(8);
    if (p) strcpy(p, "/cd/");
    return p;
}
static inline char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org; (void)app;
    char *p = (char *)malloc(8);
    if (p) strcpy(p, "/ram/");
    return p;
}

static inline size_t SDL_strlen(const char *s) { return strlen(s); }
static inline void   SDL_free(void *p)         { free(p); }
static inline char  *SDL_strdup(const char *s) { return strdup(s); }
static inline int    SDL_strncasecmp(const char *a, const char *b, size_t n) {
    return strncasecmp(a, b, n);
}
static inline char *SDL_strupr(char *s) {
    if (s) for (char *p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return s;
}
static inline char *SDL_strlwr(char *s) {
    if (s) for (char *p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return s;
}

// SDL_Log → LogPrintf (variadic forwarding)
#define SDL_Log LogPrintf

#ifdef __cplusplus
}
#endif

#else  // !LBA2_TARGET_DREAMCAST
#include <SDL3/SDL.h>
#endif
