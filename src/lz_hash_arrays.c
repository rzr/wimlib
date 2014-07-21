/*
 * lz_hash_arrays.c
 *
 * Hash array match-finder for Lempel-Ziv compression.
 *
 * Copyright (c) 2014 Eric Biggers.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib/lz_mf.h"
#include "wimlib/util.h"
#include <pthread.h>
#include <string.h>

/* Number of hash buckets.  This can be changed, but should be a power of 2 so
 * that the correct hash bucket can be selected using a fast bitwise AND.  */
#define LZ_HA_HASH_LEN     (1 << 15)

/* Number of bytes from which the hash code is computed at each position.  This
 * can be changed, provided that lz_ha_hash() is updated as well.  */
#define LZ_HA_HASH_BYTES   3

/* TODO */
#define LZ_HA_SLOT_BITS		4
#define LZ_HA_SLOTS_PER_BUCKET	(1 << LZ_HA_SLOT_BITS)
#define LZ_HA_SLOT_MASK	(LZ_HA_SLOTS_PER_BUCKET - 1)

#define LZ_HA_POS_BITS		(32 - LZ_HA_SLOT_BITS)
#define LZ_HA_POS_MASK		(((u32)1 << LZ_HA_POS_BITS) - 1)

struct lz_ha {
	struct lz_mf base;
	u32 *arrays;
	u32 next_hash;
};

static u32 crc32_table[256];
static pthread_once_t crc32_table_filled = PTHREAD_ONCE_INIT;

static void
crc32_init(void)
{
        for (u32 b = 0; b < 256; b++) {
                u32 r = b;
                for (int i = 0; i < 8; i++) {
                        if (r & 1)
                                r = (r >> 1) ^ 0xEDB88320;
                        else
                                r >>= 1;
                }
                crc32_table[b] = r;
        }
}

/* This hash function is taken from the LZMA SDK.  It seems to work well.
 *
 * TODO: Maybe use the SSE4.2 CRC32 instruction when available?  */
static inline u32
lz_ha_hash(const u8 *p)
{
	u32 hash = 0;

	hash ^= crc32_table[p[0]];
	hash ^= p[1];
	hash ^= (u32)p[2] << 8;

	return hash % LZ_HA_HASH_LEN;
}

static void
lz_ha_set_default_params(struct lz_mf_params *params)
{
	if (params->min_match_len < LZ_HA_HASH_BYTES)
		params->min_match_len = LZ_HA_HASH_BYTES;

	if (params->max_match_len == 0)
		params->max_match_len = params->max_window_size;

	if (params->nice_match_len == 0)
		params->nice_match_len = 24;

	if (params->nice_match_len < params->min_match_len)
		params->nice_match_len = params->min_match_len;

	if (params->nice_match_len > params->max_match_len)
		params->nice_match_len = params->max_match_len;
}

static bool
lz_ha_params_valid(const struct lz_mf_params *_params)
{
	struct lz_mf_params params = *_params;

	lz_ha_set_default_params(&params);

	/* Avoid edge case where min_match_len = 3, max_match_len = 2 */
	return (params.min_match_len <= params.max_match_len);
}

static u64
lz_ha_get_needed_memory(u32 max_window_size)
{
	return LZ_HA_HASH_LEN * LZ_HA_SLOTS_PER_BUCKET * sizeof(u32);
}

static bool
lz_ha_init(struct lz_mf *_mf)
{
	struct lz_ha *mf = (struct lz_ha *)_mf;

	lz_ha_set_default_params(&mf->base.params);

	mf->arrays = ALIGNED_MALLOC(lz_ha_get_needed_memory(mf->base.params.max_window_size),
				    64);
	if (!mf->arrays)
		return false;

	pthread_once(&crc32_table_filled, crc32_init);

	return true;
}

static void
lz_ha_load_window(struct lz_mf *_mf, const u8 window[], u32 size)
{
	struct lz_ha *mf = (struct lz_ha *)_mf;

	for (u32 i = 0; i < LZ_HA_HASH_LEN; i++)
		mf->arrays[i << LZ_HA_SLOT_BITS] = 0;

	if (size >= LZ_HA_HASH_BYTES)
		mf->next_hash = lz_ha_hash(window);
}

