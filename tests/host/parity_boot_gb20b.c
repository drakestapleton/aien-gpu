/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* parity_boot_gb20b -- host verification of the GB20B (Spark 0x1b/0xb) path:
 *   1. tools/gb20b_boot.c builder parity against oracle gb10_gsp_boot.c on every shared page
 *      (GSP_ARGUMENTS_CACHED, GSP_FMC_BOOT_PARAMS, libos_args, logs, rpc_shared, WprMeta) plus
 *      GB20B COTv2/bootloader/FMC/signature assertions and negative controls.
 *   2. om/boot.om (st[25]=1) against a simulated GB20B SEC2 + GSP register block modeled from
 *      NVIDIA open-gpu-kernel-modules 580.173.02 kernel_sec2_gb20b.c.
 * Host only: no device or GPU is touched. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omhost.h"
#include "gb10_gsp_boot.h"

extern const char agpu_om_boot[];
extern int gb20b_boot_build(gsp_arena *a, const gsp_boot_in *in, gsp_boot_plan *out, const char **why);

#define REG_SCRATCH      0x000005e0u
#define REG_SEC2_EMEMC0  0x00840ac0u
#define REG_SEC2_EMEMD0  0x00840ac4u
#define REG_SEC2_QHEAD0  0x00840c00u
#define REG_SEC2_QTAIL0  0x00840c04u
#define REG_SEC2_MHEAD0  0x00840c80u
#define REG_SEC2_MTAIL0  0x00840c84u
#define REG_GSP_MB0      0x00110040u
#define REG_GSP_MB1      0x00110044u
#define REG_GSP_OS       0x00110080u
#define REG_GSP_HWCFG2   0x001100f4u
#define REG_GSP_CPUCTL   0x00111388u

static unsigned checks;

static void check(const char *what, int cond)
{
	checks++;
	if (!cond) {
		printf("PARITY-BOOT-GB20B FAIL %s\n", what);
		exit(1);
	}
}

