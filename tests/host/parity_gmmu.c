/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* parity_gmmu -- om/gmmu.om, om/memchk.om and om/rpdes.om (the words kmod/aien_gpu_mem.c runs)
 * against the oracle aien-dev/aienux c9678d7b native/kernel/gpu/gmmu.c (entry encoders, level index
 * math, TLB invalidate register writes, and page tables the oracle built, walked with the Omega
 * plan), and against hand vectors from the NVIDIA 580.173.02 headers where the oracle has no
 * counterpart (PTE PCF, reserved-PDE parameters, argument checks).
 * Disagreement with the oracle, on purpose (source wins): PTE PCF 16/17 (REGULAR_RW_ATOMIC_*_ACD,
 * virt_mem_allocator_gm107.c:2436-2490) where the oracle's constants are 0/1 (*_ACE); the oracle
 * encoders are called here with the source values. Prints PARITY-GMMU PASS or the first difference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omhost.h"
#include "gmmu.c"

extern const char agpu_om_gmmu[], agpu_om_memchk[], agpu_om_rpdes[];

#define FAIL 1000000ull
static unsigned long checks;

static void die(const char *what, uint64_t got, uint64_t want)
{
	printf("PARITY-GMMU FAIL %s: got %llu want %llu\n", what, (unsigned long long)got,
	       (unsigned long long)want);
	exit(1);
}

static void eq(const char *what, uint64_t got, uint64_t want)
{
	checks++;
	if (got != want)
		die(what, got, want);
}

static uint64_t call(const void *u, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
		     uint64_t a5)
{
	uint64_t st = 0, r = om_call(u, a0, a1, a2, a3, a4, a5, &st);

	if (st)
		die("trap", st, 0);
	return r;
}

static uint64_t g(uint64_t op, uint64_t a, uint64_t b) { return call(agpu_om_gmmu, op, a, b, 0, 0, 0); }

