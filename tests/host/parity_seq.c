/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* parity_seq -- om/seq.om + kmod/agpu_exec.h (what the module runs for a GSP-RM "run CPU sequencer"
 * request) against the oracle aien-dev/aienux c9678d7b native/kernel/gpu/gsp_seq.c.
 * Valid requests: identical register traces, saves and verdicts. Malformed requests: the same refusal
 * class, and the Omega path performs no register access at all (it checks the whole buffer first; the
 * oracle may have run a prefix). Offsets outside BAR0: refused by Omega only (a deliberate addition).
 * Prints PARITY-SEQ PASS valid=N malformed=M or the first difference. Host only. */
#include <stdio.h>
#include <string.h>
#include "omhost.h"
#include "gsp_seq.h"
#include "../../kmod/agpu_exec.h"

#define BAR 0x04000000u
#define MAXW 4096u

struct trace {
	uint32_t n, off[8192], val[8192];
	uint8_t wr[8192];
	uint32_t reads;
};
static struct trace tc, to;

static uint32_t sim_value(uint32_t off, uint32_t k) { return (off * 2654435761u) ^ (k * 40503u); }

static uint32_t rd(void *ctx, uint32_t off)
{
	struct trace *t = ctx;
	uint32_t v = sim_value(off, t->reads++);
	if (t->n < 8192) { t->off[t->n] = off; t->val[t->n] = v; t->wr[t->n] = 0; }
	t->n++;
	return v;
}

static void wr(void *ctx, uint32_t off, uint32_t v)
{
	struct trace *t = ctx;
	if (t->n < 8192) { t->off[t->n] = off; t->val[t->n] = v; t->wr[t->n] = 1; }
	t->n++;
}

static int core_ok(void *ctx, uint32_t op) { (void)ctx; (void)op; return -1; }

static uint32_t rng = 12345u;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static uint8_t msg[65536];
static uint64_t ops[12288];
static uint32_t cmd[MAXW];

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* The received message: element header 48, RPC header 32 (length at 56), the request at 80. */
static void make_msg(uint32_t nwords, uint32_t cmdi, uint32_t buf_words)
{
	memset(msg, 0, sizeof(msg));
	put32(msg + 56, 32 + 40 + 4 * buf_words);
	put32(msg + 80, nwords);
	put32(msg + 84, cmdi);
	for (uint32_t i = 0; i < buf_words; i++)
		put32(msg + 120 + 4 * i, cmd[i]);
}

/* A random valid command list of total length w words. Register offsets aligned inside BAR0. */
static uint32_t gen_valid(uint32_t maxw, int with_core, int may_hang)
{
	uint32_t w = 0;
	while (w < maxw) {
		uint32_t op = rnd() % (with_core ? 9 : 5);
		static const uint32_t pay[9] = { 2, 3, 5, 1, 2, 0, 0, 0, 0 };
		if (w + 1 + pay[op] > maxw)
			break;
		cmd[w++] = op;
		for (uint32_t j = 0; j < pay[op]; j++)
			cmd[w + j] = rnd();
		if (op <= 2 || op == 4)
			cmd[w] = (rnd() % (BAR / 4)) * 4;
		if (op == 4)
			cmd[w + 1] = rnd() % 8;
		if (op == 2) {			/* poll: match within 0..3 reads, or never */
			cmd[w + 1] = (may_hang && rnd() % 3 == 0) ? 0xffffffffu : 0u;
			cmd[w + 2] = cmd[w + 1] ? rnd() : 0;
		}
		if (op == 3)
			cmd[w] = rnd() % 50;
		w += pay[op];
	}
	return w;
}

static int run_both(uint32_t nwords, uint32_t cmdi, uint32_t budget, uint64_t *om_result, int *c_rc,
		    const char **why)
{
	gsp_seq_io io = { rd, wr, core_ok, 0, &tc, budget, { 0 } };
	struct agpu_regio rio = { rd, wr, 0, &to, budget, { 0 } };
	uint64_t st;
	uint32_t done;

	memset(&tc, 0, sizeof(tc));
	memset(&to, 0, sizeof(to));
	*c_rc = gsp_seq_run(cmd, nwords, cmdi, &io, why);
	*om_result = om_call(agpu_om_seq, P(msg), P(ops), BAR, 0, 0, 0, &st);
	if (st) {
		printf("PARITY-SEQ FAIL omega trap %llu\n", (unsigned long long)st);
		return -1;
	}
	if (*om_result < 1000000) {
		int r = agpu_seq_exec(ops, (uint32_t)*om_result, &rio, &done);
		if ((r == 0) != (*c_rc == 0)) {
			printf("PARITY-SEQ FAIL verdict C %d (%s) Omega exec %d\n", *c_rc, *why ? *why : "-", r);
			return -1;
		}
		if (memcmp(io.save, rio.save, sizeof(io.save))) {
			printf("PARITY-SEQ FAIL save area differs\n");
			return -1;
		}
	}
	return 0;
}

