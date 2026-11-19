/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* gb20b_mmio.h -- the register-level GSP start-up on GB20B, in C: the reference om/boot.om is checked
 * against (tests/host/parity_boot.c). Host test code only; the module runs om/boot.om.
 *
 * The chip in the DGX Spark reports PMC_BOOT_42 0x1bba1000 (architecture 0x1b, implementation 0xb).
 * NVIDIA's driver (open-gpu-kernel-modules 580.173.02, MIT) dispatches that as GB20B
 * (src/nvidia/generated/g_kernel_sec2_nvoc.c:674-953, g_kernel_gsp_nvoc.c:788-795, 924-930).
 * GB20B has no FSP: SEC2 boots the GSP-FMC (kgspBootstrap_GH100, kernel_gsp_gh100.c:833-995, SEC2
 * branch :868-872). What the driver does on BAR0, in order:
 *
 *   0. wait until NV_PMC_SCRATCH_RESET_PLUS_2 (0x5e0) reads exactly 0xff: FWSEC finished
 *        (_ksec2WaitBootCond_GB20B, ksec2WaitForSecureBoot_GB20B, kernel_sec2_gb20b.c:541-575; called
 *        first by ksec2PrepareBootCommands_GB20B :962-968)
 *   1. wait until SEC2 EMEM channel 0 is empty: QUEUE_HEAD(0) == QUEUE_TAIL(0)
 *        (ksec2CanSendPacket_GB10B, kernel_sec2_gb10b.c:343-357, the HAL GB20B also takes,
 *        g_kernel_sec2_nvoc.c:751-753; polled by ksec2PollForCanSend_IMPL, kernel_sec2.c:605-640)
 *   2. EMEMC(0) = offset 0, auto-increment on write          (ksec2SendPacket_GB10B :700-733)
 *   3. EMEMD(0) = the packet, one dword at a time, a short tail zero-padded
 *   4. EMEMC(0) read back: the write pointer moved by exactly the dwords written
 *   5. QUEUE_TAIL(0) = padded size - 4, THEN QUEUE_HEAD(0) = 0 (HEAD interrupts SEC2)
 *   6. the reply (ksec2SendBootCommands_GB20B :1034-1085 calls ksec2SendAndReadMessage_IMPL,
 *      kernel_sec2.c:496-523):
 *      a. wait until MSGQ_HEAD(0) != MSGQ_TAIL(0)             (ksec2IsResponseAvailable_GB20B :142-155,
 *         _ksec2WaitForResponse kernel_sec2.c:203-233)
 *      b. read HEAD and TAIL again; packet bytes = TAIL - HEAD + 4, 4..1024, else stop
 *         (ksec2ReadPacket_GB20B :481-537; TAIL points at the last dword)
 *      c. EMEMC(0) = offset 0, auto-increment on read; read bytes/4 dwords from EMEMD(0); read EMEMC
 *      d. MSGQ_TAIL(0) = head, THEN MSGQ_HEAD(0) = head (the driver acknowledges before it checks
 *         the read pointer, :534-536)
 *      e. the read pointer moved by exactly bytes/4, else stop
 *      f. the packet: SOM and EOM both set (one packet; the driver passes no reassembly buffer),
 *         message type 0x7e, vendor 0x10de, at least 8 bytes (ksec2GetPacketInfo_GB20B :235-281,
 *         ksec2ValidateMctpPayloadHeader_GB20B :294-335), NVDM type 0x15 SEC2_RESPONSE or 0x18 SMBPBI
 *         (ksec2ProcessNvdmMessage_GB20B :344-371), at least 20 bytes, error code (dword 4) == 0
 *         (ksec2ProcessCommandResponse_GB20B :385-418, ksec2ErrorCode2NvStatusMap_GB20B :1115-1139).
 *         MCTP_HEADER is 7 bytes packed (kern_fsp_cot_payload.h:27-33), so the NVDM type is byte 7 and
 *         NVDM_PAYLOAD_COMMAND_RESPONSE {taskId, commandNvdmType, errorCode} is dwords 2, 3, 4.
 *      GB10B instead waits for SEC2 MAILBOX0 != 0xc001cafe (ksec2SendBootCommands_GB10B :640);
 *      gb20b/dev_sec_pri.h does not define that register and GB20B code never reads it.
 *   7. wait until GSP HWCFG2 reads non-zero and not 0xBADF41xx (target mask released)
 *        (_ksec2IsGspTargetMaskReleased, ksec2WaitForGspTargetMaskReleased_GB10B, taken by GB20B,
 *        g_kernel_sec2_nvoc.c:814-816)
 *   8. wait until HWCFG2.RISCV_BR_PRIV_LOCKDOWN (bit 13) is UNLOCK or GSP MAILBOX0 is non-zero, where
 *      MAILBOX0:MAILBOX1 equal to the FMC boot-argument address means "not read yet"
 *        (_kgspLockdownReleasedOrFmcError, kernel_gsp_gh100.c:492-533)
 *   9. GSP MAILBOX0 non-zero after that = the FMC reported an error: stop (:940-946)
 *  10. GSP FALCON_OS = the GspRmBoot descriptor's appVersion (:954-956)
 *  11. RISC-V CPUCTL.ACTIVE_STAT (bit 7) must read ACTIVE (kflcnIsRiscvActive_GA102)
 *
 * Polling only. Each wait has a poll budget; `pause` runs between polls. Measured on the chip so far
 * (2026-11-18, ~/handoffs/L332/window0-20261118T234217Z): only that with the GB10B step 6 the start-up
 * ran ahead of SEC2 and read 0xbadf4100. Register behaviour beyond the driver source is UNVERIFIED. */
