// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* parity_boot -- om/boot.om (the Omega step machine the module runs) against its C reference
 * tests/host/gb20b_mmio.c, both driving the simulated GB20B SEC2 + GSP of tests/host/gb20b_sim.c.
 * For every scenario the two must make the same register accesses in the same order with the same
 * values, pause the same number of times and report the same stage, reason and outputs. A passing
 * (stage 0) scenario must also leave the simulator with no violation: no access the driver never
 * makes on GB20B and no order it never uses.
 * Host only: the simulator is not the chip. Prints PARITY-BOOT PASS scenarios=N or the first difference. */
#include <stdio.h>
#include <string.h>
#include "omhost.h"
#include "gb20b_mmio.h"
#include "gb20b_sim.h"

#define ARGS_PA 0x0000000123456000ull
#define APP_VERSION 0x0005f3a1u

static uint8_t pkt[1024];
static struct g2sim sim_c, sim_o;
static unsigned pauses_c;

static void count_pause(void *ctx)
{
	(void)ctx;
	pauses_c++;
}

struct res {
	uint32_t stage, why;
	uint32_t polls0, polls[4];
	uint32_t scratch, dwords, ememc_after;
	uint32_t rsp_bytes, rsp_ememc, rsp_dw0, rsp_dw1, rsp_task, rsp_cmd, rsp_err;
	uint32_t hwcfg2, gsp_mb0, gsp_mb1, cpuctl;
	unsigned pauses;
};

static void run_c(const struct g2sim_cfg *cfg, uint32_t len, uint32_t budget, struct res *r)
{
	struct g2_in in = { pkt, len, ARGS_PA, APP_VERSION, budget, budget, budget, budget, count_pause, 0 };
	struct g2_out o;
	struct ck_gpu_regs regs;

	g2sim_init(&sim_c, cfg);
	regs = g2sim_regs(&sim_c);
	pauses_c = 0;
	g2_boot(&regs, &in, &o);
	memset(r, 0, sizeof(*r));
	r->stage = o.stage;
	r->why = o.why;
	r->polls0 = o.polls0;
	memcpy(r->polls, o.polls, sizeof(r->polls));
	r->scratch = o.scratch;
	r->dwords = o.dwords;
	r->ememc_after = o.ememc_after;
	r->rsp_bytes = o.rsp_bytes;
	r->rsp_ememc = o.rsp_ememc;
	r->rsp_dw0 = o.rsp_dw0;
	r->rsp_dw1 = o.rsp_dw1;
	r->rsp_task = o.rsp_task;
	r->rsp_cmd = o.rsp_cmd;
	r->rsp_err = o.rsp_err;
	r->hwcfg2 = o.hwcfg2;
	r->gsp_mb0 = o.gsp_mb0;
	r->gsp_mb1 = o.gsp_mb1;
	r->cpuctl = o.cpuctl;
	r->pauses = pauses_c;
}

