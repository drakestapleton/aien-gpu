// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* gb20b_gsp_boot.c -- the GSP boot package for GB20B (see gb20b_gsp_boot.h).
 *
 * The aienux builder is compiled into this unit with GB20B's boot-loader geometry and COT version in
 * place of GB10B's, then two GB20B differences it has no input for are applied to its result: the
 * FRTS fields of the COT payload are zero, and GspFwWprMeta names a CrashCat queue. Everything else
 * is the oracle's code unchanged, so the layout tests of that repository still describe it.
 * Transitional host code: the Omega builder for both profiles is omega-gpu-driver gsp/arena.om. */
#include "gb20b_gsp_boot.h"

#undef GSP_GB10B_BOOTLOADER_SIZE
#define GSP_GB10B_BOOTLOADER_SIZE GSP_GB20B_BOOTLOADER_SIZE
#undef GSP_GB10B_BL_CODE_OFF
#define GSP_GB10B_BL_CODE_OFF GSP_GB20B_BL_CODE_OFF
#undef GSP_GB10B_BL_DATA_OFF
#define GSP_GB10B_BL_DATA_OFF GSP_GB20B_BL_DATA_OFF
#undef GSP_GB10B_BL_MANIFEST_OFF
#define GSP_GB10B_BL_MANIFEST_OFF GSP_GB20B_BL_MANIFEST_OFF
#undef GSP_COT_VERSION
#define GSP_COT_VERSION GSP_GB20B_COT_VERSION

#include "gb10_gsp_boot.c"

/* GspFwWprMeta's second union arm (gsp_fw_wpr_meta.h:171-185): NvU32 partitionRpcPadding[4], then
 * NvU64 sysmemAddrOfCrashReportQueue and NvU32 sizeOfCrashReportQueue over elfDataOffset..elfDataSize. */
_Static_assert(offsetof(gsp_mirror_wpr_meta, partitionRpcAddr) == 208, "WprMeta union at 208");
_Static_assert(offsetof(gsp_mirror_wpr_meta, elfDataOffset) == 224, "sysmemAddrOfCrashReportQueue at 224");
_Static_assert(offsetof(gsp_mirror_wpr_meta, elfDataSize) == 232, "sizeOfCrashReportQueue at 232");
_Static_assert(offsetof(gsp_mirror_cot, frtsSysmemOffset) == 12 && offsetof(gsp_mirror_cot, frtsVidmemSize) == 32,
	       "COT FRTS fields are payload bytes 12..35");
/* Shared GB10B|GB20B HALs verified in open-gpu-kernel-modules 580.173.02 generated/g_kernel_gsp_nvoc.c:
 * kgspGetMinWprHeapSizeMB_647ce6, kgspGetMaxWprHeapSizeMB_aa578c, kgspGetFwHeapParamOsCarveoutSize,
 * kgspPopulateWprMeta_GB10B, kgspGetLogCount_520858. Assert the inherited macros explicitly. */
_Static_assert(GSP_GB10B_FW_HEAP_SIZE == (133ULL << 20), "GB20B WPR FW heap size is 133 MiB (139460608 B)");
_Static_assert(GSP_GB10B_LOG_COUNT == 7u, "GB20B libos log buffer count is 7");

int gsp_boot_build_gb20b(gsp_arena *a, const gsp_boot_in *in, gsp_boot_plan *out, gsp_region *crash,
			 const char **why)
{
	uint8_t *cot, *wpr, *q;
	uint64_t pa;

	*why = 0;
	if (!a || !in || !out || !crash)
		FAIL("null argument");
	/* said in GB20B's words before the shared builder can say it in GB10B's */
	if (!in->bootloader || in->bootloader_size != GSP_GB20B_BOOTLOADER_SIZE)
		FAIL("bootloader is not the GB20B GspRmBoot image size");
	if (in->bl_code_off != GSP_GB20B_BL_CODE_OFF || in->bl_data_off != GSP_GB20B_BL_DATA_OFF ||
	    in->bl_manifest_off != GSP_GB20B_BL_MANIFEST_OFF)
		FAIL("bootloader descriptor offsets are not the GB20B values");
	if (!in->fmc || in->fmc_size != GSP_GB20B_FMC_SIZE)
		FAIL("GSP-FMC is not the GB20B image size");
	if (gsp_boot_build(a, in, out, why))
		return -1;

	/* COT version 2 leaves the FRTS fields zero: frtsSysmemOffset, frtsSysmemSize, frtsVidmemOffset,
	 * frtsVidmemSize are payload bytes 12..35 */
	cot = out->cot_packet + 8;
	if ((cot[CO(version)] | (uint32_t)cot[CO(version) + 1] << 8) != GSP_GB20B_COT_VERSION)
		FAIL("COT payload is not version 2");
	bzero_(cot + CO(frtsSysmemOffset), 24);

	/* the CrashCat queue: one zeroed page, named in GspFwWprMeta */
	q = take(a, GSP_GB20B_CRASH_QUEUE_SIZE, &pa);
	if (!q)
		FAIL("arena too small");
	if (!fits_dma(a->pa, a->used, in->dma_bits))
		FAIL("address beyond the DMA window");
	wpr = a->base + (out->wpr_meta.pa - a->pa);
	wr64(wpr + WM(elfDataOffset), pa);
	wr32(wpr + WM(elfDataSize), GSP_GB20B_CRASH_QUEUE_SIZE);
	*crash = (gsp_region){ pa, GSP_GB20B_CRASH_QUEUE_SIZE };
	return 0;
}
