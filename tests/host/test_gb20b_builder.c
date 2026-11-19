// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* test_gb20b_builder -- direct host tests and negative controls for tools/gb20b_gsp_boot.c.
 * Exercises gsp_boot_build_gb20b()'s own size, descriptor-offset, COTv2/FRTS, and CrashCat checks
 * (which aien-gpu-boot's fail-fast load() size checks intercept before the builder runs).
 * Host only: plain memory, no device touched. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb20b_gsp_boot.h"

static unsigned checks;

static void check(const char *what, int cond)
{
	checks++;
	if (!cond) {
		printf("TEST-GB20B-BUILDER FAIL %s\n", what);
		exit(1);
	}
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

int main(void)
{
	static uint8_t elf[16384], sig[4096], bl[GSP_GB20B_BOOTLOADER_SIZE], fmc[GSP_GB20B_FMC_SIZE];
	static uint8_t h[GSP_COT_HASH_SIZE], pk[GSP_COT_KEY_SIZE], s[GSP_COT_SIG_SIZE], si[GSP_SYSTEM_INFO_SIZE];
	size_t cap = 4u << 20;
	uint8_t *buf = aligned_alloc(GSP_PAGE, cap);
	gsp_arena arena;
	gsp_boot_in in = { 0 }, bad;
	gsp_boot_plan plan;
	gsp_region crash = { 0 };
	const char *why = NULL;

	check("alloc arena", buf != NULL);
	memset(buf, 0, cap);
	for (size_t i = 0; i < sizeof(elf); i++) elf[i] = (uint8_t)(i * 3u + 1u);
	for (size_t i = 0; i < sizeof(sig); i++) sig[i] = (uint8_t)(i * 5u + 2u);
	for (size_t i = 0; i < sizeof(bl); i++) bl[i] = (uint8_t)(i * 7u + 3u);
	for (size_t i = 0; i < sizeof(fmc); i++) fmc[i] = (uint8_t)(i * 11u + 4u);
	for (size_t i = 0; i < sizeof(h); i++) h[i] = (uint8_t)(0x40u + i);
	for (size_t i = 0; i < sizeof(pk); i++) pk[i] = (uint8_t)(0x80u + i);
	for (size_t i = 0; i < sizeof(s); i++) s[i] = (uint8_t)(0xc0u + i);
	for (size_t i = 0; i < sizeof(si); i++) si[i] = (uint8_t)(i ^ 0x5au);

	in.rm_elf = elf; in.rm_elf_size = sizeof(elf);
	in.rm_sig = sig; in.rm_sig_size = sizeof(sig);
	in.bootloader = bl; in.bootloader_size = GSP_GB20B_BOOTLOADER_SIZE;
	in.bl_code_off = GSP_GB20B_BL_CODE_OFF;
	in.bl_data_off = GSP_GB20B_BL_DATA_OFF;
	in.bl_manifest_off = GSP_GB20B_BL_MANIFEST_OFF;
	in.fmc = fmc; in.fmc_size = GSP_GB20B_FMC_SIZE;
	in.fmc_hash = h; in.fmc_pubkey = pk; in.fmc_sig = s;
	in.sysmem_heap_pa = 0x5000000000ull; in.sysmem_heap_size = 512ull << 20;
	in.system_info = si; in.dma_bits = 40;

	/* 1. Valid GB20B build */
	check("arena init", gsp_arena_init(&arena, buf, 0x4000001000ull, cap) == 0);
	check("build valid gb20b", gsp_boot_build_gb20b(&arena, &in, &plan, &crash, &why) == 0);
	check("cot len 868", plan.cot_packet_len == 868u && plan.emem_queue_tail == 864u);
	check("cot version 2", (plan.cot_packet[8] | (plan.cot_packet[9] << 8)) == GSP_GB20B_COT_VERSION);
	check("cot size 860", (plan.cot_packet[10] | (plan.cot_packet[11] << 8)) == GSP_COT_PAYLOAD_SIZE);
	for (unsigned k = 0; k < 24; k++)
		check("cot frts zero", plan.cot_packet[8 + 12 + k] == 0);
	const uint8_t *wpr = buf + (plan.wpr_meta.pa - arena.pa);
	check("wpr bl size", rd64(wpr + offsetof(gsp_mirror_wpr_meta, sizeOfBootloader)) == GSP_GB20B_BOOTLOADER_SIZE);
	check("wpr bl code 0xf000", rd64(wpr + offsetof(gsp_mirror_wpr_meta, bootloaderCodeOffset)) == GSP_GB20B_BL_CODE_OFF);
	check("wpr fw heap 133M", rd64(wpr + offsetof(gsp_mirror_wpr_meta, gspFwHeapSize)) == GSP_GB10B_FW_HEAP_SIZE);
	check("wpr crash addr", crash.pa != 0 && (crash.pa & (GSP_PAGE - 1)) == 0 &&
	      rd64(wpr + 224) == crash.pa);
	check("wpr crash size 4096", crash.size == GSP_GB20B_CRASH_QUEUE_SIZE &&
	      rd32(wpr + 232) == GSP_GB20B_CRASH_QUEUE_SIZE);

	/* 2. Negative controls directly reaching gsp_boot_build_gb20b() */
	bad = in; bad.bootloader_size = 196608u; /* GB10B bootloader size */
	check("arena reset 1", gsp_arena_init(&arena, buf, 0x4000001000ull, cap) == 0);
	check("refuse gb10b bootloader size",
	      gsp_boot_build_gb20b(&arena, &bad, &plan, &crash, &why) != 0 && why && strstr(why, "GB20B GspRmBoot"));

	bad = in; bad.fmc_size = 195912u; /* GB10B FMC size */
	check("arena reset 2", gsp_arena_init(&arena, buf, 0x4000001000ull, cap) == 0);
	check("refuse gb10b fmc size",
	      gsp_boot_build_gb20b(&arena, &bad, &plan, &crash, &why) != 0 && why && strstr(why, "GB20B image size"));

	bad = in; bad.bl_code_off = 0xb000u; /* GB10B monCodeOff with GB20B file sizes */
	check("arena reset 3", gsp_arena_init(&arena, buf, 0x4000001000ull, cap) == 0);
	check("refuse wrong bl_code_off",
	      gsp_boot_build_gb20b(&arena, &bad, &plan, &crash, &why) != 0 && why && strstr(why, "descriptor offsets"));

	bad = in; bad.bl_data_off = 0xb00u;
	check("arena reset 4", gsp_arena_init(&arena, buf, 0x4000001000ull, cap) == 0);
	check("refuse wrong bl_data_off",
	      gsp_boot_build_gb20b(&arena, &bad, &plan, &crash, &why) != 0 && why && strstr(why, "descriptor offsets"));

	bad = in; bad.bl_manifest_off = 0x100u;
	check("arena reset 5", gsp_arena_init(&arena, buf, 0x4000001000ull, cap) == 0);
	check("refuse wrong bl_manifest_off",
	      gsp_boot_build_gb20b(&arena, &bad, &plan, &crash, &why) != 0 && why && strstr(why, "descriptor offsets"));

	free(buf);
	printf("TEST-GB20B-BUILDER PASS checks=%u controls=5\n", checks);
	return 0;
}
