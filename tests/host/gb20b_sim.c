// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* gb20b_sim.c -- simulated GB20B SEC2 + GSP falcon (see gb20b_sim.h). NOT the chip. */
#include "gb20b_sim.h"
#include "gb20b_mmio.h"

#define PHASE_IDLE 0u
#define PHASE_POSTED 1u
#define PHASE_MASK 2u
#define PHASE_FMC 3u
#define PHASE_UP 4u
#define PHASE_REPLIED 5u
#define PHASE_TAIL_ACKED 6u
#define PHASE_REJECTED 9u

static void logev(struct g2sim *s, uint32_t off, uint32_t val, uint8_t write)
{
	if (s->nlog < G2SIM_LOG_MAX) {
		s->log[s->nlog].off = off;
		s->log[s->nlog].val = val;
		s->log[s->nlog].write = write;
	}
	s->nlog++;
}

void g2sim_init(struct g2sim *s, const struct g2sim_cfg *cfg)
{
	for (uint32_t i = 0; i < sizeof(*s); i++)
		((uint8_t *)s)[i] = 0;
	s->cfg = *cfg;
	/* An old message still in the channel: HEAD != TAIL until busy_reads HEAD reads have passed. */
	if (cfg->busy_reads) {
		s->qhead = 0;
		s->qtail = 16;
	}
	s->hwcfg2 = G2_PRIV_LOCKED | 0xA7u; /* SIM-CHOICE: target locked; low byte = extra error information */
	s->scratch_left = cfg->secboot_reads;
	s->left = cfg->busy_reads;
}

static uint32_t emem_index(const struct g2sim *s)
{
	return ((s->ememc >> 8) & 0xFFu) * 64u + ((s->ememc >> 2) & 0x3Fu);
}

static void emem_advance(struct g2sim *s)
{
	uint32_t dw = emem_index(s) + 1u;

	s->ememc = (s->ememc & ~0xFFFCu) | ((dw / 64u) & 0xFFu) << 8 | (dw % 64u) << 2;
}

/* SEC2 accepts the GB20B chain-of-trust packet only: MCTP SOM|EOM, NVDM vendor PCI 0x10de type COT,
 * payload version 2 (cotPayloadVersion, g_kernel_sec2_nvoc.c:485), size 860, every FRTS field zero
 * (ksec2PrepareBootCommands_GB20B :988-1000 and ksec2SetupGspImages_GB20B, which never sets
 * frtsSysmemSize). The FRTS fields are bytes 20..43 of the packet: dwords 5..10. */
static uint32_t packet_ok(const struct g2sim *s)
{
	if (s->qtail != 864u || s->emem[0] != 0xC0000000u || s->emem[1] != 0x1410DE7Eu ||
	    (s->emem[2] & 0xFFFFu) != 2u || (s->emem[2] >> 16) != 860u)
		return 0;
	for (uint32_t i = 5; i <= 10; i++)
		if (s->emem[i] != 0)
			return 0;
	return 1;
}

/* SEC2 posts its reply into the channel and moves the message queue (SIM-CHOICE: at dword 0, as the
 * driver's reader assumes, :505-512). Default: one packet, SEC2_RESPONSE to COT, error 0 for an
 * accepted packet and FSP_ERR_IFS_ERR_INVALID_DATA 0xA1 for a refused one (SIM-CHOICE). */
static void post_reply(struct g2sim *s)
{
	uint32_t dw[5] = { 0xC0000000u, 0x1510DE7Eu, 0u, 0x14u, s->packet_ok ? 0u : 0xA1u };
	uint32_t bytes = s->cfg.rsp_bytes ? s->cfg.rsp_bytes : 20u;

	for (uint32_t i = 0; i < 5; i++)
		if (s->cfg.rsp_set & (1u << i))
			dw[i] = s->cfg.rsp_dw[i];
	for (uint32_t i = 0; i < 256; i++)
		s->emem[i] = i < 5 ? dw[i] : 0x5EC20000u + i; /* stale channel contents beyond the reply */
	s->qhead = s->qtail; /* the command is consumed */
	s->mhead = s->cfg.rsp_head;
	s->mtail = s->cfg.rsp_head + bytes - 4u;
	s->reply_ok = s->packet_ok && dw[4] == 0;
	s->phase = PHASE_REPLIED;
}

