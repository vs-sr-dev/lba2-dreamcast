/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2017 Greg Kennedy

	See smacker.h for more information.

	smk_bitstream.h
		SMK bitstream structure. Presents a block of raw bytes one
		bit at a time, and protects against over-read.
*/

#ifndef SMK_BITSTREAM_H
#define SMK_BITSTREAM_H

#ifdef LBA2_TARGET_DREAMCAST
/* V2.7.6: expose the bitstream layout so hufftree.c can inline the bit-read
 * in its hot lookup loop. The original libsmacker keeps this private and goes
 * through a function call per bit (_smk_bs_read_1) — on SH4 with no inlining
 * across translation units that costs ~30 cycles per bit on top of a similar
 * recursive call into _smk_huff8_lookup. Inlining directly into a loop turns
 * the per-bit cost from ~50-70 cycles to ~5-10 cycles, the dominant win for
 * frame decode budget. */
struct smk_bit_t
{
	const unsigned char* buffer;
	unsigned long size;
	unsigned long byte_num;
	char bit_num;
};
#else
/** Bitstream structure, Forward declaration */
struct smk_bit_t;
#endif

/* BITSTREAM Functions */
/** Initialize a bitstream */
struct smk_bit_t* smk_bs_init(const unsigned char* b, unsigned long size);

/** This macro checks return code from _smk_bs_read_1 and
	jumps to error label if problems occur. */
#define smk_bs_read_1(t,uc) \
{ \
	if ((char)(uc = _smk_bs_read_1(t)) < 0) \
	{ \
		fprintf(stderr, "libsmacker::smk_bs_read_1(" #t ", " #uc ") - ERROR (file: %s, line: %lu)\n", __FILE__, (unsigned long)__LINE__); \
		goto error; \
	} \
}
/** Read a single bit from the bitstream, and advance.
	Returns -1 on error. */
char _smk_bs_read_1(struct smk_bit_t* bs);

/** This macro checks return code from _smk_bs_read_8 and
	jumps to error label if problems occur. */
#define smk_bs_read_8(t,s) \
{ \
	if ((short)(s = _smk_bs_read_8(t)) < 0) \
	{ \
		fprintf(stderr, "libsmacker::smk_bs_read_8(" #t ", " #s ") - ERROR (file: %s, line: %lu)\n", __FILE__, (unsigned long)__LINE__); \
		goto error; \
	} \
}
/** Read eight bits from the bitstream (one byte), and advance.
	Returns -1 on error. */
short _smk_bs_read_8(struct smk_bit_t* bs);

#endif