#ifndef AGPU_GB20B_MMIO_H
#define AGPU_GB20B_MMIO_H

#include <stdint.h>
#include "gmmu.h" /* struct ck_gpu_regs (aienux oracle) */

#define G2_PMC_SCRATCH_RESET_PLUS_2 0x000005e0u /* gb20b/dev_boot.h:26 */
#define G2_SEC2_EMEMC0 0x00840ac0u               /* gb20b/dev_sec_pri.h:26 */
#define G2_SEC2_EMEMD0 0x00840ac4u               /* :40 */
#define G2_SEC2_QHEAD0 0x00840c00u               /* :44 */
#define G2_SEC2_QTAIL0 0x00840c04u               /* :48 */
#define G2_SEC2_MSGQ_HEAD0 0x00840c80u           /* :53 */
#define G2_SEC2_MSGQ_TAIL0 0x00840c84u           /* :57 */
#define G2_SEC2_MAILBOX0 0x00840040u             /* GB10B only; a GB20B start-up must not touch it */
#define G2_SEC2_MAILBOX1 0x00840044u
#define G2_GSP_MAILBOX0 0x00110040u
#define G2_GSP_MAILBOX1 0x00110044u
#define G2_GSP_OS 0x00110080u
#define G2_GSP_HWCFG2 0x001100f4u
#define G2_GSP_QHEAD0 0x00110c00u
#define G2_RISCV_CPUCTL 0x00111388u

#define G2_EMEMC_AINCW (1u << 24)
#define G2_EMEMC_AINCR (1u << 25)
#define G2_EMEM_DW_PER_BLK 64u
#define G2_EMEM_CHANNEL_BYTES 1024u
#define G2_SECBOOT_DONE 0xffu
#define G2_PRIV_LOCKED 0xBADF4100u
#define G2_PRIV_LOCKED_MASK 0xFFFFFF00u
#define G2_HWCFG2_LOCKDOWN (1u << 13)
#define G2_CPUCTL_ACTIVE (1u << 7)
#define G2_MCTP_TYPE_VENDOR_PCI 0x7eu
#define G2_VENDOR_NV 0x10deu
#define G2_NVDM_SEC2_RESPONSE 0x15u
#define G2_NVDM_SMBPBI 0x18u

/* stage, equal to om/boot.om st[12] */
enum {
	G2_OK = 0,
	G2_E_ARG = 1,
	G2_E_BUSY = 2,
	G2_E_EMEM = 3,
	G2_E_SEC2 = 4,     /* no reply within the budget */
	G2_E_MASK = 5,
	G2_E_LOCKDOWN = 6,
	G2_E_FMC = 7,
	G2_E_RISCV = 8,
	G2_E_SECBOOT = 9,  /* FWSEC not done within the budget */
	G2_E_REPLY = 10,   /* the reply was refused: why says which check */
	G2_E_SEC2ERR = 11, /* SEC2 answered with a non-zero error code */
	G2_STAGES = 12,
};

/* why, equal to om/boot.om st[31] */
enum { G2_WHY_SIZE = 1, G2_WHY_POINTER = 2, G2_WHY_PACKETS = 3, G2_WHY_TYPE = 4, G2_WHY_VENDOR = 5,
       G2_WHY_NVDM = 6, G2_WHY_SHORT = 7, G2_WHYS = 8 };

struct g2_in {
	const uint8_t *packet;
	uint32_t len;
	uint64_t fmc_args_pa;
	uint32_t app_version;
	uint32_t polls_send;     /* steps 0 and 1 */
	uint32_t polls_reply;    /* step 6 */
	uint32_t polls_mask;     /* step 7 */
	uint32_t polls_lockdown; /* step 8 */
	void (*pause)(void *ctx);
	void *pause_ctx;
};

struct g2_out {
	uint32_t stage, why;
	uint32_t polls0, polls[4];
	uint32_t scratch;
	uint32_t dwords, ememc_after;
	uint32_t rsp_bytes, rsp_ememc, rsp_dw0, rsp_dw1, rsp_task, rsp_cmd, rsp_err;
	uint32_t hwcfg2, gsp_mb0, gsp_mb1, cpuctl;
};

uint32_t g2_boot(const struct ck_gpu_regs *r, const struct g2_in *in, struct g2_out *o);

#endif
