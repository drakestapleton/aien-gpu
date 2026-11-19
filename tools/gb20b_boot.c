/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* gb20b_boot.c -- GB20B (Spark 0x1b/0xb) GSP boot arena + COTv2 packet builder.
 * Mirrors build/oracle/native/kernel/dev/gb10_gsp_boot.c using the exact gsp_mirror_* offsets
 * verified against NVIDIA 580.173.02 SDK headers, with the source-verified GB20B differences:
 *   - GspRmBoot_GB20B: 212992 B, monCodeOff=0xf000, monDataOff=0xa00, manifestOff=0x0
 *   - GspRmFmcGfwProdSigned_GB20B: 212296 B, 48B hash / 384B key / 384B sig
 *   - .fwsignature_gb20y: 4096 B
 *   - WprMeta: shared kgspPopulateWprMeta_GB10B (vga/pmu/nonWprHeap = 0)
 *   - SEC2 COT packet: MCTP 0xc0000000, NVDM 0x1410de7e, version=2, size=860, frts*=0
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "gb10_gsp_boot.h"

#define GSP_GB20B_BOOTLOADER_SIZE 212992u
#define GSP_GB20B_BL_CODE_OFF     0xf000u
#define GSP_GB20B_BL_DATA_OFF     0xa00u
#define GSP_GB20B_BL_MANIFEST_OFF 0x0u
#define GSP_GB20B_FMC_SIZE        212296u
#define GSP_GB20B_COT_VERSION     2u

_Static_assert(sizeof(gsp_mirror_wpr_meta) == GSP_WPR_META_SIZE, "GspFwWprMeta is 256 bytes");
_Static_assert(sizeof(gsp_mirror_libos_arg) == GSP_LIBOS_ENTRY_SIZE, "LibosMemoryRegionInitArgument is 32");
_Static_assert(sizeof(gsp_mirror_args_cached) == GSP_ARGS_CACHED_SIZE, "GSP_ARGUMENTS_CACHED is 88");
_Static_assert(sizeof(gsp_mirror_msgq_tx) == 32, "msgqTxHeader is 32");
_Static_assert(sizeof(gsp_mirror_fmc_params) == GSP_FMC_PARAMS_SIZE, "GSP_FMC_BOOT_PARAMS is 88");
_Static_assert(sizeof(gsp_mirror_cot) == GSP_COT_PAYLOAD_SIZE, "NVDM_PAYLOAD_COT is 860 packed");
_Static_assert(offsetof(gsp_mirror_cot, gspBootArgsSysmemOffset) == 852, "COT boot args at 852");

#define OFF(T, f) ((uint32_t)offsetof(T, f))
#define WM(f)     OFF(gsp_mirror_wpr_meta, f)
#define TXH(f)    OFF(gsp_mirror_msgq_tx, f)
#define FP(f)     OFF(gsp_mirror_fmc_params, f)
#define CO(f)     OFF(gsp_mirror_cot, f)

static const struct { const char *id; uint32_t size; } gb20b_logs[GSP_GB10B_LOG_COUNT] = {
	{"LOGINIT", 0x10000}, {"LOGINTR", 0x10000}, {"LOGRM", 0x10000}, {"LOGMNOC", 0x10000},
	{"LOGDBG", 0x10000},  {"LOGROOT", 0x1000},  {"LOGKRNL", 0x10000},
};

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) { p[i] = (uint8_t)v; v >>= 8; } }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; } }
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static uint8_t *take(gsp_arena *a, uint64_t size, uint64_t *pa)
{
	uint64_t off = up(a->used, GSP_PAGE);
	uint64_t sz = up(size, GSP_PAGE);

	if (off < a->used || sz < size || off + sz < off || off + sz > a->cap)
		return NULL;
	a->used = off + sz;
	memset(a->base + off, 0, sz);
	*pa = a->pa + off;
	return a->base + off;
}

static int fits_dma(uint64_t pa, uint64_t len, uint32_t dma_bits)
{
	if (dma_bits >= 64)
		return pa + len >= pa;
	uint64_t lim = 1ULL << dma_bits;

	return pa < lim && len <= lim - pa;
}

#define FAIL(s) do { *why = (s); return -1; } while (0)