static void wr32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ---- 1. Builder parity against gb10_gsp_boot.c ---- */
static void test_builder_parity(void)
{
	static uint8_t elf[16384], sig[4096], bl10[GSP_GB10B_BOOTLOADER_SIZE], bl20[212992];
	static uint8_t fmc[212296], h[48], pk[384], s[384], si[GSP_SYSTEM_INFO_SIZE];
	size_t arena_sz = 4u << 20;
	uint8_t *buf10 = aligned_alloc(GSP_PAGE, arena_sz);
	uint8_t *buf20 = aligned_alloc(GSP_PAGE, arena_sz);
	gsp_arena a10, a20;
	gsp_boot_in in10 = { 0 }, in20 = { 0 };
	gsp_boot_plan p10, p20;
	const char *why = NULL;

	check("alloc arenas", buf10 && buf20);
	memset(buf10, 0, arena_sz);
	memset(buf20, 0, arena_sz);
	for (size_t i = 0; i < sizeof(elf); i++) elf[i] = (uint8_t)(i * 3u + 1u);
	for (size_t i = 0; i < sizeof(sig); i++) sig[i] = (uint8_t)(i * 5u + 2u);
	for (size_t i = 0; i < sizeof(bl10); i++) bl10[i] = (uint8_t)(i * 7u + 3u);
	for (size_t i = 0; i < sizeof(bl20); i++) bl20[i] = (uint8_t)(i * 11u + 4u);
	for (size_t i = 0; i < sizeof(fmc); i++) fmc[i] = (uint8_t)(i * 13u + 5u);
	for (size_t i = 0; i < sizeof(h); i++) h[i] = (uint8_t)(0x40u + i);
	for (size_t i = 0; i < sizeof(pk); i++) pk[i] = (uint8_t)(0x80u + i);
	for (size_t i = 0; i < sizeof(s); i++) s[i] = (uint8_t)(0xc0u + i);
	for (size_t i = 0; i < sizeof(si); i++) si[i] = (uint8_t)(i ^ 0x5au);

	in10.rm_elf = elf; in10.rm_elf_size = sizeof(elf);
	in10.rm_sig = sig; in10.rm_sig_size = sizeof(sig);
	in10.bootloader = bl10; in10.bootloader_size = sizeof(bl10);
	in10.bl_code_off = GSP_GB10B_BL_CODE_OFF;
	in10.bl_data_off = GSP_GB10B_BL_DATA_OFF;
	in10.bl_manifest_off = GSP_GB10B_BL_MANIFEST_OFF;
	in10.fmc = fmc; in10.fmc_size = sizeof(fmc);
	in10.fmc_hash = h; in10.fmc_pubkey = pk; in10.fmc_sig = s;
	in10.sysmem_heap_pa = 0x5000000000ull; in10.sysmem_heap_size = 512ull << 20;
	in10.system_info = si; in10.gpu_instance = 0; in10.fmc_regkeys = 0x12345678u; in10.dma_bits = 40;

	in20 = in10;
	in20.bootloader = bl20; in20.bootloader_size = sizeof(bl20);
	in20.bl_code_off = 0xf000u;

	check("arena init 10", gsp_arena_init(&a10, buf10, 0x4000001000ull, arena_sz) == 0);
	check("arena init 20", gsp_arena_init(&a20, buf20, 0x4000001000ull, arena_sz) == 0);
	check("build gb10b", gsp_boot_build(&a10, &in10, &p10, &why) == 0);
	check("build gb20b", gb20b_boot_build(&a20, &in20, &p20, &why) == 0);

	/* Shared pages placed before bootloader must match byte-for-byte */
	check("args_cached pa eq", p10.args_cached.pa == p20.args_cached.pa);
	check("args_cached bytes eq",
	      memcmp(buf10 + (p10.args_cached.pa - a10.pa),
		     buf20 + (p20.args_cached.pa - a20.pa), GSP_PAGE) == 0);
	check("libos_args bytes eq",
	      memcmp(buf10 + (p10.libos_args.pa - a10.pa),
		     buf20 + (p20.libos_args.pa - a20.pa), GSP_PAGE) == 0);
	check("rpc_shared bytes eq",
	      memcmp(buf10 + (p10.rpc_shared.pa - a10.pa),
		     buf20 + (p20.rpc_shared.pa - a20.pa), GSP_MQ_SHARED_SIZE) == 0);
	for (unsigned i = 0; i < GSP_GB10B_LOG_COUNT; i++) {
		check("log bytes eq",
		      p10.log[i].pa == p20.log[i].pa && p10.log[i].size == p20.log[i].size &&
		      memcmp(buf10 + (p10.log[i].pa - a10.pa),
			     buf20 + (p20.log[i].pa - a20.pa), p10.log[i].size) == 0);
	}
	/* GSP_FMC_BOOT_PARAMS: same contents (points to wpr_meta.pa and libos_args.pa, both identical) */
	check("fmc_params bytes eq",
	      memcmp(buf10 + (p10.fmc_params.pa - a10.pa),
		     buf20 + (p20.fmc_params.pa - a20.pa), GSP_PAGE) == 0);
	/* Radix-3 region populated and identical */
	check("radix3 region set", p20.radix3.pa == p20.radix3_root_pa && p20.radix3.size == p10.radix3.size);
	check("radix3 bytes eq",
	      memcmp(buf10 + (p10.radix3.pa - a10.pa),
		     buf20 + (p20.radix3.pa - a20.pa), p10.radix3.size) == 0);

	/* WprMeta: exact GB20B fields */
	const uint8_t *wpr20 = buf20 + (p20.wpr_meta.pa - a20.pa);
	check("wpr magic", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, magic)) == GSP_WPR_META_MAGIC);
	check("wpr bl size", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, sizeOfBootloader)) == 212992u);
	check("wpr bl code 0xf000", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, bootloaderCodeOffset)) == 0xf000u);
	check("wpr bl data 0xa00", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, bootloaderDataOffset)) == 0xa00u);
	check("wpr sig size 4096", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, sizeOfSignature)) == 4096u);
	check("wpr heap 133M", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, gspFwHeapSize)) == GSP_GB10B_FW_HEAP_SIZE);
	check("wpr nonWpr 0", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, nonWprHeapSize)) == 0);
	check("wpr vga 0", rd64(wpr20 + offsetof(gsp_mirror_wpr_meta, vgaWorkspaceSize)) == 0);
	check("wpr pmu 0", rd32(wpr20 + offsetof(gsp_mirror_wpr_meta, pmuReservedSize)) == 0);

	/* COTv2 packet exact check */
	check("cot len 868", p20.cot_packet_len == 868u && p20.emem_queue_tail == 864u);
	check("cot mctp", rd32(p20.cot_packet + 0) == 0xc0000000u);
	check("cot nvdm", rd32(p20.cot_packet + 4) == 0x1410de7eu);
	check("cot ver2 sz860", rd32(p20.cot_packet + 8) == ((860u << 16) | 2u));
	check("cot fmc pa", rd64(p20.cot_packet + 12) == p20.fmc.pa);
	for (int k = 0; k < 6; k++)
		check("cot frts 0", rd32(p20.cot_packet + 20 + k * 4) == 0);
	check("cot hash", memcmp(p20.cot_packet + 44, h, 48) == 0);
	check("cot pkey", memcmp(p20.cot_packet + 92, pk, 384) == 0);
	check("cot sig", memcmp(p20.cot_packet + 476, s, 384) == 0);
	check("cot bootargs pa", rd64(p20.cot_packet + 860) == p20.fmc_params.pa);

	/* Negative controls on gb20b_boot_build */
	gsp_boot_in bad = in20;
	bad.bl_code_off = 0xb000u;
	check("reject gb10b bl_code_off", gsp_arena_init(&a20, buf20, 0x4000001000ull, arena_sz) == 0 &&
	      gb20b_boot_build(&a20, &bad, &p20, &why) != 0);

	bad = in20;
	bad.bootloader_size = GSP_GB10B_BOOTLOADER_SIZE;
	check("reject gb10b bl_size", gsp_arena_init(&a20, buf20, 0x4000001000ull, arena_sz) == 0 &&
	      gb20b_boot_build(&a20, &bad, &p20, &why) != 0);

	bad = in20;
	bad.fmc_size = 100000u;
	check("reject bad fmc_size", gsp_arena_init(&a20, buf20, 0x4000001000ull, arena_sz) == 0 &&
	      gb20b_boot_build(&a20, &bad, &p20, &why) != 0);

	bad = in20;
	bad.rm_sig_size = 2048u;
	check("reject bad sig_size", gsp_arena_init(&a20, buf20, 0x4000001000ull, arena_sz) == 0 &&
	      gb20b_boot_build(&a20, &bad, &p20, &why) != 0);

	free(buf10);
	free(buf20);
}

