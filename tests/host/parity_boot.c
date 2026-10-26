/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* parity_boot -- om/boot.om (the Omega step machine the module runs) against its oracle, the C start-up
 * of aien-dev/aienux c9678d7b native/kernel/gpu/gsp_mmio.c, both driving that commit's simulated SEC2 +
 * GSP (gpu/gsp_sim.c). For every scenario the two must make the same register accesses in the same
 * order with the same values, pause the same number of times and report the same stage and outputs.
 * Host only: the simulator is not the chip. Prints PARITY-BOOT PASS scenarios=N or the first difference. */
#include <stdio.h>
#include <string.h>
#include "omhost.h"
#include "gsp_mmio.h"
#include "gsp_sim.h"

#define ARGS_PA 0x0000000123456000ull
#define APP_VERSION 0x0005f3a1u

static uint8_t pkt[1024];
static struct ck_gspsim sim_c, sim_o;
static unsigned pauses_c;

static void count_pause(void *ctx)
{
	(void)ctx;
	pauses_c++;
}

struct res {
	int32_t stage;
	uint32_t polls[4], dwords, ememc_after, sec2_mb0, sec2_mb1, hwcfg2, gsp_mb0, gsp_mb1, cpuctl;
	unsigned pauses;
};

static void run_c(const struct ck_gspsim_cfg *cfg, uint32_t len, uint32_t budget, struct res *r)
{
	struct ck_gsp_mmio_in in = { pkt, len, ARGS_PA, APP_VERSION, budget, budget, budget, budget, count_pause, 0 };
	struct ck_gsp_mmio_out o;
	struct ck_gpu_regs regs;

	ck_gspsim_init(&sim_c, cfg);
	regs = ck_gspsim_regs(&sim_c);
	pauses_c = 0;
	ck_gsp_mmio_boot(&regs, &in, &o);
	r->stage = o.stage;
	memcpy(r->polls, o.polls, sizeof(r->polls));
	r->dwords = o.dwords;
	r->ememc_after = o.ememc_after;
	r->sec2_mb0 = o.sec2_mb0;
	r->sec2_mb1 = o.sec2_mb1;
	r->hwcfg2 = o.hwcfg2;
	r->gsp_mb0 = o.gsp_mb0;
	r->gsp_mb1 = o.gsp_mb1;
	r->cpuctl = o.cpuctl;
	r->pauses = pauses_c;
}

/* The module's executor, as kmod/aien_gpu_boot.c runs it, over the simulator instead of BAR0. */
static int run_om(const struct ck_gspsim_cfg *cfg, uint32_t len, uint32_t budget, struct res *r)
{
	uint64_t st[32] = { 0 }, act[4], obs = 0, status;
	struct ck_gpu_regs regs;
	unsigned steps = 0;

	ck_gspsim_init(&sim_o, cfg);
	regs = ck_gspsim_regs(&sim_o);
	st[1] = len;
	st[2] = ARGS_PA;
	st[3] = APP_VERSION;
	st[4] = st[5] = st[6] = st[7] = budget;
	memset(r, 0, sizeof(*r));
	for (;;) {
		uint64_t v = om_call(agpu_om_boot, P(st), P(pkt), obs, P(act), 0, 0, &status);
		if (status) {
			printf("PARITY-BOOT FAIL omega trap %llu\n", (unsigned long long)status);
			return -1;
		}
		if (v == 1)
			break;
		if (v != 0 || ++steps > 4000000) {
			printf("PARITY-BOOT FAIL omega result %llu steps %u\n", (unsigned long long)v, steps);
			return -1;
		}
		obs = 0;
		if (act[0] == 1)
			obs = regs.rd(regs.ctx, (uint32_t)act[1]);
		else if (act[0] == 2)
			regs.wr(regs.ctx, (uint32_t)act[1], (uint32_t)act[2]);
		else if (act[0] == 3)
			r->pauses++;
		else {
			printf("PARITY-BOOT FAIL omega action %llu\n", (unsigned long long)act[0]);
			return -1;
		}
	}
	r->stage = -(int32_t)st[12];
	for (int i = 0; i < 4; i++)
		r->polls[i] = (uint32_t)st[13 + i];
	r->dwords = (uint32_t)st[17];
	r->ememc_after = (uint32_t)st[18];
	r->sec2_mb0 = (uint32_t)st[19];
	r->sec2_mb1 = (uint32_t)st[20];
	r->hwcfg2 = (uint32_t)st[21];
	r->gsp_mb0 = (uint32_t)st[22];
	r->gsp_mb1 = (uint32_t)st[23];
	r->cpuctl = (uint32_t)st[24];
	return 0;
}