static uint32_t sim_rd(void *ctx, uint32_t off)
{
	struct g2sim *s = ctx;
	uint32_t v = 0;

	switch (off) {
	case G2_PMC_SCRATCH_RESET_PLUS_2:
		if (s->scratch_left == 0) {
			v = G2_SECBOOT_DONE;
		} else {
			v = s->cfg.secboot_value;
			if (s->scratch_left != G2SIM_NO_END)
				s->scratch_left--;
		}
		break;
	case G2_SEC2_QHEAD0:
		if (s->phase == PHASE_IDLE && s->qhead != s->qtail) {
			if (s->left == 0)
				s->qhead = s->qtail; /* SEC2 drained the channel */
			else if (s->left != G2SIM_NO_END)
				s->left--;
		}
		v = s->qhead;
		break;
	case G2_SEC2_QTAIL0: v = s->qtail; break;
	case G2_SEC2_EMEMC0: v = s->ememc; break;
	case G2_SEC2_EMEMD0:
		if (!(s->ememc & G2_EMEMC_AINCR) || emem_index(s) >= 256u ||
		    (s->phase != PHASE_REPLIED && s->phase != PHASE_TAIL_ACKED)) {
			s->violations++; /* the driver reads the channel only for a posted reply, with AINCR */
			break;
		}
		v = s->emem[emem_index(s)];
		if (!s->cfg.rsp_no_inc)
			emem_advance(s);
		break;
	case G2_SEC2_MSGQ_HEAD0:
		if (s->phase == PHASE_POSTED) {
			if (s->left == 0)
				post_reply(s);
			else if (s->left != G2SIM_NO_END)
				s->left--;
		}
		v = s->mhead;
		break;
	case G2_SEC2_MSGQ_TAIL0:
		v = s->mtail;
		if (s->cfg.rsp_withdrawn && s->phase == PHASE_REPLIED && s->tail_reads++ >= 1)
			v = s->mhead;
		break;
	case G2_GSP_HWCFG2:
		if (s->phase == PHASE_MASK && s->zero_left) {
			if (s->zero_left != G2SIM_NO_END)
				s->zero_left--;
			logev(s, off, 0, 0);
			return 0; /* the driver's mask check treats 0 as not released */
		}
		if (s->phase == PHASE_MASK) {
			if (s->left == 0) {
				s->phase = PHASE_FMC;
				s->hwcfg2 = G2_HWCFG2_LOCKDOWN | 0x5u; /* SIM-CHOICE: other HWCFG2 bits */
				s->gsp_mb0 = (uint32_t)s->cfg.fmc_args_pa;
				s->gsp_mb1 = (uint32_t)(s->cfg.fmc_args_pa >> 32);
				s->left = s->cfg.lockdown_reads;
			} else if (s->left != G2SIM_NO_END) {
				s->left--;
			}
		} else if (s->phase == PHASE_FMC && s->gsp_mb0 != (uint32_t)s->cfg.fmc_args_pa) {
			/* the FMC has read its arguments: count down to release (or the error code) */
			if (s->left == 0) {
				if (s->cfg.fmc_error) {
					s->gsp_mb0 = s->cfg.fmc_error;
				} else {
					s->hwcfg2 &= ~G2_HWCFG2_LOCKDOWN;
					s->phase = PHASE_UP;
					/* dead: every low bit but ACTIVE, so a wrong mask bit shows */
					s->cpuctl = s->cfg.riscv_dead ? G2_CPUCTL_ACTIVE - 1u : G2_CPUCTL_ACTIVE;
				}
			} else if (s->left != G2SIM_NO_END) {
				s->left--;
			}
		} else if (s->phase != PHASE_UP && s->phase != PHASE_FMC) {
			s->violations++; /* HWCFG2 before the reply is acknowledged: the start-up ran ahead of SEC2 */
		}
		v = s->hwcfg2;
		break;
	case G2_GSP_MAILBOX0:
		if (s->phase != PHASE_FMC && s->phase != PHASE_UP)
			s->violations++; /* unreadable while the target mask is locked (measured: 0xbadf4100) */
		if (s->phase == PHASE_FMC && s->gsp_mb0 == (uint32_t)s->cfg.fmc_args_pa) {
			if (s->cfg.args_reads == G2SIM_NO_END) {
			} else if (s->cfg.args_reads == 0) {
				s->gsp_mb0 = s->gsp_mb1 = 0;
			} else {
				s->cfg.args_reads--;
			}
		}
		v = s->phase == PHASE_FMC || s->phase == PHASE_UP ? s->gsp_mb0 : G2_PRIV_LOCKED;
		break;
	case G2_GSP_MAILBOX1: v = s->gsp_mb1; break;
	case G2_GSP_OS: v = s->os; break;
	case G2_RISCV_CPUCTL: v = s->cpuctl; break;
	default: s->violations++; break; /* includes SEC2 MAILBOX0/1: GB10B only */
	}
	logev(s, off, v, 0);
	return v;
}