/* The module's executor, as kmod/aien_gpu_boot.c runs it, over the simulator instead of BAR0. */
static int run_om(const struct g2sim_cfg *cfg, uint32_t len, uint32_t budget, struct res *r)
{
	uint64_t st[32] = { 0 }, act[4], obs = 0, status;
	struct ck_gpu_regs regs;
	unsigned steps = 0;

	g2sim_init(&sim_o, cfg);
	regs = g2sim_regs(&sim_o);
	/* Pre-fill output/scratch slots 8..31 so om/boot.om's entry zeroing loop is actively tested. */
	for (int i = 8; i < 32; i++)
		st[i] = 0xdeadbeefcafebabeULL;
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
	r->stage = (uint32_t)st[12];
	r->why = (uint32_t)st[31];
	r->polls0 = (uint32_t)st[25];
	for (int i = 0; i < 4; i++)
		r->polls[i] = (uint32_t)st[13 + i];
	r->scratch = (uint32_t)st[27];
	r->dwords = (uint32_t)st[17];
	r->ememc_after = (uint32_t)st[18];
	r->rsp_bytes = (uint32_t)st[20];
	/* st[10] is the packet's EMEM start until the reply is read; the reference reports the reply value */
	r->rsp_ememc = st[20] >= 4 && st[20] <= 1024 ? (uint32_t)st[10] : 0;
	r->rsp_dw0 = (uint32_t)st[28];
	r->rsp_dw1 = (uint32_t)st[29];
	r->rsp_task = (uint32_t)st[30];
	r->rsp_cmd = (uint32_t)st[26];
	r->rsp_err = (uint32_t)st[19];
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
	for (uint32_t i = 0; i < sim_c.nlog && i < G2SIM_LOG_MAX; i++) {
		const struct g2sim_ev *a = &sim_c.log[i], *b = &sim_o.log[i];

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

/* One fault per scenario kind; kind 0 is the clean start-up. */
#define KINDS 38u
static void fault(unsigned kind, struct g2sim_cfg *c)
{
	switch (kind) {
	case 1: c->busy_reads = 2; break;
	case 2: c->busy_reads = G2SIM_NO_END; break;
	case 3: c->reply_reads = G2SIM_NO_END; break;                 /* SEC2 never replies */
	case 4: c->mask_reads = G2SIM_NO_END; break;
	case 5: c->args_reads = G2SIM_NO_END; break;
	case 6: c->lockdown_reads = G2SIM_NO_END; break;
	case 7: c->fmc_error = 0x37u; break;
	case 8: c->riscv_dead = 1; break;
	case 9: c->emem_no_inc = 1; break;
	case 10: c->hwcfg2_zero_reads = 3; break;
	case 11: c->fmc_args_pa = 0; c->args_reads = 4; break;
	case 12: c->secboot_reads = 2; break;                         /* FWSEC a little late */
	case 13: c->secboot_reads = G2SIM_NO_END; break;              /* FWSEC never done */
	case 14: c->secboot_reads = G2SIM_NO_END; c->secboot_value = 0x1ffu; break; /* 0xff only in the low byte */
	case 15: c->reply_reads = 2; break;                           /* reply a little late */
	case 16: c->rsp_bytes = 1028; break;                          /* larger than the channel */
	case 17: c->rsp_bytes = 1024; break;                          /* the largest the channel holds */
	case 18: c->rsp_no_inc = 1; break;                            /* read pointer does not move */
	case 19: c->rsp_set = 1; c->rsp_dw[0] = 0x80000000u; break;   /* first of several packets */
	case 20: c->rsp_set = 1; c->rsp_dw[0] = 0x40000000u; break;   /* last of several packets */
	case 21: c->rsp_set = 1; c->rsp_dw[0] = 0; break;             /* middle packet */
	case 22: c->rsp_set = 2; c->rsp_dw[1] = 0x1510DE7Fu; break;   /* message type not 0x7e */
	case 23: c->rsp_set = 2; c->rsp_dw[1] = 0x1510DF7Eu; break;   /* vendor not 0x10de */
	case 24: c->rsp_set = 2; c->rsp_dw[1] = 0x1410DE7Eu; break;   /* NVDM type COT, not a response */
	case 25: c->rsp_set = 2; c->rsp_dw[1] = 0x1810DE7Eu; break;   /* SMBPBI: accepted like the driver */
	case 26: c->rsp_bytes = 16; break;                            /* too short for a command response */
	case 27: c->rsp_bytes = 8; break;                             /* header only */
	case 28: c->rsp_set = 16; c->rsp_dw[4] = 0x9Eu; break;        /* FSP_ERR_IFS_ERR_INVALID_STATE */
	case 29: c->rsp_set = 16; c->rsp_dw[4] = 0x9Fu; break;        /* FSP_ERR_IFR_FILE_NOT_FOUND */
	case 30: c->rsp_set = 16; c->rsp_dw[4] = 0x80000000u; break;  /* any other non-zero code */
	case 31: c->rsp_bytes = 22; break;                            /* not dword aligned: the driver truncates */
	case 32: c->rsp_head = 0x40; break;                           /* a non-zero message-queue head */
	case 33: c->rsp_head = 0x100; c->rsp_bytes = 0xffffff14u; break; /* tail 0x10 below head: 32-bit wrap */
	case 34: c->rsp_bytes = 6; break;                             /* one dword read: no message header */
	case 35: c->rsp_bytes = 3; break;                             /* below 4: refused before EMEM read/ack */
	case 36: c->rsp_bytes = 5; break;                             /* smallest tail!=head size >= 4: 1 dword */
	case 37: c->rsp_bytes = 19; break;                            /* 1 byte below 20: too short for response */
	default: break;
	}
}

int main(void)
{
	static const uint32_t lens[] = { 0, 1, 3, 4, 5, 7, 8, 868, 1023, 1024, 1025 };
	static const uint32_t budgets[] = { 0, 1, 3, 100 };
	unsigned n = 0, clean = 0;
	unsigned seen[G2_STAGES] = { 0 }, why[G2_WHYS] = { 0 };

	for (unsigned sc = 0; sc < 2 * KINDS; sc++) {
		/* kinds 0..KINDS-1 with a pattern packet that SEC2 refuses, then again with the packet the
		 * simulator accepts: the MCTP dword, the NVDM COT dword, COT version 2 size 860 and zero
		 * FRTS fields (bytes 20..43). The GB10B packet (version 1, FRTS size set) is one more
		 * scenario after this loop. */
		static const uint32_t hdr[3] = { 0xC0000000u, 0x1410DE7Eu, 2u | (860u << 16) };

		for (unsigned i = 0; i < sizeof(pkt); i++)
			pkt[i] = (uint8_t)(i * 7u + 3u);
		if (sc >= KINDS) {
			memcpy(pkt, hdr, sizeof(hdr));
			memset(pkt + 20, 0, 24);
		}
		for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
			for (unsigned bi = 0; bi < sizeof(budgets) / sizeof(budgets[0]); bi++) {
				struct g2sim_cfg c;
				struct res rc, ro;
				char name[96];

				memset(&c, 0, sizeof(c));
				c.reply_reads = 3;
				c.mask_reads = 4;
				c.args_reads = 2;
				c.lockdown_reads = 5;
				c.fmc_args_pa = ARGS_PA;
				fault(sc % KINDS, &c);
				snprintf(name, sizeof(name), "scenario %u len %u budget %u", sc, lens[li], budgets[bi]);
				run_c(&c, lens[li], budgets[bi], &rc);
				if (run_om(&c, lens[li], budgets[bi], &ro))
					return 1;
				if (!same(name))
					return 1;
				if (memcmp(&rc, &ro, sizeof(rc))) {
					printf("PARITY-BOOT FAIL %s: outputs differ: stage C %u Omega %u, why %u/%u, polls %u/%u %u/%u %u/%u %u/%u %u/%u, pauses %u/%u, rsp %u/%u err %#x/%#x ememc %#x/%#x\n",
					       name, rc.stage, ro.stage, rc.why, ro.why, rc.polls0, ro.polls0, rc.polls[0],
					       ro.polls[0], rc.polls[1], ro.polls[1], rc.polls[2], ro.polls[2], rc.polls[3],
					       ro.polls[3], rc.pauses, ro.pauses, rc.rsp_bytes, ro.rsp_bytes, rc.rsp_err,
					       ro.rsp_err, rc.rsp_ememc, ro.rsp_ememc);
					return 1;
				}
				if (rc.stage >= G2_STAGES || rc.why >= G2_WHYS) {
					printf("PARITY-BOOT FAIL %s: stage %u why %u out of range\n", name, rc.stage, rc.why);
					return 1;
				}
				if (rc.stage == G2_OK) {
					/* a start-up reported good made no access and used no order the driver never does */
					if (sim_c.violations || sim_c.os != APP_VERSION || sim_c.phase != 4) {
						printf("PARITY-BOOT FAIL %s: stage 0 with %u violations, phase %u\n", name,
						       sim_c.violations, sim_c.phase);
						return 1;
					}
					clean++;
				}
				if ((rc.why != 0) != (rc.stage == G2_E_REPLY)) {
					printf("PARITY-BOOT FAIL %s: reason %u with stage %u\n", name, rc.why, rc.stage);
					return 1;
				}
				seen[rc.stage]++;
				why[rc.why]++;
				n++;
			}
		}
	}
	/* The GB10B packet (COT version 1, FRTS size = the FMC size) on a GB20B SEC2: refused by the chip
	 * model with an error reply, and the start-up must stop at "SEC2 reported an error". */
	{
		static const uint32_t hdr1[3] = { 0xC0000000u, 0x1410DE7Eu, 1u | (860u << 16) };
		struct g2sim_cfg c;
		struct res rc, ro;

		memset(&c, 0, sizeof(c));
		c.fmc_args_pa = ARGS_PA;
		memset(pkt, 0, sizeof(pkt));
		memcpy(pkt, hdr1, sizeof(hdr1));
		pkt[28] = 0x48; pkt[29] = 0xfd; pkt[30] = 0x02; /* 195912, the GB10B FMC size */
		run_c(&c, 868, 100, &rc);
		if (run_om(&c, 868, 100, &ro) || !same("gb10b packet") || memcmp(&rc, &ro, sizeof(rc)))
			return 1;
		if (rc.stage != G2_E_SEC2ERR || rc.rsp_err != 0xA1u) {
			printf("PARITY-BOOT FAIL gb10b packet: stage %u err %#x, want 11 and 0xa1\n", rc.stage, rc.rsp_err);
			return 1;
		}
		n++;
	}
	/* coverage gate: every stage, success included, and every refusal reason was reached */
	for (unsigned s = 0; s < G2_STAGES; s++)
		if (!seen[s]) {
			printf("PARITY-BOOT FAIL coverage: no scenario ended at stage %u\n", s);
			return 1;
		}
	for (unsigned w = 1; w < G2_WHYS; w++)
		if (!why[w]) {
			printf("PARITY-BOOT FAIL coverage: no scenario refused the reply for reason %u\n", w);
			return 1;
		}
	printf("PARITY-BOOT PASS scenarios=%u ok=%u clean=%u stages-1..11=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u reasons-1..7=%u,%u,%u,%u,%u,%u,%u\n",
	       n, seen[0], clean, seen[1], seen[2], seen[3], seen[4], seen[5], seen[6], seen[7], seen[8], seen[9],
	       seen[10], seen[11], why[1], why[2], why[3], why[4], why[5], why[6], why[7]);
	return 0;
}