static uint64_t tab[512];
static uint64_t m(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
	return call(agpu_om_memchk, op, P(tab), a, b, c, d);
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* ---- TLB register recorder for ck_gmmu_invalidate ---- */
static uint32_t wr_off[8], wr_val[8], nwr;
static uint32_t rd_reg(void *ctx, uint32_t off) { (void)ctx; (void)off; return 0; }
static void wr_reg(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	if (nwr < 8) {
		wr_off[nwr] = off;
		wr_val[nwr] = v;
	}
	nwr++;
}

static uint64_t le(const uint8_t *p, int n)
{
	uint64_t v = 0;

	for (int i = n - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void encoders(void)
{
	for (int k = 0; k < 20000; k++) {
		uint64_t va = rnd() & ((1ull << 57) - 1);
		uint32_t lv = (uint32_t)(rnd() % 7);

		eq("level index", g(1, lv, va), lv_index(lv, va));
		uint64_t pa = (rnd() & 0x000FFFFFFFFFF000ull) | 0x1000;
		uint32_t ap = 2 + (uint32_t)(rnd() & 1);

		eq("pde", g(2, pa, ap), ck_gmmu_pde(pa, ap));
		uint64_t dual[2], bpa = (rnd() & 0x000FFFFFFFFFFF00ull) | 0x100;

		ck_gmmu_dual_pde(dual, bpa, ap, pa, ap);
		eq("dual big", g(6, bpa, ap), dual[0]);
		eq("dual small", g(2, pa, ap), dual[1]);
		uint64_t fl = rnd() & 3;

		eq("pte", g(3, pa, fl), ck_gmmu_pte(pa, 2 + (uint32_t)(fl >> 1), 16 + (uint32_t)(fl & 1), 0));
	}
	for (uint32_t lv = 0; lv < 7; lv++) {
		eq("entries", g(7, lv, 0), 1ull << (lv_hi[lv] - lv_lo[lv] + 1));
		eq("entry bytes", g(8, lv, 0), lv == LV_PD0 ? 16 : 8);
	}
	/* hand vectors (dev_mmu.h fields; decimal values from Wolfram) */
	eq("pte hand cached", g(3, 0x12345000, 0), 305418373);
	eq("pte hand uncached", g(3, 0x12345000, 1), 305418381);
	eq("pde hand", g(2, 0xFEDCBA000ull, 2), 68414054420ull);
	/* refusals */
	eq("idx level 7", g(1, 7, 0), FAIL + 1);
	eq("idx va 2^57", g(1, 0, 1ull << 57), FAIL + 6);
	eq("pde pa 0", g(2, 0, 2), FAIL + 2);
	eq("pde unaligned", g(2, 0x1800, 2), FAIL + 2);
	eq("pde 2^52", g(2, 1ull << 52, 2), FAIL + 2);
	eq("pde ap 1 (video)", g(2, 0x1000, 1), FAIL + 3);
	eq("pde ap 4", g(2, 0x1000, 4), FAIL + 3);
	eq("big unaligned", g(6, 0x1080, 2), FAIL + 2);
	eq("big ok 256", g(6, 0x1100, 3), 0x1100 | 16 | 6);
	eq("pte flags 4", g(3, 0x1000, 4), FAIL + 4);
	eq("pte unaligned", g(3, 0x1001, 0), FAIL + 2);
	eq("entries level 7", g(7, 7, 0), FAIL + 1);
	eq("bytes level 7", g(8, 7, 0), FAIL + 1);
	eq("unknown op", g(11, 0, 0), FAIL + 99);
	/* walk plan: 4 KiB 0 1 2 3 4 6 leaf; 64 KiB ... 4 5 leaf; 2 MiB ... 4 leaf */
	for (uint32_t lv = 0; lv < 4; lv++)
		eq("next dir", g(9, lv, 12), lv + 1);
	eq("next pd0 4k", g(9, 4, 12), 6);
	eq("next pd0 64k", g(9, 4, 16), 5);
	eq("next pd0 2m", g(9, 4, 21), 7);
	eq("next big", g(9, 5, 16), 7);
	eq("next small", g(9, 6, 12), 7);
	eq("next small 64k", g(9, 6, 16), FAIL + 1);
	eq("next page shift", g(9, 0, 13), FAIL + 7);
	eq("next level 7", g(9, 7, 12), FAIL + 1);
	/* GSP's range: PD0 and below are GSP-RM's */
	eq("cpuok pd1 4G", g(10, 3, 1ull << 32), 0);
	eq("cpuok pd0 4G", g(10, 4, 1ull << 32), FAIL + 5);
	eq("cpuok pt 4.5G-1", g(10, 6, 4831838207ull), FAIL + 5);
	eq("cpuok pd0 4.5G", g(10, 4, 4831838208ull), 0);
	eq("cpuok pd0 4G-1", g(10, 4, 4294967295ull), 0);
	eq("cpuok 2^57", g(10, 0, 1ull << 57), FAIL + 6);
}

static uint8_t winmem[160 * 4096] __attribute__((aligned(4096)));

static void tables(void)
{
	struct ck_gpu_window w = { winmem, 0x123456789000ull, sizeof(winmem) };
	struct ck_gmmu_as as;
	struct ck_gpu_regs regs = { rd_reg, wr_reg, NULL };
	uint64_t vas[12], pas[12];
	int n = 0;

	memset(winmem, 0, sizeof(winmem));
	if (ck_gmmu_as_init(&as, &w, w.pa_base, 64) != CK_GMMU_OK)
		die("oracle as_init", 1, 0);
	/* TLB words: the oracle's three writes vs om/gmmu.om op 4 */
	nwr = 0;
	if (ck_gmmu_invalidate(&as, &regs, 4) != CK_GMMU_OK || nwr != 3)
		die("oracle invalidate", nwr, 3);
	eq("tlb pdb reg", wr_off[0], 0xB830A0);
	eq("tlb pdb", g(4, as.root_pa, 0), wr_val[0]);
	eq("tlb upper", g(4, as.root_pa, 1), wr_val[1]);
	eq("tlb inval", g(4, as.root_pa, 2), wr_val[2]);
	eq("tlb upper nonzero", wr_val[1] != 0, 1);
	eq("tlb hand pdb", g(4, 0xFEDCBA000ull, 0), 267242402);
	eq("tlb hand upper", g(4, 0xFEDCBA000ull, 1), 0);
	eq("tlb root 0", g(4, 0, 0), FAIL + 2);
	eq("tlb which 3", g(4, 0x1000, 3), FAIL + 1);
	eq("pending set", g(5, 0x80000000ull, 0), 1);
	eq("pending clear", g(5, 0x7fffffffull, 0), 0);
	/* tables the oracle builds, walked with the Omega plan and compared with Omega encodings */
	while (n < 12) {
		uint64_t va = (rnd() & ((1ull << 48) - 1)) & ~0xFFFull, pa;
		int dup = 0;

		if (va >= 0x100000000ull && va < 0x120000000ull)
			continue;
		for (int i = 0; i < n; i++)
			dup |= (vas[i] >> 21) == (va >> 21);
		if (dup && n % 3)
			continue;
		pa = w.pa_base + (uint64_t)(64 + n) * 4096;
		if (ck_gmmu_map(&as, va, pa, 4096, CK_GMMU_SZ_4K, 2, 16) != CK_GMMU_OK)
			continue;
		vas[n] = va;
		pas[n++] = pa;
	}
	for (int i = 0; i < n; i++) {
		uint64_t t = as.root_pa, lv = 0, va = vas[i];

		for (int step = 0; step < 7; step++) {
			uint64_t idx = g(1, lv, va), nx = g(9, lv, 12), esz = g(8, lv, 0);
			const uint8_t *e = winmem + (t - w.pa_base) + idx * esz;

			if (nx == 7) {
				eq("leaf pte", le(e, 8), g(3, pas[i], 0));
				break;
			}
			uint64_t word = le(e + (esz == 16 ? 8 : 0), 8), next = word & 0x000FFFFFFFFFF000ull;

			if (esz == 16)
				eq("dual big half empty", le(e, 8), 0);
			eq("pde in oracle table", word, g(2, next, 2));
			t = next;
			lv = nx;
		}
	}
}

static void memchk(void)
{
	const uint64_t G4 = 4294967296ull, G45 = 4831838208ull, T48 = 1ull << 48, B = 2ull << 40;

	eq("alloc ok", m(1, 4096, 0x10000, 0, 0), 0);
	eq("alloc uncached", m(1, 4096, 0x10000, 1, 0), 0);
	eq("alloc size 0", m(1, 0, 0x10000, 0, 0), FAIL + 1);
	eq("alloc size odd", m(1, 4097, 0x10000, 0, 0), FAIL + 1);
	eq("alloc 64G", m(1, 1ull << 36, 0x200000000ull, 0, 0), 0);
	eq("alloc 64G+4K", m(1, (1ull << 36) + 4096, 0x200000000ull, 0, 0), FAIL + 1);
	eq("alloc va odd", m(1, 4096, 0x10001, 0, 0), FAIL + 2);
	eq("alloc top", m(1, 4096, T48 - 4096, 0, 0), 0);
	eq("alloc past top", m(1, 8192, T48 - 4096, 0, 0), FAIL + 3);
	eq("alloc va 2^48", m(1, 4096, T48, 0, 0), FAIL + 3);
	eq("alloc wrap", m(1, 1ull << 36, 0xFFFFFFFFFFFFF000ull, 0, 0), FAIL + 3);
	eq("alloc below gsp", m(1, 4096, G4 - 4096, 0, 0), 0);
	eq("alloc into gsp", m(1, 8192, G4 - 4096, 0, 0), FAIL + 4);
	eq("alloc gsp end", m(1, 4096, G45 - 4096, 0, 0), FAIL + 4);
	eq("alloc above gsp", m(1, 4096, G45, 0, 0), 0);
	eq("alloc across gsp", m(1, 1ull << 30, G4 - 4096, 0, 0), FAIL + 4);
	eq("alloc flags 2", m(1, 4096, 0x10000, 2, 0), FAIL + 5);
	tab[0] = 0x10000;
	tab[1] = 0x2000;
	tab[2] = 0x20000;
	tab[3] = 0x1000;
	eq("overlap adjacent", m(2, 2, 0x12000, 0x1000, 0), 0);
	eq("overlap after", m(2, 2, 0x21000, 0x1000, 0), 0);
	eq("overlap inside", m(2, 2, 0x11000, 0x1000, 0), FAIL + 6);
	eq("overlap front", m(2, 2, 0xF000, 0x2000, 0), FAIL + 6);
	eq("overlap cover", m(2, 2, 0x1000, 0x100000, 0), FAIL + 6);
	eq("overlap second", m(2, 2, 0x20000, 0x1000, 0), FAIL + 6);
	eq("overlap n 1 skips second", m(2, 1, 0x20000, 0x1000, 0), 0);
	eq("overlap full", m(2, 256, 0x900000, 0x1000, 0), FAIL + 8);
	eq("overlap n 257", m(2, 257, 0x900000, 0x1000, 0), FAIL + 7);
	memset(tab, 0, sizeof(tab));
	eq("mmap exact", m(3, 0x10000, 0x2000, 0x10000, 0x2000), 0);
	eq("mmap elsewhere", m(3, 0x11000, 0x2000, 0x10000, 0x2000), FAIL + 9);
	eq("mmap short", m(3, 0x10000, 0x1000, 0x10000, 0x2000), FAIL + 9);
	eq("handle", m(4, B + (3ull << 36), 0, 0, 0), 3);
	eq("handle base", m(4, B, 0, 0, 0), FAIL + 10);
	eq("handle below", m(4, 4096, 0, 0, 0), FAIL + 10);
	eq("handle offset", m(4, B + (3ull << 36) + 4096, 0, 0, 0), FAIL + 10);
	eq("handle 2^20", m(4, B + (1ull << 56), 0, 0, 0), FAIL + 10);
	eq("off of 3", m(7, 3, 0, 0, 0), B + (3ull << 36));
	eq("off of 0", m(7, 0, 0, 0, 0), FAIL + 13);
	eq("off of 2^20", m(7, 1ull << 20, 0, 0, 0), FAIL + 13);
	for (int k = 0; k < 2000; k++) {
		uint64_t h = 1 + rnd() % ((1ull << 20) - 1);

		eq("handle round trip", m(4, m(7, h, 0, 0, 0), 0, 0, 0), h);
	}
	eq("slice whole", m(5, 0, 4096, 4096, 0), 0);
	eq("slice inner", m(5, 100, 3996, 4096, 0), 0);
	eq("slice past", m(5, 100, 3997, 4096, 0), FAIL + 11);
	eq("slice empty", m(5, 0, 0, 4096, 0), FAIL + 11);
	eq("slice off end", m(5, 4096, 1, 4096, 0), FAIL + 11);
	eq("slice wrap", m(5, 1, ~0ull, 4096, 0), FAIL + 11);
	eq("contig", m(6, 0x5000, 0x6000, 0, 0), 0);
	eq("not contig", m(6, 0x5000, 0x8000, 0, 0), FAIL + 12);
	eq("kva first", m(8, T48, 4096, 0, 0), T48);
	eq("kva from 0", m(8, 0, 4096, 0, 0), T48);
	eq("kva 8K align", m(8, T48 + 4096, 8192, 0, 0), T48 + 8192);
	eq("kva 12K align 16K", m(8, T48 + 4096, 12288, 0, 0), T48 + 16384);
	eq("kva 3M align 2M", m(8, T48 + 4096, 3ull << 20, 0, 0), T48 + (2ull << 20));
	eq("kva size 0", m(8, T48, 0, 0, 0), FAIL + 14);
	eq("kva full", m(8, T48 + (1ull << 40) - 4096, 8192, 0, 0), FAIL + 14);
	eq("kva last page", m(8, T48 + (1ull << 40) - 4096, 4096, 0, 0), T48 + (1ull << 40) - 4096);
	eq("memchk unknown op", m(9, 0, 0, 0, 0), FAIL + 99);
}

static void rpdes(void)
{
	static const uint8_t shift[5] = { 56, 47, 38, 29, 21 };
	uint8_t p[184];
	uint64_t lv[8] = { 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 1, 0, 0 };

	memset(p, 0xAA, sizeof(p));
	eq("rpdes ok", call(agpu_om_rpdes, P(p), P(lv), 0, 0, 0, 0), 0);
	/* offsets: ctrl90f1.h:270-332 via gdb ptype /o (stagec-layout/layouts1.txt:140-155) */
	eq("hSubDevice", le(p, 4), 0);
	eq("subDeviceId", le(p + 4, 4), 0);
	eq("pageSize", le(p + 8, 8), 0x200000);
	eq("virtAddrLo", le(p + 16, 8), 0x100000000ull);
	eq("virtAddrHi", le(p + 24, 8), 0x11FFFFFFFull);
	eq("numLevelsToCopy", le(p + 32, 4), 5);
	eq("hole", le(p + 36, 4), 0);
	for (int i = 0; i < 6; i++) {
		const uint8_t *l = p + 40 + 24 * i;

		eq("level physAddress", le(l, 8), i < 5 ? lv[i] : 0);
		eq("level size", le(l + 8, 8), i == 0 ? 16 : i < 5 ? 4096 : 0);
		eq("level aperture", le(l + 16, 4), i < 5 ? 4 : 0);	/* GMMU_APERTURE_SYS_COH */
		eq("level pageShift", l[20], i < 5 ? shift[i] : 0);
		eq("level pad", le(l + 21, 3), 0);
	}
	lv[5] = 0;
	eq("rpdes noncoh", call(agpu_om_rpdes, P(p), P(lv), 0, 0, 0, 0), 0);
	eq("level aperture noncoh", le(p + 40 + 16, 4), 3);
	lv[5] = 2;
	eq("rpdes flag 2", call(agpu_om_rpdes, P(p), P(lv), 0, 0, 0, 0), FAIL + 2);
	lv[5] = 1;
	lv[2] = 0;
	eq("rpdes level 0", call(agpu_om_rpdes, P(p), P(lv), 0, 0, 0, 0), FAIL + 1);
	lv[2] = 0x3001;
	eq("rpdes unaligned", call(agpu_om_rpdes, P(p), P(lv), 0, 0, 0, 0), FAIL + 1);
	lv[2] = 1ull << 52;
	eq("rpdes 2^52", call(agpu_om_rpdes, P(p), P(lv), 0, 0, 0, 0), FAIL + 1);
}

int main(void)
{
	encoders();
	tables();
	memchk();
	rpdes();
	printf("PARITY-GMMU PASS checks=%lu (oracle aienux c9678d7b gmmu.c; PCF per NVIDIA source)\n", checks);
	return 0;
}
