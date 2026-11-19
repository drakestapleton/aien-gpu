/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* unit_test -- om/ident.om and om/abi.om against hand-written expectations from their sources, and the
 * trampoline contract (kmod/agpu_om.S): a trap returns its code and unwinds, callee-saved registers
 * x19..x28 keep the caller's values across a normal return and a trap, a call after a trap works.
 * Prints UNIT PASS checks=N or the first failure. Host only. */
#include <stdio.h>
#include <string.h>
#include "omhost.h"

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("UNIT FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t ident(uint64_t v, uint64_t d, uint64_t bar, uint64_t b0, uint64_t b42, uint64_t stage)
{
	uint64_t st, r = om_call(agpu_om_ident, v, d, bar, b0, b42, stage, &st);
	CHECK(st == 0);
	return r;
}

/* PMC_BOOT_42 with ARCHITECTURE 29:24 and IMPLEMENTATION 23:20 (nv_ref.h:153,170) */
static uint64_t boot42(uint32_t arch, uint32_t impl, uint32_t rev) { return (uint64_t)arch << 24 | impl << 20 | rev; }

static void test_ident(void)
{
	const uint64_t V = 0x10de, D = 0x2e12, BAR = 64ull << 20;
	CHECK(ident(V, D, BAR, 0, 0, 0) == 0);					/* stage 0: PCI facts only */
	CHECK(ident(0x1b36, D, BAR, 0, 0, 0) == 1);
	CHECK(ident(V, 0x2e13, BAR, 0, 0, 0) == 2);
	CHECK(ident(V, D, 4096, 0, 0, 0) == 3);
	CHECK(ident(V, D, BAR, 0, 0, 2) == 4);
	CHECK(ident(V, D, BAR, 0x1b0000a1, boot42(0x1a, 0xb, 0xa1), 1) == 6);	/* GB10B: another chip */
	CHECK(ident(V, D, BAR, 0, boot42(0x1b, 0xb, 0), 1) == 5);
	CHECK(ident(V, D, BAR, 0xffffffffu, boot42(0x1b, 0xb, 0), 1) == 5);
	CHECK(ident(V, D, BAR, 1, boot42(0x1b, 0xb, 0), 1) == 0);		/* GB20B */
	CHECK(ident(V, D, BAR, 0x1bb000a1u, 0x1bba1000u, 1) == 0);		/* Spark bare-metal GPUID.TXT */
	CHECK(ident(V, D, BAR, 1, boot42(0x1c, 0xb, 0), 1) == 6);		/* unknown arch > 0x1b */
	CHECK(ident(V, D, BAR, 1, boot42(0x19, 0xb, 0), 1) == 6);		/* unknown arch < 0x1a */
	CHECK(ident(V, D, BAR, 1, boot42(0x1a, 0x0, 0), 1) == 6);		/* GB100 */
	CHECK(ident(V, D, BAR, 1, boot42(0x1b, 0xc, 0), 1) == 7);		/* GB20C */
	CHECK(ident(V, D, BAR, 1, boot42(0x1b, 0x0, 0), 1) == 7);		/* GB200 */
	CHECK(ident(V, D, BAR, 1, 0xffffffffu, 1) == 6);
}

static uint8_t blk[128];
static void w32(unsigned o, uint32_t v) { for (int i = 0; i < 4; i++) blk[o + i] = (uint8_t)(v >> (8 * i)); }
static void w64(unsigned o, uint64_t v) { w32(o, (uint32_t)v); w32(o + 4, (uint32_t)(v >> 32)); }

static const uint64_t AIOVA = 0x80000000ull, ABYTES = 64ull << 20;
static void good_boot(void)
{
	memset(blk, 0, sizeof(blk));
	w32(0, 1);
	w64(8, 0x1000);		/* COT packet offset */
	w32(16, 868);
	w32(20, 0x5f3a1);
	w64(24, AIOVA + 0x2000);	/* FMC boot params */
	w64(32, 0x100000);		/* command queue */
	w64(40, 0x140000);		/* status queue */
	for (int i = 0; i < 4; i++)
		w32(48 + 4 * i, 1000000);
	w32(64, 10);
	w32(68, 30000);
	w32(72, 3);
}
static uint64_t abi(uint64_t req)
{
	uint64_t st, r = om_call(agpu_om_abi, req, P(blk), AIOVA, ABYTES, 0, 0, &st);
	CHECK(st == 0);
	return r;
}

static void test_abi(void)
{
	good_boot(); CHECK(abi(1) == 0);
	good_boot(); CHECK(abi(7) == 1);
	good_boot(); w32(0, 2); CHECK(abi(1) == 2);
	good_boot(); w32(16, 0); CHECK(abi(1) == 3);
	good_boot(); w32(16, 1025); CHECK(abi(1) == 3);
	good_boot(); w64(8, 0x1002); CHECK(abi(1) == 3);
	good_boot(); w64(8, ABYTES - 100); CHECK(abi(1) == 3);
	good_boot(); w64(8, ~0ull - 3); CHECK(abi(1) == 3);			/* wrap */
	good_boot(); w64(24, AIOVA - 8); CHECK(abi(1) == 4);
	good_boot(); w64(24, AIOVA + ABYTES - 80); CHECK(abi(1) == 4);
	good_boot(); w64(24, AIOVA + ABYTES - 88); CHECK(abi(1) == 0);
	good_boot(); w64(32, 0x100800); CHECK(abi(1) == 5);
	good_boot(); w64(40, 0x100000 + 0x20000); CHECK(abi(1) == 5);		/* overlap */
	good_boot(); w64(40, ABYTES - 0x1000); CHECK(abi(1) == 5);
	good_boot(); w64(32, 0x140000); w64(40, 0x100000); CHECK(abi(1) == 0);
	good_boot(); w32(52, 0); CHECK(abi(1) == 6);
	good_boot(); w32(60, 10000001); CHECK(abi(1) == 6);
	good_boot(); w32(64, 0); CHECK(abi(1) == 7);
	good_boot(); w32(64, 1001); CHECK(abi(1) == 7);
	good_boot(); w32(68, 60001); CHECK(abi(1) == 8);
	good_boot(); blk[127] = 1; CHECK(abi(1) == 9);
	good_boot(); blk[76] = 1; CHECK(abi(1) == 9);
	/* request 2, AGPU_ARENA_READ: off 0, len 8 */
	memset(blk, 0, sizeof(blk)); w64(8, 4096); CHECK(abi(2) == 0);
	memset(blk, 0, sizeof(blk)); w64(0, ABYTES - 4096); w64(8, 4096); CHECK(abi(2) == 0);
	memset(blk, 0, sizeof(blk)); w64(0, ABYTES - 4095); w64(8, 4096); CHECK(abi(2) == 10);
	memset(blk, 0, sizeof(blk)); CHECK(abi(2) == 10);					/* empty */
	memset(blk, 0, sizeof(blk)); w64(8, (16ull << 20) + 1); CHECK(abi(2) == 10);	/* > 16 MiB */
	memset(blk, 0, sizeof(blk)); w64(0, ~0ull - 10); w64(8, 4096); CHECK(abi(2) == 10);	/* wrap */
}

/* Calls agpu_om_enter with x19..x28 set to known values and reads them back after. */
static int callee_saved_kept(const void *entry, const uint64_t *a, uint64_t *status)
{
	uint64_t out[10];
	register const void *e __asm__("x0") = entry;
	register const uint64_t *ar __asm__("x1") = a;
	register uint64_t *s __asm__("x2") = status;
	__asm__ volatile(
		"mov x19, #19\n mov x20, #20\n mov x21, #21\n mov x22, #22\n mov x23, #23\n"
		"mov x24, #24\n mov x25, #25\n mov x26, #26\n mov x27, #27\n mov x28, #28\n"
		"bl agpu_om_enter\n"
		"stp x19, x20, [%3]\n stp x21, x22, [%3, #16]\n stp x23, x24, [%3, #32]\n"
		"stp x25, x26, [%3, #48]\n stp x27, x28, [%3, #64]\n"
		: "+r"(e), "+r"(ar), "+r"(s)
		: "r"(out)
		: "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		  "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x30", "memory", "cc");
	for (int i = 0; i < 10; i++)
		if (out[i] != (uint64_t)(19 + i))
			return 0;
	return 1;
}

static void test_trap(void)
{
	uint64_t st[32] = { 0 }, act[4] = { 0 }, status = 99;
	static uint8_t pkt[1024];
	/* A state the module never builds (pc 23 with a packet length past the 1024-byte view):
	 * pword indexes the packet out of bounds, so boot.om traps 3 (bounds). */
	st[0] = 23; st[1] = 2000; st[9] = 1024;
	uint64_t r = om_call(agpu_om_boot, P(st), P(pkt), 0, P(act), 0, 0, &status);
	CHECK(status == 3 && r == 0);
	/* the same call through the register check */
	const uint64_t a[6] = { P(st), P(pkt), 0, P(act), 0, 0 };
	st[0] = 23; st[1] = 2000; st[9] = 1024; status = 99;
	CHECK(callee_saved_kept(agpu_om_boot, a, &status) && status == 3);
	/* a normal call afterwards, also with the register check */
	const uint64_t b[6] = { 0x10de, 0x2e12, 64ull << 20, 0, 0, 0 };
	status = 99;
	CHECK(callee_saved_kept(agpu_om_ident, b, &status) && status == 0);
	CHECK(om_call(agpu_om_ident, 0x10de, 0x2e12, 64ull << 20, 0, 0, 0, &status) == 0 && status == 0);
}

int main(void)
{
	test_ident();
	test_abi();
	test_trap();
	if (fails)
		return 1;
	printf("UNIT PASS checks=%u\n", checks);
	return 0;
}