/* ---- 2. GB20B SEC2 + GSP MMIO simulator for om/boot.om ---- */
struct gb20b_sim {
	/* Inputs / fault injection */
	uint32_t scratch_delay, scratch_stuck_val;
	uint32_t busy_delay;
	int emem_write_broken;
	uint32_t msgq_delay;
	uint32_t reply_head, reply_tail;
	uint32_t reply_words[16];
	int emem_read_broken;
	uint32_t mask_delay;
	uint32_t hwcfg2_step8_ready;
	uint32_t locked_mb0_reads, locked_hwcfg2_reads;
	uint32_t lockdown_delay;
	uint32_t fmc_err;
	uint32_t cpuctl_val;
	/* Live state & observations */
	uint32_t ememc, emem_dw;
	uint32_t qhead, qtail, mhead, mtail;
	uint32_t qsent, mhead_reads, ack_tail_seen, ack_head_seen, ack_order_ok;
	uint32_t mmio_ops, pauses, os_written;
	uint32_t tx_words[256];
};

static void sim_init(struct gb20b_sim *s)
{
	memset(s, 0, sizeof(*s));
	s->scratch_stuck_val = 0x42u;
	s->reply_head = 0u;
	s->reply_tail = 16u; /* 20 bytes = 5 dwords */
	s->reply_words[0] = 0xc0000000u;
	s->reply_words[1] = 0x1510de7eu;
	s->reply_words[2] = 7u;   /* taskId */
	s->reply_words[3] = 0x14u; /* NVDM_TYPE_COT */
	s->reply_words[4] = 0u;    /* FSP_OK */
	s->cpuctl_val = 0x80u;     /* ACTIVE */
}