static int same(const char *name)
{
	if (sim_c.nlog != sim_o.nlog) {
		printf("PARITY-BOOT FAIL %s: %u accesses (C) vs %u (Omega)\n", name, sim_c.nlog, sim_o.nlog);
		return 0;
	}
	for (uint32_t i = 0; i < sim_c.nlog && i < CK_GSPSIM_LOG_MAX; i++) {
		const struct ck_gspsim_ev *a = &sim_c.log[i], *b = &sim_o.log[i];
		if (a->off != b->off || a->val != b->val || a->write != b->write) {
			printf("PARITY-BOOT FAIL %s: access %u C %s %#x=%#x Omega %s %#x=%#x\n", name, i,
			       a->write ? "wr" : "rd", a->off, a->val, b->write ? "wr" : "rd", b->off, b->val);
			return 0;
		}
	}
	if (sim_c.violations != sim_o.violations || memcmp(sim_c.emem, sim_o.emem, sizeof(sim_c.emem))) {
		printf("PARITY-BOOT FAIL %s: simulator state differs\n", name);
		return 0;
	}
	return 1;
}

int main(void)
{
	static const uint32_t lens[] = { 0, 1, 3, 4, 5, 7, 8, 868, 1023, 1024, 1025 };
	static const uint32_t budgets[] = { 0, 1, 3, 100 };
	unsigned n = 0;

	for (unsigned i = 0; i < sizeof(pkt); i++)
		pkt[i] = (uint8_t)(i * 7u + 3u);
	unsigned seen[9] = { 0 };

	for (unsigned sc = 0; sc < 24; sc++) {
		/* scenarios 12..23 repeat 0..11 with the packet the simulator accepts: the MCTP dword, the
		 * NVDM COT dword and the COT payload header of L192 gsp_boot_build (test_gsp_mmio.c
		 * make_packet); 0..11 keep a pattern packet that SEC2 rejects. */
		static const uint32_t hdr[3] = { 0xC0000000u, 0x1410DE7Eu, 1u | (860u << 16) };
		if (sc == 12)
			memcpy(pkt, hdr, sizeof(hdr));
		for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
			for (unsigned bi = 0; bi < sizeof(budgets) / sizeof(budgets[0]); bi++) {
				struct ck_gspsim_cfg c = { 0, 3, 4, 2, 5, 0, 0, 0, ARGS_PA, 0 };
				struct res rc, ro;
				char name[96];

				switch (sc % 12) {
				case 1: c.busy_reads = 2; break;
				case 2: c.busy_reads = CK_GSPSIM_NO_END; break;
				case 3: c.cmd_reads = CK_GSPSIM_NO_END; break;
				case 4: c.mask_reads = CK_GSPSIM_NO_END; break;
				case 5: c.args_reads = CK_GSPSIM_NO_END; break;
				case 6: c.lockdown_reads = CK_GSPSIM_NO_END; break;
				case 7: c.fmc_error = 0x37u; break;
				case 8: c.riscv_dead = 1; break;
				case 9: c.emem_no_inc = 1; break;
				case 10: c.hwcfg2_zero_reads = 3; break;
				case 11: c.fmc_args_pa = 0; c.args_reads = 4; break;
				default: break;
				}
				snprintf(name, sizeof(name), "scenario %u len %u budget %u", sc, lens[li], budgets[bi]);
				run_c(&c, lens[li], budgets[bi], &rc);
				if (run_om(&c, lens[li], budgets[bi], &ro))
					return 1;
				if (!same(name))
					return 1;
				if (memcmp(&rc, &ro, sizeof(rc))) {
					printf("PARITY-BOOT FAIL %s: outputs differ: stage C %d Omega %d, polls %u/%u %u/%u %u/%u %u/%u, pauses %u/%u\n",
					       name, rc.stage, ro.stage, rc.polls[0], ro.polls[0], rc.polls[1], ro.polls[1],
					       rc.polls[2], ro.polls[2], rc.polls[3], ro.polls[3], rc.pauses, ro.pauses);
					return 1;
				}
				if (rc.stage <= 0 && rc.stage >= -8)
					seen[-rc.stage]++;
				n++;
			}
		}
	}
	/* coverage gate: every stage of the start-up, success included, was reached by some scenario */
	for (unsigned s = 0; s < 9; s++)
		if (!seen[s]) {
			printf("PARITY-BOOT FAIL coverage: no scenario ended at stage %u\n", s);
			return 1;
		}
	printf("PARITY-BOOT PASS scenarios=%u ok=%u stages-1..8=%u,%u,%u,%u,%u,%u,%u,%u\n", n, seen[0], seen[1],
	       seen[2], seen[3], seen[4], seen[5], seen[6], seen[7], seen[8]);
	return 0;
}