static int same_trace(void)
{
	if (tc.n != to.n)
		return 0;
	for (uint32_t i = 0; i < tc.n && i < 8192; i++)
		if (tc.off[i] != to.off[i] || tc.val[i] != to.val[i] || tc.wr[i] != to.wr[i])
			return 0;
	return 1;
}

static uint64_t class_of(const char *why)
{
	if (!why) return 0;
	if (!strcmp(why, "empty buffer")) return 1000002;
	if (!strcmp(why, "cmdIndex not inside the buffer")) return 1000004;
	if (!strcmp(why, "unknown opcode")) return 1000005;
	if (!strcmp(why, "opcode overruns cmdIndex")) return 1000006;
	if (!strcmp(why, "save index out of range")) return 1000007;
	return 1;
}

int main(void)
{
	unsigned valid = 0, bad = 0;
	uint64_t r;
	int c;
	const char *why;

	for (unsigned t = 0; t < 3000; t++) {
		uint32_t w = gen_valid(1 + rnd() % 200, t % 3 == 0, 1);
		uint32_t nwords = w + 1 + rnd() % 4;	/* cmdIndex < bufferSizeDWord */
		make_msg(nwords, w, nwords);
		if (run_both(nwords, w, rnd() % 4, &r, &c, &why))
			return 1;
		if (r >= 1000000 || !same_trace()) {
			printf("PARITY-SEQ FAIL valid case %u: omega %llu, traces %u/%u\n", t, (unsigned long long)r, tc.n, to.n);
			return 1;
		}
		valid++;
	}
	/* malformed: each oracle refusal class, built on top of a valid prefix */
	for (unsigned t = 0; t < 2000; t++) {
		uint32_t w = gen_valid(1 + rnd() % 60, 0, 0), nwords, cmdi;
		unsigned kind = t % 5;
		nwords = w + 4;
		cmdi = w;
		switch (kind) {
		case 0: nwords = 0; break;			/* empty buffer */
		case 1: cmdi = nwords; break;			/* cmdIndex not inside */
		case 2: cmd[w] = 9 + rnd() % 1000; cmdi = w + 1; break;	/* unknown opcode */
		case 3: cmd[w] = 2; cmdi = w + 1 + rnd() % 5; break;	/* poll overruns */
		case 4: cmd[w] = 4; cmd[w + 1] = 0; cmd[w + 2] = 8 + rnd() % 100; cmdi = w + 3; break; /* save index */
		}
		make_msg(nwords, cmdi, nwords > MAXW ? MAXW : (nwords ? nwords : 1));
		if (run_both(nwords, cmdi, 2, &r, &c, &why))
			return 1;
		if (c == 0 || r != class_of(why) || to.n != 0) {
			printf("PARITY-SEQ FAIL malformed kind %u case %u: oracle %d (%s), omega %llu, omega accesses %u\n",
			       kind, t, c, why ? why : "-", (unsigned long long)r, to.n);
			return 1;
		}
		bad++;
	}
	/* register offsets: unaligned or past BAR0 are refused by Omega (code 8) before any access */
	cmd[0] = 0; cmd[1] = BAR; cmd[2] = 1;
	make_msg(4, 3, 4);
	r = om_call(agpu_om_seq, P(msg), P(ops), BAR, 0, 0, 0, &(uint64_t){0});
	cmd[1] = 6;
	make_msg(4, 3, 4);
	uint64_t r2 = om_call(agpu_om_seq, P(msg), P(ops), BAR, 0, 0, 0, &(uint64_t){0});
	cmd[1] = BAR - 4;
	make_msg(4, 3, 4);
	uint64_t r3 = om_call(agpu_om_seq, P(msg), P(ops), BAR, 0, 0, 0, &(uint64_t){0});
	if (r != 1000008 || r2 != 1000008 || r3 != 1) {
		printf("PARITY-SEQ FAIL BAR0 bounds: %llu %llu %llu\n", (unsigned long long)r, (unsigned long long)r2,
		       (unsigned long long)r3);
		return 1;
	}
	/* message shorter than its own buffer */
	make_msg(4, 3, 4);
	put32(msg + 56, 32 + 40 + 4 * 3);
	r = om_call(agpu_om_seq, P(msg), P(ops), BAR, 0, 0, 0, &(uint64_t){0});
	if (r != 1000003) {
		printf("PARITY-SEQ FAIL short message: %llu\n", (unsigned long long)r);
		return 1;
	}
	printf("PARITY-SEQ PASS valid=%u malformed=%u\n", valid, bad);
	return 0;
}