static uint32_t ememc_encode(uint32_t dw, uint32_t flags)
{
	return flags | ((dw / 64u) << 8) | ((dw % 64u) << 2);
}

static uint32_t sim_read(struct gb20b_sim *s, uint32_t off)
{
	s->mmio_ops++;
	switch (off) {
	case REG_SCRATCH:
		if (s->scratch_delay) {
			s->scratch_delay--;
			return s->scratch_stuck_val;
		}
		return 0xffu;
	case REG_SEC2_QHEAD0:
		return 0u;
	case REG_SEC2_QTAIL0:
		if (s->busy_delay) {
			s->busy_delay--;
			return 16u;
		}
		return 0u;
	case REG_SEC2_EMEMC0:
		return ememc_encode(s->emem_dw, s->ememc & 0x03000000u);
	case REG_SEC2_EMEMD0: {
		uint32_t idx = s->emem_dw;
		uint32_t v = idx < 16u ? s->reply_words[idx] : 0u;
		if ((s->ememc & 0x02000000u) && !s->emem_read_broken)
			s->emem_dw++;
		return v;
	}
	case REG_SEC2_MHEAD0:
		s->mhead_reads++;
		return s->mhead;
	case REG_SEC2_MTAIL0:
		if (s->msgq_delay) {
			s->msgq_delay--;
			return s->mhead;
		}
		return s->mtail;
	case REG_GSP_HWCFG2:
		if (s->mask_delay) {
			s->mask_delay--;
			return 0xbadf4100u;
		}
		if (!s->hwcfg2_step8_ready) {
			s->hwcfg2_step8_ready = 1;
			return 0x00002001u; /* step 7 mask released, lockdown still set */
		}
		if (s->locked_hwcfg2_reads) {
			s->locked_hwcfg2_reads--;
			return 0xbadf4100u;
		}
		if (s->lockdown_delay) {
			s->lockdown_delay--;
			return 0x00002001u; /* LOCKDOWN bit 13 set */
		}
		return 0x00000001u; /* unlocked */
	case REG_GSP_MB0:
		if (s->locked_mb0_reads) {
			s->locked_mb0_reads--;
			return 0xbadf4100u;
		}
		return s->fmc_err;
	case REG_GSP_MB1:
		return 0u;
	case REG_GSP_CPUCTL:
		return s->cpuctl_val;
	default:
		printf("PARITY-BOOT-GB20B FAIL unexpected read %#x\n", off);
		exit(1);
	}
}

static void sim_write(struct gb20b_sim *s, uint32_t off, uint32_t val)
{
	s->mmio_ops++;
	switch (off) {
	case REG_SEC2_EMEMC0:
		s->ememc = val;
		s->emem_dw = ((val >> 8) & 0xffu) * 64u + ((val >> 2) & 0x3fu);
		break;
	case REG_SEC2_EMEMD0:
		if (s->emem_dw < 256u)
			s->tx_words[s->emem_dw] = val;
		if ((s->ememc & 0x01000000u) && !s->emem_write_broken)
			s->emem_dw++;
		break;
	case REG_SEC2_QTAIL0:
		s->qtail = val;
		break;
	case REG_SEC2_QHEAD0:
		s->qhead = val;
		s->qsent++;
		/* Publish pending reply on MSGQ */
		s->mhead = s->reply_head;
		s->mtail = s->reply_tail;
		break;
	case REG_SEC2_MTAIL0:
		s->mtail = val;
		s->ack_tail_seen = 1;
		if (!s->ack_head_seen)
			s->ack_order_ok = 1;
		break;
	case REG_SEC2_MHEAD0:
		s->mhead = val;
		s->ack_head_seen = 1;
		break;
	case REG_GSP_OS:
		s->os_written = val;
		break;
	default:
		printf("PARITY-BOOT-GB20B FAIL unexpected write %#x=%#x\n", off, val);
		exit(1);
	}
}

