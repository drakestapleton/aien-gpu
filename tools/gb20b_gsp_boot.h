/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* gb20b_gsp_boot.h -- the GSP boot package for GB20B, the chip in the DGX Spark (PMC_BOOT_42
 * 0x1bba1000: architecture 0x1b, implementation 0xb).
 *
 * The package builder of aien-dev/aienux c9678d7b (native/kernel/dev/gb10_gsp_boot.[ch]) lays out what
 * NVIDIA's driver 580.173.02 hands a GB10B. GB20B takes the same layout HALs (kgspPopulateWprMeta_GB10B
 * is "GB10B | GB20B | GB20C", generated/g_kernel_gsp_nvoc.c:987-989; libos arguments, radix3, message
 * queues and FMC parameters have no per-chip HAL) and differs in exactly these inputs:
 *
 *   boot loader      GspRmBoot_GB20B, 212992 bytes, RM_RISCV_UCODE_DESC manifest 0x0, monitor data
 *                    0xa00, monitor code 0xf000 (g_kernel_gsp_nvoc.c:792-794; GB10B: 196608, code 0xb000)
 *   FMC              GspRmFmcGfwProdSigned_GB20B (:928-930): another image, hash, key and signature
 *   GSP-RM signature section .fwsignature_gb20y (nv-firmware-chip-family-select.h:64-68)
 *   COT payload      version 2 (cotPayloadVersion, generated/g_kernel_sec2_nvoc.c:478-485) and every
 *                    FRTS field 0: ksec2PrepareBootCommands_GB20B zeroes the payload and
 *                    ksec2SetupGspImages_GB20B never sets frtsSysmemSize (kernel_sec2_gb20b.c:730-838,
 *                    988-1000); the GB10B HAL stores the FMC size there (kernel_sec2_gb10b.c)
 *   crash queue      a 4096-byte system-memory CrashCat queue whose address and size
 *                    kgspPopulateWprMeta_GH100 writes into GspFwWprMeta (kernel_gsp_gh100.c:366-372;
 *                    kgspGetCrashcatSysmemBufferSize is 4096 for GB20B and 0 for GB10B,
 *                    g_kernel_gsp_nvoc.c:1859-1866). Whether GSP-RM needs it to boot: NOT DETERMINED.
 *
 * Which images are in a package is the packager's business (tools/package.sh checks each against a
 * pin); this builder checks the geometry it can see. Nothing here has run on the chip. */
#ifndef AGPU_GB20B_GSP_BOOT_H
#define AGPU_GB20B_GSP_BOOT_H

#include "gb10_gsp_boot.h"

#define GSP_GB20B_BOOT_42 0x1bba1000u /* read on the Spark, 2026-11-12 and 2026-11-18 */
#define GSP_GB20B_BOOTLOADER_SIZE 212992u
#define GSP_GB20B_BL_CODE_OFF 0xf000u
#define GSP_GB20B_BL_DATA_OFF 0xa00u
#define GSP_GB20B_BL_MANIFEST_OFF 0x0u
#define GSP_GB20B_FMC_SIZE 212296u
#define GSP_GB20B_COT_VERSION 2u
#define GSP_GB20B_CRASH_QUEUE_SIZE 4096u

/* gsp_boot_build for GB20B. *crash receives the CrashCat queue region (zeroed, in the arena, after
 * every block of the plan). Returns 0, or -1 with *why set. */
int gsp_boot_build_gb20b(gsp_arena *a, const gsp_boot_in *in, gsp_boot_plan *out, gsp_region *crash,
			 const char **why);

#endif
