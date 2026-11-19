/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* gb20b_sim.h -- a simulated GB20B SEC2 + GSP falcon for the host test of the start-up. NOT the chip.
 *
 * Models only what the driver source says the host sees on BAR0 during the GSP start-up on GB20B:
 * the FWSEC-done scratch register, SEC2 EMEM channel 0 (1 KiB, EMEMC/EMEMD with write and read
 * auto-increment), its command queue HEAD/TAIL, its message queue HEAD/TAIL carrying the reply, and the
 * GSP falcon's MAILBOX0/1, HWCFG2 (target-mask pattern, lockdown bit 13), FALCON_OS, RISC-V CPUCTL and
 * QUEUE_HEAD(0). Timing is counted in reads of the polled register. Every access is logged in order;
 * accesses the driver never makes on GB20B (SEC2 MAILBOX0/1 above all), and orders it never uses, are
 * counted as violations. Choices the source does not settle are marked SIM-CHOICE.
 * Derived from the GB10B simulator of aien-dev/aienux c9678d7b native/kernel/gpu/gsp_sim.[ch]. */
#ifndef AGPU_GB20B_SIM_H
#define AGPU_GB20B_SIM_H

#include <stdint.h>
#include "gmmu.h"

#define G2SIM_LOG_MAX 2048u
#define G2SIM_NO_END 0xFFFFFFFFu /* a wait that never ends */

struct g2sim_cfg {
	uint32_t secboot_reads;     /* scratch reads that do not yet return 0xff */
	uint32_t secboot_value;     /* what the scratch returns before that (SIM-CHOICE: any value but 0xff) */
	uint32_t busy_reads;        /* QUEUE_HEAD reads during which the channel still holds an old message */
	uint32_t reply_reads;       /* MSGQ_HEAD reads before SEC2 posts its reply */
	uint32_t rsp_bytes;         /* reply size in bytes; 0 = the default 20 */
	uint32_t rsp_set;           /* bit i: use rsp_dw[i] instead of the default reply dword i */
	uint32_t rsp_dw[5];
	uint32_t rsp_head;          /* MSGQ_HEAD value SEC2 posts (SIM-CHOICE: 0) */
	uint32_t rsp_no_inc;        /* hardware fault: EMEMC does not advance on read */
	uint32_t mask_reads;        /* HWCFG2 reads that return 0xBADF41A7 after the reply is acknowledged */
	uint32_t args_reads;        /* GSP MAILBOX0 reads that still return the boot-argument address */
	uint32_t lockdown_reads;    /* HWCFG2 reads with lockdown set after the target mask is released */
	uint32_t fmc_error;         /* non-zero: the FMC writes this code to GSP MAILBOX0 instead of releasing */
	uint32_t riscv_dead;        /* RISC-V never reports ACTIVE */
	uint32_t emem_no_inc;       /* hardware fault: EMEMC does not advance on write */
	uint64_t fmc_args_pa;       /* what SEC2 stuffs in GSP MAILBOX0/1 */
	uint32_t hwcfg2_zero_reads; /* HWCFG2 reads that return 0 before the target-mask pattern (SIM-CHOICE) */
};

struct g2sim_ev {
	uint32_t off, val;
	uint8_t write;
};

struct g2sim {
	struct g2sim_cfg cfg;
	uint32_t emem[256];
	uint32_t ememc, qhead, qtail, mhead, mtail;
	uint32_t scratch_left;
	uint32_t gsp_mb0, gsp_mb1, hwcfg2, os, cpuctl, gsp_qhead_writes;
	/* phase: 0 idle, 1 command posted, 5 reply posted, 6 tail acknowledged, 2 mask, 3 FMC running,
	 * 4 released, 9 rejected */
	uint32_t phase, left, zero_left;
	uint32_t packet_ok, reply_ok, os_written_at_phase;
	struct g2sim_ev log[G2SIM_LOG_MAX];
	uint32_t nlog, violations;
};

void g2sim_init(struct g2sim *s, const struct g2sim_cfg *cfg);
struct ck_gpu_regs g2sim_regs(struct g2sim *s);

#endif
