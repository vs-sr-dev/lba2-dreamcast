/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2017 Greg Kennedy

	See smacker.h for more information.

	smk_malloc.h
		"Safe" implementations of malloc and free.
		Verbose implementation of assert.

	V2.7.3 (Dreamcast port): added an opt-in frame arena. libsmacker rebuilds
	huffman trees per frame for both video and audio decode (smk_render_*),
	doing ~1200 per-node calloc/free pairs per frame on stereo 16-bit audio +
	video. On KOS newlib that costs ~7-10 ms per frame, visibly stuttering
	15 fps SMK as soon as content (e.g. voice speech) brings the trees up to
	full size. The arena diverts those allocations to a static bump pool that
	is reset once per smk_render call; smk_free is a no-op for arena pointers.
	Persistent allocations (smk_open_*, audio.buffer, video.frame, etc.) bypass
	the arena because the arena is only enabled inside smk_render.
*/

#ifndef SMK_MALLOC_H
#define SMK_MALLOC_H

/* calloc */
#include <stdlib.h>
/* fprintf */
#include <stdio.h>
/* memset */
#include <string.h>

/* Error messages from calloc */
#include <errno.h>

#ifdef LBA2_TARGET_DREAMCAST
/* 512 KB pool covers worst-case keyframes + full huff trees with margin.
 * Tree nodes are ~24 bytes each; 1200 nodes = 30 KB. Chunk buffer up to
 * ~64 KB. 512 KB leaves headroom for any frame size variance. */
#define SMK_ARENA_BYTES (512u * 1024u)
extern unsigned char  smk_arena_buf[SMK_ARENA_BYTES];
extern unsigned long  smk_arena_off;
extern int            smk_arena_active;

/* Reset before smk_render, disable after — only smk_render-internal mallocs
 * use the pool; persistent allocations (open/close paths) keep using calloc. */
static inline void smk_arena_reset(void)   { smk_arena_off = 0u; smk_arena_active = 1; }
static inline void smk_arena_disable(void) { smk_arena_active = 0; }

/* Returns 1 if p was allocated from the arena (so smk_free can no-op). */
static inline int smk_arena_owns(const void *p) {
    return ((const unsigned char *)p >= smk_arena_buf) &&
           ((const unsigned char *)p <  smk_arena_buf + SMK_ARENA_BYTES);
}
#endif

/**
	Verbose assert:
		branches to an error block if pointer is null
*/
#define smk_assert(p) \
{ \
	if (!p) \
	{ \
		fprintf(stderr, "libsmacker::smk_assert(" #p "): ERROR: NULL POINTER at line %lu, file %s\n", (unsigned long)__LINE__, __FILE__); \
		goto error; \
	} \
}

/**
	Safe free: attempts to prevent double-free by setting pointer to NULL.
		Optionally warns on attempts to free a NULL pointer.
	V2.7.3: arena pointers are no-ops; only heap-allocated pointers go to free().
*/
#ifdef LBA2_TARGET_DREAMCAST
#define smk_free(p) \
{ \
	if (p) \
	{ \
		if (!smk_arena_owns(p)) free(p); \
		p = NULL; \
	} \
}
#else
#define smk_free(p) \
{ \
	if (p) \
	{ \
		free(p); \
		p = NULL; \
	} \
}
#endif

/**
	Safe malloc: exits if calloc() returns NULL.
		Also initializes blocks to 0.
	Optionally warns on attempts to malloc over an existing pointer.
	V2.7.3 (DC): when arena is active, bump from the static pool; on overflow
		fall back to calloc. Pool memory is zeroed on first use of each region
		(memset of just the requested span — same behaviour as calloc).
*/
#ifdef LBA2_TARGET_DREAMCAST
#define smk_malloc(p, x) \
{ \
	if (smk_arena_active) \
	{ \
		unsigned long _smk_aligned = (smk_arena_off + 7u) & ~7u; \
		if (_smk_aligned + (unsigned long)(x) <= SMK_ARENA_BYTES) \
		{ \
			p = (void *)(smk_arena_buf + _smk_aligned); \
			smk_arena_off = _smk_aligned + (unsigned long)(x); \
			memset(p, 0, (size_t)(x)); \
		} \
		else \
		{ \
			p = calloc(1, x); \
			if (!p) \
			{ \
				fprintf(stderr, "libsmacker::smk_malloc(arena overflow + calloc fail): %lu bytes\n", (unsigned long)(x)); \
				exit(EXIT_FAILURE); \
			} \
		} \
	} \
	else \
	{ \
		p = calloc(1, x); \
		if (!p) \
		{ \
			fprintf(stderr, "libsmacker::smk_malloc(" #p ", %lu) - ERROR: calloc() returned NULL (file: %s, line: %lu)\n\tReason: [%d] %s\n", \
				(unsigned long) (x), __FILE__, (unsigned long)__LINE__, errno, strerror(errno)); \
			exit(EXIT_FAILURE); \
		} \
	} \
}
#else
#define smk_malloc(p, x) \
{ \
	p = calloc(1, x); \
	if (!p) \
	{ \
		fprintf(stderr, "libsmacker::smk_malloc(" #p ", %lu) - ERROR: calloc() returned NULL (file: %s, line: %lu)\n\tReason: [%d] %s\n", \
			(unsigned long) (x), __FILE__, (unsigned long)__LINE__, errno, strerror(errno)); \
		exit(EXIT_FAILURE); \
	} \
}
#endif

#endif
