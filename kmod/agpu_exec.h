/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/*
 * agpu_exec.h -- the only register executor in aien-gpu. Omega units decide; this performs.
 * Shared, unchanged, by the kernel module (BAR0 via readl/writel) and the host tests (a simulator), so
 * the host tests exercise the code the module runs.
 *
 * agpu_seq_exec performs an operation list that om/seq.om has already checked end to end (opcodes,
 * payload sizes, register offsets inside BAR0, save indexes). REG_POLL reads until (v & mask) == val
 * with the same budget rule as the aienux oracle gsp_seq.c: budget reads after the first, at least one.
 * Core operations (reset, start, wait, resume of the GSP falcon) are refused with AGPU_SEQ_E_CORE:
 * they restart the chip, NVIDIA's source cannot tell what GSP-RM expects of them on GB10B, so they
 * wait for chip evidence instead of a guess.
 */
#ifndef AGPU_EXEC_H
#define AGPU_EXEC_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 agpu_u32;
typedef u64 agpu_u64;
#else
#include <stdint.h>
typedef uint32_t agpu_u32;
typedef uint64_t agpu_u64;
#endif

struct agpu_regio {
	agpu_u32 (*rd)(void *ctx, agpu_u32 off);
	void (*wr)(void *ctx, agpu_u32 off, agpu_u32 val);
	void (*delay_us)(void *ctx, agpu_u32 us);
	void *ctx;
	agpu_u32 poll_budget;	/* reads allowed after the first one of a REG_POLL (0 counts as 1) */
	agpu_u32 save[8];	/* REG_STORE results, GSP_SEQ_BUF_REG_SAVE_SIZE */
};

enum {
	AGPU_SEQ_OK = 0,
	AGPU_SEQ_E_POLL = 1,	/* a REG_POLL never matched */
	AGPU_SEQ_E_CORE = 2,	/* a core operation was requested */
	AGPU_SEQ_E_OP = 3,	/* an opcode outside 0..8 (om/seq.om never passes one) */
};

/* ops: 6 words per operation (opcode, payload[5]) as om/seq.om writes them. *done = operations completed. */
static inline int agpu_seq_exec(const agpu_u64 *ops, agpu_u32 n, struct agpu_regio *io, agpu_u32 *done)
{
	agpu_u32 i;

	for (i = 0; i < n; i++) {
		const agpu_u64 *o = ops + 6 * i;
		agpu_u32 a = (agpu_u32)o[1], b = (agpu_u32)o[2], c = (agpu_u32)o[3];

		*done = i;
		switch (o[0]) {
		case 0:			/* REG_WRITE addr val */
			io->wr(io->ctx, a, b);
			break;
		case 1:			/* REG_MODIFY addr mask val: (old & ~mask) | val */
			io->wr(io->ctx, a, (io->rd(io->ctx, a) & ~b) | c);
			break;
		case 2: {		/* REG_POLL addr mask val timeout error */
			agpu_u32 left = io->poll_budget ? io->poll_budget : 1;
			agpu_u32 v = io->rd(io->ctx, a);

			while ((v & b) != c) {
				if (left == 0)
					return AGPU_SEQ_E_POLL;
				left--;
				if (io->delay_us)
					io->delay_us(io->ctx, 1);
				v = io->rd(io->ctx, a);
			}
			break;
		}
		case 3:			/* DELAY_US us */
			if (io->delay_us)
				io->delay_us(io->ctx, a);
			break;
		case 4:			/* REG_STORE addr index (index < 8, checked by om/seq.om) */
			io->save[b & 7] = io->rd(io->ctx, a);
			break;
		case 5: case 6: case 7: case 8:
			return AGPU_SEQ_E_CORE;
		default:
			return AGPU_SEQ_E_OP;
		}
	}
	*done = n;
	return AGPU_SEQ_OK;
}

#endif