int gb20b_boot_build(gsp_arena *a, const gsp_boot_in *in, gsp_boot_plan *out, const char **why)
{
	*why = NULL;
	if (!a || !in || !out)
		FAIL("null argument");
	if (!in->rm_elf || !in->rm_elf_size)
		FAIL("GSP-RM ELF missing");
	if (!in->rm_sig || in->rm_sig_size != 4096u)
		FAIL("GSP-RM gb20y signature missing or not 4096 B");
	if (!in->bootloader || in->bootloader_size != GSP_GB20B_BOOTLOADER_SIZE)
		FAIL("bootloader is not the GB20B GspRmBoot image size (212992 B)");
	if (in->bl_code_off != GSP_GB20B_BL_CODE_OFF ||
	    in->bl_data_off != GSP_GB20B_BL_DATA_OFF ||
	    in->bl_manifest_off != GSP_GB20B_BL_MANIFEST_OFF)
		FAIL("bootloader descriptor offsets are not the GB20B values (0xf000/0xa00/0x0)");
	if (!in->fmc || in->fmc_size != GSP_GB20B_FMC_SIZE)
		FAIL("GSP-FMC image is not the GB20B size (212296 B)");
	if (!in->fmc_hash || !in->fmc_pubkey || !in->fmc_sig)
		FAIL("GSP-FMC hash, key or signature missing");
	if (!in->sysmem_heap_pa || (in->sysmem_heap_pa & (GSP_PAGE - 1)) ||
	    !in->sysmem_heap_size || (in->sysmem_heap_size & (GSP_PAGE - 1)))
		FAIL("sysmem heap zero or not 4 KiB aligned");
	if (!in->system_info)
		FAIL("GspSystemInfo missing");
	if (in->nregs)
		FAIL("GB20B builder expects nregs=0 in side-project path");
	if (in->dma_bits == 0 || in->dma_bits > 64)
		FAIL("dma_bits out of range");
	if (a->used)
		FAIL("arena not empty");

	memset(out, 0, sizeof(*out));
	uint64_t pa;
	uint8_t *p;

	/* 1. WprMeta page */
	uint8_t *wpr = take(a, GSP_PAGE, &pa);
	if (!wpr)
		FAIL("arena too small");
	out->wpr_meta = (gsp_region){pa, GSP_WPR_META_SIZE};

	/* 2. libos init-argument page */
	uint8_t *libos = take(a, GSP_PAGE, &pa);
	if (!libos)
		FAIL("arena too small");
	out->libos_args = (gsp_region){pa, GSP_PAGE};

	/* 3. 7 log buffers */
	for (uint32_t i = 0; i < GSP_GB10B_LOG_COUNT; i++) {
		uint32_t sz = gb20b_logs[i].size;

		p = take(a, sz, &pa);
		if (!p)
			FAIL("arena too small");
		for (uint32_t k = 0; k < sz / GSP_PAGE; k++)
			wr64(p + 8 + 8 * k, pa + (uint64_t)k * GSP_PAGE);
		out->log[i] = (gsp_region){pa, sz};
	}

	/* 4. GSP_ARGUMENTS_CACHED page */
	uint8_t *args = take(a, GSP_PAGE, &pa);
	if (!args)
		FAIL("arena too small");
	out->args_cached = (gsp_region){pa, GSP_PAGE};

	/* 5. RPC shared buffer (129 pages) */
	uint8_t *shared = take(a, GSP_MQ_SHARED_SIZE, &pa);
	if (!shared)
		FAIL("arena too small");
	out->rpc_shared = (gsp_region){pa, GSP_MQ_SHARED_SIZE};
	for (uint32_t k = 0; k < GSP_MQ_NUM_PTES; k++)
		wr64(shared + 8 * k, pa + (uint64_t)k * GSP_PAGE);
	out->cmdq_pa = pa + GSP_MQ_CMDQ_OFF;
	out->statq_pa = pa + GSP_MQ_STATQ_OFF;
	uint8_t *cmdq = shared + GSP_MQ_CMDQ_OFF;
	wr32(cmdq + TXH(version), 0);
	wr32(cmdq + TXH(size), GSP_MQ_QUEUE_SIZE);
	wr32(cmdq + TXH(msgSize), GSP_MQ_MSG_SIZE);
	wr32(cmdq + TXH(msgCount), GSP_MQ_MSG_COUNT);
	wr32(cmdq + TXH(writePtr), 0);
	wr32(cmdq + TXH(flags), GSP_MQ_FLAGS_SWAP_RX);
	wr32(cmdq + TXH(rxHdrOff), GSP_MQ_RXHDR_OFF);
	wr32(cmdq + TXH(entryOff), GSP_MQ_ENTRY_OFF);

	/* libos entries: 7 logs + RMARGS */
	for (uint32_t i = 0; i <= GSP_GB10B_LOG_COUNT; i++) {
		uint8_t *e = libos + i * GSP_LIBOS_ENTRY_SIZE;
		int rm = (i == GSP_GB10B_LOG_COUNT);

		wr64(e + OFF(gsp_mirror_libos_arg, id8), gsp_init_arg_id(rm ? "RMARGS" : gb20b_logs[i].id));
		wr64(e + OFF(gsp_mirror_libos_arg, pa), rm ? out->args_cached.pa : out->log[i].pa);
		wr64(e + OFF(gsp_mirror_libos_arg, size), rm ? GSP_PAGE : out->log[i].size);
		e[OFF(gsp_mirror_libos_arg, kind)] = GSP_LIBOS_KIND_CONTIGUOUS;
		e[OFF(gsp_mirror_libos_arg, loc)] = GSP_LIBOS_LOC_SYSMEM;
	}

	/* GSP_ARGUMENTS_CACHED (exact gsp_mirror_args_cached offsets) */
	wr64(args + OFF(gsp_mirror_args_cached, mq.sharedMemPhysAddr), out->rpc_shared.pa);
	wr32(args + OFF(gsp_mirror_args_cached, mq.pageTableEntryCount), GSP_MQ_NUM_PTES);
	wr64(args + OFF(gsp_mirror_args_cached, mq.cmdQueueOffset), GSP_MQ_CMDQ_OFF);
	wr64(args + OFF(gsp_mirror_args_cached, mq.statQueueOffset), GSP_MQ_STATQ_OFF);
	wr32(args + OFF(gsp_mirror_args_cached, gpuInstance), in->gpu_instance);
	args[OFF(gsp_mirror_args_cached, bDmemStack)] = 1;
	wr64(args + OFF(gsp_mirror_args_cached, sysmemHeapArgs.pa), in->sysmem_heap_pa);
	wr64(args + OFF(gsp_mirror_args_cached, sysmemHeapArgs.size), in->sysmem_heap_size);

	/* 6. Radix-3 ELF tree */
	uint64_t np[4], lvl_off[4] = {0, 0, 0, 0}, n = 0;
	np[3] = (in->rm_elf_size + GSP_PAGE - 1) / GSP_PAGE;
	for (int i = 3; i > 0; i--)
		np[i - 1] = ((np[i] - 1) >> 9) + 1;
	if (np[0] != 1)
		FAIL("GSP-RM ELF exceeds radix-3 single root");
	for (int i = 1; i < 4; i++) {
		n += np[i - 1];
		lvl_off[i] = n * GSP_PAGE;
	}
	uint64_t r3_size = (n + np[3]) * GSP_PAGE;
	uint8_t *rad = take(a, r3_size, &pa);
	if (!rad)
		FAIL("arena too small for radix-3 ELF");
	for (int lv = 0; lv < 3; lv++) {
		for (uint64_t k = 0; k < np[lv + 1]; k++)
			wr64(rad + lvl_off[lv] + k * 8, pa + lvl_off[lv + 1] + k * GSP_PAGE);
	}
	memcpy(rad + lvl_off[3], in->rm_elf, in->rm_elf_size);
	out->radix3 = (gsp_region){pa, r3_size};
	out->radix3_root_pa = pa;
	out->elf_data_pa = pa + lvl_off[3];
	for (int i = 0; i < 4; i++)
		out->radix3_level_pages[i] = (uint32_t)np[i];

	/* 7. GB20B bootloader (212992 B), 8. .fwsignature_gb20y (4096 B), 9. GB20B FMC (212296 B) */
	p = take(a, in->bootloader_size, &pa);
	if (!p)
		FAIL("arena too small");
	memcpy(p, in->bootloader, in->bootloader_size);
	out->bootloader = (gsp_region){pa, in->bootloader_size};

	uint64_t sig_sz = up(in->rm_sig_size, 256);
	p = take(a, sig_sz, &pa);
	if (!p)
		FAIL("arena too small");
	memcpy(p, in->rm_sig, in->rm_sig_size);
	out->signature = (gsp_region){pa, sig_sz};

	p = take(a, in->fmc_size, &pa);
	if (!p)
		FAIL("arena too small");
	memcpy(p, in->fmc, in->fmc_size);
	out->fmc = (gsp_region){pa, in->fmc_size};

	/* 10. GSP_FMC_BOOT_PARAMS (exact gsp_mirror_fmc_params offsets) */
	uint8_t *fp = take(a, GSP_PAGE, &pa);
	if (!fp)
		FAIL("arena too small");
	out->fmc_params = (gsp_region){pa, GSP_FMC_PARAMS_SIZE};
	wr32(fp + FP(regkeys), in->fmc_regkeys);
	wr32(fp + FP(boot.target), GSP_DMA_TARGET_COHERENT);
	wr32(fp + FP(boot.gspRmDescSize), GSP_WPR_META_SIZE);
	wr64(fp + FP(boot.gspRmDescOffset), out->wpr_meta.pa);
	fp[FP(boot.bIsGspRmBoot)] = 1;
	wr32(fp + FP(rm.target), GSP_DMA_TARGET_NONCOHERENT);
	wr64(fp + FP(rm.bootArgsOffset), out->libos_args.pa);

	/* WprMeta (shared kgspPopulateWprMeta_GB10B offsets, GB20B monCodeOff=0xf000) */
	wr64(wpr + WM(magic), GSP_WPR_META_MAGIC);
	wr64(wpr + WM(revision), GSP_WPR_META_REVISION);
	wr64(wpr + WM(sysmemAddrOfRadix3Elf), out->radix3_root_pa);
	wr64(wpr + WM(sizeOfRadix3Elf), in->rm_elf_size);
	wr64(wpr + WM(sysmemAddrOfBootloader), out->bootloader.pa);
	wr64(wpr + WM(sizeOfBootloader), in->bootloader_size);
	wr64(wpr + WM(bootloaderCodeOffset), in->bl_code_off);
	wr64(wpr + WM(bootloaderDataOffset), in->bl_data_off);
	wr64(wpr + WM(bootloaderManifestOffset), in->bl_manifest_off);
	wr64(wpr + WM(sysmemAddrOfSignature), out->signature.pa);
	wr64(wpr + WM(sizeOfSignature), sig_sz);
	wr64(wpr + WM(gspFwHeapSize), GSP_GB10B_FW_HEAP_SIZE);

	if (!fits_dma(a->pa, a->used, in->dma_bits) ||
	    !fits_dma(in->sysmem_heap_pa, in->sysmem_heap_size, in->dma_bits))
		FAIL("address beyond the DMA window");

	/* Pre-boot SET_SYSTEM_INFO RPC */
	gsp_mq q = { .cmdq = cmdq, .statq = shared + GSP_MQ_STATQ_OFF, .tx_seq = 0, .rx_seq = 0, .rpc_seq = 0 };
	gsp_seg si = { in->system_info, GSP_SYSTEM_INFO_SIZE };
	if (gsp_mq_send(&q, GSP_FN_SET_SYSTEM_INFO, &si, 1, GSP_SYSTEM_INFO_SIZE, 0, 0, why))
		return -1;
	out->tx_seq = q.tx_seq;
	out->cmdq_write_ptr = rd32(cmdq + TXH(writePtr));

	/* GB20B SEC2 COTv2 packet (8 + 860 = 868 B):
	 *   MCTP 0xc0000000 (SOM|EOM, SEID=0, SEQ=0)
	 *   NVDM 0x1410de7e (type 0x7e, PCI vendor 0x10de, NVDM_TYPE_COT 0x14)
	 *   NVDM_PAYLOAD_COT: version=2, size=860, gspFmcSysmemOffset, frts*=0,
	 *                     hash384 (48 B), publicKey (384 B), signature (384 B),
	 *                     gspBootArgsSysmemOffset (@852).
	 */
	uint8_t *pk = out->cot_packet;
	memset(pk, 0, sizeof(out->cot_packet));
	wr32(pk + 0, (1u << 31) | (1u << 30));
	wr32(pk + 4, 0x7eu | (0x10deu << 8) | (0x14u << 24));
	uint8_t *cot = pk + 8;
	wr16(cot + CO(version), GSP_GB20B_COT_VERSION);
	wr16(cot + CO(size), GSP_COT_PAYLOAD_SIZE);
	wr64(cot + CO(gspFmcSysmemOffset), out->fmc.pa);
	/* frtsSysmemOffset/Size and frtsVidmemOffset/Size stay 0 on GB20B */
	memcpy(cot + CO(hash384), in->fmc_hash, GSP_COT_HASH_SIZE);
	memcpy(cot + CO(publicKey), in->fmc_pubkey, GSP_COT_KEY_SIZE);
	memcpy(cot + CO(signature), in->fmc_sig, GSP_COT_SIG_SIZE);
	wr64(cot + CO(gspBootArgsSysmemOffset), out->fmc_params.pa);

	out->cot_packet_len = 8 + GSP_COT_PAYLOAD_SIZE;
	out->emem_queue_tail = (uint32_t)up(out->cot_packet_len, 4) - 4;
	return 0;
}
