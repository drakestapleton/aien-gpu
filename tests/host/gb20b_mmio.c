// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* gb20b_mmio.c -- the GB20B GSP start-up in C (see gb20b_mmio.h for the steps and sources). */
#include "gb20b_mmio.h"

static uint32_t rd(const struct ck_gpu_regs *r, uint32_t off)
{
	return r->rd(r->ctx, off);
}

static void wr(const struct ck_gpu_regs *r, uint32_t off, uint32_t v)
{
	r->wr(r->ctx, off, v);
}

static void between_polls(const struct g2_in *in)
{
	if (in->pause)
		in->pause(in->pause_ctx);
}

static uint32_t emem_dwords(uint32_t ememc)
{
	return ((ememc >> 8) & 0xFFu) * G2_EMEM_DW_PER_BLK + ((ememc >> 2) & 0x3Fu);
}

static int mask_released(uint32_t hwcfg2)
{
	return hwcfg2 != 0 && (hwcfg2 & G2_PRIV_LOCKED_MASK) != G2_PRIV_LOCKED;
}

/* Step 8 condition. Reads MAILBOX0, MAILBOX1 only on a match, then HWCFG2, in the driver's order. */
static int lockdown_done(const struct ck_gpu_regs *r, const struct g2_in *in, struct g2_out *o)
{
	o->gsp_mb0 = rd(r, G2_GSP_MAILBOX0);
	if (o->gsp_mb0 != 0 && o->gsp_mb0 == (uint32_t)in->fmc_args_pa) {
		o->gsp_mb1 = rd(r, G2_GSP_MAILBOX1);
		if (o->gsp_mb1 == (uint32_t)(in->fmc_args_pa >> 32))
			return 0; /* boot arguments not read by the FMC yet */
	}
	o->hwcfg2 = rd(r, G2_GSP_HWCFG2);
	return !(o->hwcfg2 & G2_HWCFG2_LOCKDOWN) || o->gsp_mb0 != 0;
}

static uint32_t stop(struct g2_out *o, uint32_t stage, uint32_t why)
{
	o->why = why;
	return o->stage = stage;
}

/* Step 6 b..f: read the reply, acknowledge it, check it. */
static uint32_t reply(const struct ck_gpu_regs *r, struct g2_out *o)
{
	uint32_t head, tail, bytes, dw[5] = { 0, 0, 0, 0, 0 }, i, som, eom, nvdm;

	head = rd(r, G2_SEC2_MSGQ_HEAD0);
	tail = rd(r, G2_SEC2_MSGQ_TAIL0);
	bytes = tail - head + 4u; /* 32-bit, as the driver's NvU32 */
	o->rsp_bytes = bytes;
	if (bytes < 4u || bytes > G2_EMEM_CHANNEL_BYTES)
		return stop(o, G2_E_REPLY, G2_WHY_SIZE);

	wr(r, G2_SEC2_EMEMC0, G2_EMEMC_AINCR);
	for (i = 0; i < bytes / 4u; i++) {
		uint32_t v = rd(r, G2_SEC2_EMEMD0);

		if (i < 5u)
			dw[i] = v;
	}
	o->rsp_ememc = rd(r, G2_SEC2_EMEMC0);
	wr(r, G2_SEC2_MSGQ_TAIL0, head);
	wr(r, G2_SEC2_MSGQ_HEAD0, head);
	o->rsp_dw0 = dw[0];
	o->rsp_dw1 = dw[1];
	o->rsp_task = dw[2];
	o->rsp_cmd = dw[3];
	o->rsp_err = dw[4];
	if (emem_dwords(o->rsp_ememc) != bytes / 4u)
		return stop(o, G2_E_REPLY, G2_WHY_POINTER);

	som = dw[0] >> 31;
	eom = (dw[0] >> 30) & 1u;
	if (som) { /* a first packet carries the message header: the driver checks it before anything else */
		if ((dw[1] & 0x7Fu) != G2_MCTP_TYPE_VENDOR_PCI)
			return stop(o, G2_E_REPLY, G2_WHY_TYPE);
		if (((dw[1] >> 8) & 0xFFFFu) != G2_VENDOR_NV)
			return stop(o, G2_E_REPLY, G2_WHY_VENDOR);
		if (bytes < 8u)
			return stop(o, G2_E_REPLY, G2_WHY_SHORT);
	}
	if (!(som && eom))
		return stop(o, G2_E_REPLY, G2_WHY_PACKETS);
	nvdm = dw[1] >> 24;
	if (nvdm != G2_NVDM_SEC2_RESPONSE && nvdm != G2_NVDM_SMBPBI)
		return stop(o, G2_E_REPLY, G2_WHY_NVDM);
	if (bytes < 20u)
		return stop(o, G2_E_REPLY, G2_WHY_SHORT);
	if (o->rsp_err != 0)
		return stop(o, G2_E_SEC2ERR, 0);
	return G2_OK;
}