static void make_good_cot2(uint8_t pkt[1024])
{
	memset(pkt, 0, 1024);
	wr32(pkt + 0, 0xc0000000u);
	wr32(pkt + 4, 0x1410de7eu);
	wr32(pkt + 8, (860u << 16) | 2u);
	wr32(pkt + 12, 0x12340000u);
	wr32(pkt + 860, 0x56780000u);
}

static uint64_t run_boot(struct gb20b_sim *s, uint8_t pkt[1024], uint32_t len, uint32_t profile,
			 uint32_t budget, uint64_t st[32])
{
	uint64_t act[4] = { 0 }, obs = 0, trap = 0, r;
	unsigned cap = 20000;

	memset(st, 0, 32 * sizeof(uint64_t));
	st[1] = len;
	st[2] = 0x0000004012345000ull;
	st[3] = 0x42u;
	st[4] = st[5] = st[6] = st[7] = budget;
	st[25] = profile;
	for (;;) {
		trap = 0;
		r = om_call(agpu_om_boot, P(st), P(pkt), obs, P(act), 0, 0, &trap);
		check("boot no trap", trap == 0);
		if (r == 1)
			return st[12];
		check("boot r==0 and cap", r == 0 && cap-- > 0);
		obs = 0;
		if (act[0] == 1)
			obs = sim_read(s, (uint32_t)act[1]);
		else if (act[0] == 2)
			sim_write(s, (uint32_t)act[1], (uint32_t)act[2]);
		else if (act[0] == 3)
			s->pauses++;
		else
			check("valid action", 0);
	}
}