static void sim_wr(void *ctx, uint32_t off, uint32_t val)
{
	struct g2sim *s = ctx;

	logev(s, off, val, 1);
	switch (off) {
	case G2_SEC2_EMEMC0:
		/* write auto-increment before the doorbell, read auto-increment for the reply, offset 0 */
		if (!((s->phase == PHASE_IDLE && val == G2_EMEMC_AINCW) || (s->phase == PHASE_REPLIED && val == G2_EMEMC_AINCR)))
			s->violations++;
		s->ememc = val;
		break;
	case G2_SEC2_EMEMD0: {
		uint32_t dw = emem_index(s);

		if (!(s->ememc & G2_EMEMC_AINCW) || dw >= 256u || s->phase != PHASE_IDLE) {
			s->violations++;
			break;
		}
		s->emem[dw] = val;
		if (!s->cfg.emem_no_inc)
			emem_advance(s);
		break;
	}
	case G2_SEC2_QTAIL0:
		if (s->phase != PHASE_IDLE || s->qhead != s->qtail)
			s->violations++; /* the driver writes only into an empty channel */
		s->qtail = val;
		break;
	case G2_SEC2_QHEAD0:
		/* the doorbell: TAIL must already point at the last dword of a message starting at 0 */
		if (s->phase != PHASE_IDLE || val != 0 || s->qtail == s->qhead)
			s->violations++;
		s->qhead = val;
		if (s->phase == PHASE_IDLE) {
			s->packet_ok = packet_ok(s);
			s->phase = PHASE_POSTED;
			s->left = s->cfg.reply_reads;
		}
		break;
	case G2_SEC2_MSGQ_TAIL0:
		/* the acknowledgement, first half: TAIL = the head SEC2 posted */
		if (s->phase != PHASE_REPLIED || val != s->mhead)
			s->violations++;
		s->mtail = val;
		if (s->phase == PHASE_REPLIED)
			s->phase = PHASE_TAIL_ACKED;
		break;
	case G2_SEC2_MSGQ_HEAD0:
		/* second half: HEAD = the same head, only after TAIL */
		if (s->phase != PHASE_TAIL_ACKED || val != s->mhead)
			s->violations++;
		s->mhead = val;
		if (s->phase == PHASE_TAIL_ACKED) {
			if (s->reply_ok) {
				s->phase = PHASE_MASK;
				s->left = s->cfg.mask_reads;
				s->zero_left = s->cfg.hwcfg2_zero_reads;
			} else {
				s->phase = PHASE_REJECTED; /* SIM-CHOICE: SEC2 never releases the target mask */
			}
		}
		break;
	case G2_GSP_OS:
		s->os = val;
		s->os_written_at_phase = s->phase;
		if (s->phase != PHASE_UP)
			s->violations++; /* FALCON_OS before lockdown release */
		break;
	case G2_GSP_QHEAD0:
		s->gsp_qhead_writes++;
		if (val != 0)
			s->violations++;
		break;
	default: s->violations++; break;
	}
}

struct ck_gpu_regs g2sim_regs(struct g2sim *s)
{
	struct ck_gpu_regs r = { sim_rd, sim_wr, s };

	return r;
}