static u32
lz_ha_get_matches(struct lz_mf *_mf, struct lz_match matches[])
{
	struct lz_ha *mf = (struct lz_ha *)_mf;
	const u32 bytes_remaining = lz_mf_get_bytes_remaining(&mf->base);
	const u8 * const strptr = lz_mf_get_window_ptr(&mf->base);
	const u32 max_len = min(bytes_remaining, mf->base.params.nice_match_len);
	u32 hash;
	u32 *array;
	u32 start_i;
	u32 next_i;
	u32 i;
	u32 best_len;
	u32 cur_match;
	u32 num_matches = 0;

	if (bytes_remaining <= LZ_HA_HASH_BYTES)
		goto out;

	hash = mf->next_hash;
	mf->next_hash = lz_ha_hash(strptr + 1);
	prefetch(&mf->arrays[mf->next_hash << LZ_HA_SLOT_BITS]);
	array = &mf->arrays[hash << LZ_HA_SLOT_BITS];

	start_i = array[0] >> LZ_HA_POS_BITS;
	LZ_ASSERT(start_i < LZ_HA_SLOTS_PER_BUCKET);

	best_len = LZ_HA_HASH_BYTES - 1;

	for (i = start_i;
	     (cur_match = (array[i] & LZ_HA_POS_MASK)) != 0;
	     i = (i - 1) & LZ_HA_SLOT_MASK)
	{
		u32 len;
		const u8 *matchptr;

		LZ_ASSERT(cur_match < mf->base.cur_window_pos);

		matchptr = &mf->base.cur_window[cur_match];

		if (matchptr[best_len] != strptr[best_len] ||
		    matchptr[best_len - 1] != strptr[best_len - 1] ||
		    matchptr[0] != strptr[0])
			goto next_match;

		for (len = 1; len < best_len - 1; len++)
			if (matchptr[len] != strptr[len])
				goto next_match;

		len = best_len;

		while (++len != max_len)
			if (matchptr[len] != strptr[len])
				break;

		matches[num_matches++] = (struct lz_match) {
			.len = len,
			.offset = strptr - matchptr,
		};
		best_len = len;
		if (best_len == max_len) {
			const u32 len_limit = min(bytes_remaining,
						  mf->base.params.max_match_len);
			while (len < len_limit && strptr[len] == matchptr[len])
				len++;
			matches[num_matches - 1].len = len;
			break;
		}
	next_match:
		;
	}
	
	next_i = (start_i + 1) & LZ_HA_SLOT_MASK;
	array[0] += (u32)1 << LZ_HA_POS_BITS;
	LZ_ASSERT((array[0] >> LZ_HA_POS_BITS) == next_i);
	i = next_i;
	array[i] = (next_i << LZ_HA_POS_BITS) | mf->base.cur_window_pos;
	i = (i + 1) & LZ_HA_SLOT_MASK;
	array[i] = (next_i << LZ_HA_POS_BITS);
out:
	mf->base.cur_window_pos++;
	return num_matches;
}

static void
lz_ha_skip_position(struct lz_ha *mf)
{
	const u32 bytes_remaining = lz_mf_get_bytes_remaining(&mf->base);
	u32 hash;
	u32 *array;
	u32 start_i;
	u32 next_i;
	u32 i;

	if (bytes_remaining <= LZ_HA_HASH_BYTES)
		goto out;

	hash = mf->next_hash;
	mf->next_hash = lz_ha_hash(lz_mf_get_window_ptr(&mf->base) + 1);
	prefetch(&mf->arrays[mf->next_hash << LZ_HA_SLOT_BITS]);
	array = &mf->arrays[hash << LZ_HA_SLOT_BITS];
	start_i = array[0] >> LZ_HA_POS_BITS;

	next_i = (start_i + 1) & LZ_HA_SLOT_MASK;
	array[0] += (u32)1 << LZ_HA_POS_BITS;
	LZ_ASSERT((array[0] >> LZ_HA_POS_BITS) == next_i);
	i = next_i;
	array[i] = (next_i << LZ_HA_POS_BITS) | mf->base.cur_window_pos;
	i = (i + 1) & LZ_HA_SLOT_MASK;
	array[i] = (next_i << LZ_HA_POS_BITS);
out:
	mf->base.cur_window_pos++;
}

static void
lz_ha_skip_positions(struct lz_mf *_mf, u32 n)
{
	struct lz_ha *mf = (struct lz_ha *)_mf;

	do {
		lz_ha_skip_position(mf);
	} while (--n);
}

static void
lz_ha_destroy(struct lz_mf *_mf)
{
	struct lz_ha *mf = (struct lz_ha *)_mf;

	ALIGNED_FREE(mf->arrays);
}

const struct lz_mf_ops lz_hash_arrays_ops = {
	.params_valid      = lz_ha_params_valid,
	.get_needed_memory = lz_ha_get_needed_memory,
	.init		   = lz_ha_init,
	.load_window       = lz_ha_load_window,
	.get_matches       = lz_ha_get_matches,
	.skip_positions    = lz_ha_skip_positions,
	.destroy           = lz_ha_destroy,
	.struct_size	   = sizeof(struct lz_ha),
};