static void test_mmio_gb20b(void)
{
	uint8_t pkt[1024];
	uint64_t st[32];
	struct gb20b_sim sim;

	/* 1. Happy path with delays across every wait */
	make_good_cot2(pkt);
	sim_init(&sim);
	sim.scratch_delay = 3;
	sim.busy_delay = 2;
	sim.msgq_delay = 4;
	sim.mask_delay = 2;
	sim.lockdown_delay = 3;
	check("happy stage 0", run_boot(&sim, pkt, 868, 1, 10, st) == 0);
	check("happy ack tail before head", sim.ack_order_ok == 1 && sim.ack_tail_seen && sim.ack_head_seen);
	check("happy msgq emptied", sim.mhead == sim.mtail);
	check("happy taskId 7", st[20] == 7u && st[19] == 0u);
	check("happy dwords 217", st[17] == 217u && sim.qtail == 864u && sim.qhead == 0u);
	check("happy os written", sim.os_written == 0x42u && (st[24] & 0x80u));

	check("happy read msgq_head", sim.mhead_reads > 0);

	/* 2. Happy path with MCTP IC bit (bit 7) set and nonzero MSGQ head (4..20) */
	sim_init(&sim);
	sim.reply_words[1] = 0x1510defeu;
	sim.reply_head = 4u;
	sim.reply_tail = 20u;
	check("ic=1 nonzero mhead stage 0", run_boot(&sim, pkt, 868, 1, 10, st) == 0);
	check("nonzero mhead acked to 4", sim.mhead == 4u && sim.mtail == 4u && sim.mhead_reads > 0);

	/* 3. Bad profile & COTv2 negative controls -> stage=1, zero MMIO */
	sim_init(&sim);
	check("bad profile 2", run_boot(&sim, pkt, 868, 2, 10, st) == 1 && sim.mmio_ops == 0);

	sim_init(&sim);
	check("bad len 864", run_boot(&sim, pkt, 864, 1, 10, st) == 1 && sim.mmio_ops == 0);

	make_good_cot2(pkt); wr32(pkt + 0, 1u); /* GB10B MCTP=1 must fail GB20B */
	sim_init(&sim);
	check("bad mctp", run_boot(&sim, pkt, 868, 1, 10, st) == 1 && sim.mmio_ops == 0);

	make_good_cot2(pkt); wr32(pkt + 4, 0x1510de7eu);
	sim_init(&sim);
	check("bad nvdm", run_boot(&sim, pkt, 868, 1, 10, st) == 1 && sim.mmio_ops == 0);

	make_good_cot2(pkt); wr32(pkt + 8, (860u << 16) | 1u); /* COTv1 must fail GB20B */
	sim_init(&sim);
	check("bad cot v1", run_boot(&sim, pkt, 868, 1, 10, st) == 1 && sim.mmio_ops == 0);

	for (int k = 0; k < 6; k++) {
		make_good_cot2(pkt);
		wr32(pkt + 20 + k * 4, 0x1000u);
		sim_init(&sim);
		check("nonzero frts rejected", run_boot(&sim, pkt, 868, 1, 10, st) == 1 && sim.mmio_ops == 0);
	}

	/* 4. FWSEC scratch stuck -> stage=4, st[19]=0x42, no queue send */
	make_good_cot2(pkt);
	sim_init(&sim);
	sim.scratch_delay = 50;
	check("scratch timeout", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 0x42u && sim.qsent == 0);

	/* 5. SEC2 queue busy -> stage=2; EMEM write broken -> stage=3 */
	sim_init(&sim);
	sim.busy_delay = 50;
	check("busy stage 2", run_boot(&sim, pkt, 868, 1, 5, st) == 2 && sim.qsent == 0);

	sim_init(&sim);
	sim.emem_write_broken = 1;
	check("emem write stage 3", run_boot(&sim, pkt, 868, 1, 5, st) == 3 && sim.qsent == 0);

	/* 6. MSGQ response failure modes -> stage=4 with distinct diagnostics */
	sim_init(&sim);
	sim.msgq_delay = 50;
	check("msgq timeout", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 1000u);

	sim_init(&sim);
	sim.reply_tail = 12u; /* 16 bytes < 20 */
	check("short reply", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 1001u);

	sim_init(&sim);
	sim.reply_tail = 1024u; /* > 1020 */
	check("oversize reply", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 1001u);

	sim_init(&sim);
	sim.emem_read_broken = 1;
	check("emem read skew acks then fails",
	      run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 1002u && sim.ack_order_ok == 1);

	sim_init(&sim);
	sim.reply_words[0] = 0x80000000u; /* SOM only, missing EOM */
	check("fragmented reply", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 1003u);

	sim_init(&sim);
	sim.reply_words[1] = 0x1610de7eu; /* wrong NVDM type */
	check("bad nvdm reply", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 1004u);

	sim_init(&sim);
	sim.reply_words[3] = 0x13u; /* wrong commandNvdmType */
	check("bad cmd reply", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 1005u);

	sim_init(&sim);
	sim.reply_words[4] = 0xbad1u; /* nonzero FSP errorCode */
	check("nonzero fsp err", run_boot(&sim, pkt, 868, 1, 5, st) == 4 && st[19] == 0xbad1u);

	/* 7. Step 8 0xbadf4100 PRI target-locked guard */
	sim_init(&sim);
	sim.locked_mb0_reads = 2;
	sim.locked_hwcfg2_reads = 2;
	check("transient 0xbadf4100 recovers", run_boot(&sim, pkt, 868, 1, 10, st) == 0);

	sim_init(&sim);
	sim.locked_mb0_reads = 50;
	check("persistent mb0 0xbadf4100 -> stage 6 not 7",
	      run_boot(&sim, pkt, 868, 1, 5, st) == 6 && st[22] == 0xbadf4100u);

	sim_init(&sim);
	sim.locked_hwcfg2_reads = 50;
	check("persistent hwcfg2 0xbadf4100 -> stage 6 not 7",
	      run_boot(&sim, pkt, 868, 1, 5, st) == 6 && st[21] == 0xbadf4100u);

	/* 8. Real FMC error -> stage=7; dead RISC-V -> stage=8 */
	sim_init(&sim);
	sim.fmc_err = 0xdead0001u;
	check("real fmc error stage 7", run_boot(&sim, pkt, 868, 1, 5, st) == 7 && st[22] == 0xdead0001u);

	sim_init(&sim);
	sim.cpuctl_val = 0u;
	check("dead riscv stage 8", run_boot(&sim, pkt, 868, 1, 5, st) == 8);
}

int main(void)
{
	test_builder_parity();
	test_mmio_gb20b();
	printf("PARITY-BOOT-GB20B PASS checks=%u\n", checks);
	return 0;
}