uint32_t g2_boot(const struct ck_gpu_regs *r, const struct g2_in *in, struct g2_out *o)
{
	uint32_t i, n, st;

	for (i = 0; i < sizeof(*o); i++)
		((uint8_t *)o)[i] = 0;
	if (!r || !in || !in->packet || in->len == 0 || in->len > G2_EMEM_CHANNEL_BYTES)
		return stop(o, G2_E_ARG, 0);

	/* 0 */
	for (n = 0;; n++) {
		o->scratch = rd(r, G2_PMC_SCRATCH_RESET_PLUS_2);
		if (o->scratch == G2_SECBOOT_DONE)
			break;
		if (n >= in->polls_send) {
			o->polls0 = n;
			return stop(o, G2_E_SECBOOT, 0);
		}
		between_polls(in);
	}
	o->polls0 = n;

	/* 1 */
	for (n = 0;; n++) {
		uint32_t head = rd(r, G2_SEC2_QHEAD0);

		if (head == rd(r, G2_SEC2_QTAIL0))
			break;
		if (n >= in->polls_send) {
			o->polls[0] = n;
			return stop(o, G2_E_BUSY, 0);
		}
		between_polls(in);
	}
	o->polls[0] = n;

	/* 2, 3, 4 */
	wr(r, G2_SEC2_EMEMC0, G2_EMEMC_AINCW);
	uint32_t start = emem_dwords(rd(r, G2_SEC2_EMEMC0));

	for (i = 0; i + 4 <= in->len; i += 4)
		wr(r, G2_SEC2_EMEMD0, (uint32_t)in->packet[i] | (uint32_t)in->packet[i + 1] << 8 |
					      (uint32_t)in->packet[i + 2] << 16 | (uint32_t)in->packet[i + 3] << 24);
	if (i < in->len) {
		uint32_t v = 0;

		for (uint32_t k = 0; i + k < in->len; k++)
			v |= (uint32_t)in->packet[i + k] << (8 * k);
		wr(r, G2_SEC2_EMEMD0, v);
	}
	o->dwords = (in->len + 3) / 4;
	o->ememc_after = rd(r, G2_SEC2_EMEMC0);
	if (emem_dwords(o->ememc_after) - start != o->dwords)
		return stop(o, G2_E_EMEM, 0);

	/* 5: TAIL first, HEAD last (HEAD interrupts SEC2) */
	wr(r, G2_SEC2_QTAIL0, o->dwords * 4 - 4);
	wr(r, G2_SEC2_QHEAD0, 0);

	/* 6 a */
	for (n = 0;; n++) {
		uint32_t head = rd(r, G2_SEC2_MSGQ_HEAD0);

		if (head != rd(r, G2_SEC2_MSGQ_TAIL0))
			break;
		if (n >= in->polls_reply) {
			o->polls[1] = n;
			return stop(o, G2_E_SEC2, 0);
		}
		between_polls(in);
	}
	o->polls[1] = n;
	/* 6 b..f */
	st = reply(r, o);
	if (st != G2_OK)
		return st;

	/* 7 */
	for (n = 0;; n++) {
		o->hwcfg2 = rd(r, G2_GSP_HWCFG2);
		if (mask_released(o->hwcfg2))
			break;
		if (n >= in->polls_mask) {
			o->polls[2] = n;
			return stop(o, G2_E_MASK, 0);
		}
		between_polls(in);
	}
	o->polls[2] = n;

	/* 8 */
	for (n = 0;; n++) {
		if (lockdown_done(r, in, o))
			break;
		if (n >= in->polls_lockdown) {
			o->polls[3] = n;
			o->gsp_mb1 = rd(r, G2_GSP_MAILBOX1);
			return stop(o, G2_E_LOCKDOWN, 0);
		}
		between_polls(in);
	}
	o->polls[3] = n;

	/* 9 */
	o->gsp_mb0 = rd(r, G2_GSP_MAILBOX0);
	if (o->gsp_mb0 != 0)
		return stop(o, G2_E_FMC, 0);

	/* 10, 11 */
	wr(r, G2_GSP_OS, in->app_version);
	o->cpuctl = rd(r, G2_RISCV_CPUCTL);
	if (!(o->cpuctl & G2_CPUCTL_ACTIVE))
		return stop(o, G2_E_RISCV, 0);
	return stop(o, G2_OK, 0);
}
