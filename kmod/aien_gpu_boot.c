// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * aien_gpu_boot.c -- stage B: start NVIDIA's signed GSP-RM on the GB10 with our code, then wait
 * for it to report INIT_DONE, running any CPU-sequencer request it sends on the way.
 * The boot package (GSP-RM image, radix3, WPR metadata, FMC arguments, the SEC2 COT packet,
 * queues) is built into the arena by user space (tools/aien-gpu-boot) before this call;
 * om/abi.om checks the block that describes it. om/boot.om decides every register access of the
 * 11 start-up steps; this file performs them on BAR0 one at a time. One attempt per module load:
 * a failed start-up is never retried without a reset.
 */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#include "aien_gpu.h"

static u32 blk32(const u8 *b, unsigned int o)
{
	return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (u32)b[o + 3] << 24;
}

static u64 blk64(const u8 *b, unsigned int o)
{
	return blk32(b, o) | (u64)blk32(b, o + 4) << 32;
}

/*
 * The 11 register steps. Returns 0 when om/boot.om finished (stage in st[12]), negative on a
 * trap.
 */
static int agpu_boot_mmio(struct agpu_dev *d, const u8 *in, struct agpu_boot_out *o)
{
	u64 st[32] = { 0 }, act[4], obs = 0, r;
	/* 1024-byte view; the rest of the staging buffer is unused here */
	u8 *pkt = d->msg_tx;
	u32 pause = blk32(in, 64), len = blk32(in, 16);
	u64 cap;
	int rc, i;

	memset(pkt, 0, 1024);
	memcpy(pkt, (u8 *)d->arena + blk64(in, 8), len);
	st[1] = len;
	st[2] = blk64(in, 24);
	st[3] = blk32(in, 20);
	for (i = 0; i < 4; i++)
		st[4 + i] = blk32(in, 48 + 4 * i);
	st[25] = (((d->info.boot42 >> 24) & 0x3fu) == 0x1bu) ? 1 : 0;
	/*
	 * om/boot.om bounds every wait by its budget; this cap only guards the glue against a
	 * defect
	 */
	cap = 8192 + 8ull * (st[4] + st[5] + st[6] + st[7] + 4);
	for (;;) {
		rc = agpu_om("boot", agpu_om_boot, (u64)st, (u64)pkt, obs, (u64)act, 0, 0, &r);
		if (rc)
			return rc;
		if (r == 1)
			break;
		if (r != 0 || !cap--)
			return -EPROTO;
		obs = 0;
		if (act[0] == 1) {
			obs = ioread32(d->bar0 + (u32)act[1]);
		} else if (act[0] == 2) {
			d->touched = true;
			iowrite32((u32)act[2], d->bar0 + (u32)act[1]);
		} else if (act[0] == 3) {
			udelay(pause);
			cond_resched();
		} else {
			return -EPROTO;
		}
	}
	o->stage = st[12];
	for (i = 0; i < 4; i++)
		o->polls[i] = st[13 + i];
	o->dwords = st[17];
	o->ememc_after = st[18];
	o->sec2_mb0 = st[19];
	o->sec2_mb1 = st[20];
	o->hwcfg2 = st[21];
	o->gsp_mb0 = st[22];
	o->gsp_mb1 = st[23];
	o->cpuctl = st[24];
	return 0;
}

/* Read the status queue until INIT_DONE, a refusal or the deadline. */
static void agpu_boot_wait(struct agpu_dev *d, u32 wait_ms, struct agpu_boot_out *o)
{
	ktime_t end = ktime_add_ms(ktime_get(), wait_ms);
	u32 code;
	int rc;

	for (;;) {
		rc = agpu_mq_recv(d, &code);
		if (rc < 0) {
			o->verdict = AGPU_BOOT_QUEUE_ERROR;
			o->code = code;
			return;
		}
		if (rc == 0) {
			if (ktime_after(ktime_get(), end)) {
				o->verdict = AGPU_BOOT_TIMEOUT;
				return;
			}
			usleep_range(100, 200);
			continue;
		}
		o->msgs++;
		o->last_fn = blk32(d->msg_rx, 60);
		o->rx_seq = d->rx_seq;
		if (o->last_fn == AGPU_EV_INIT_DONE) {
			o->verdict = AGPU_BOOT_INIT_DONE;
			return;
		}
		if (o->last_fn == AGPU_EV_RUN_CPU_SEQUENCER) {
			rc = agpu_seq_run(d, &code, &o->seq_ops);
			if (rc) {
				o->verdict = rc > 0 ? rc : AGPU_BOOT_SEQ_STOPPED;
				o->code = code;
				return;
			}
			o->seq_runs++;
		}
		/* any other event (log prints, error reports) is counted and read past */
	}
}

void agpu_boot(struct agpu_dev *d, struct agpu_boot *b)
{
	struct agpu_boot_out *o = &d->boot;
	const u8 *in = b->in;
	ktime_t t0;
	u64 verdict;
	int rc;

	memset(o, 0, sizeof(*o));
	if (d->info.state != AGPU_STATE_BOUND || d->info.boot_attempts) {
		o->verdict = AGPU_BOOT_STATE;
		goto out;
	}
	if (!d->arena || !d->heap || !d->info.fw_ok) {
		o->verdict = AGPU_BOOT_REFUSED;
		o->code = 100 + (!d->arena) + 2 * (!d->heap) + 4 * (!d->info.fw_ok);
		goto out;
	}
	rc = agpu_om("abi", agpu_om_abi, 1, (u64)in, d->arena_iova, d->arena_bytes, 0, 0, &verdict);
	if (rc || verdict) {
		o->verdict = AGPU_BOOT_REFUSED;
		o->code = rc ? 99 : verdict;
		goto out;
	}
	d->info.boot_attempts = 1;
	d->info.state = AGPU_STATE_BOOTING;
	t0 = ktime_get();
	rc = agpu_boot_mmio(d, in, o);
	o->mmio_us = ktime_us_delta(ktime_get(), t0);
	if (rc || o->stage) {
		o->verdict = AGPU_BOOT_MMIO_STOPPED;
		o->code = rc ? 99 : 0;
		d->info.state = AGPU_STATE_FAILED;
		goto out;
	}
	d->cmdq = (u8 *)d->arena + blk64(in, 32);
	d->statq = (u8 *)d->arena + blk64(in, 40);
	d->tx_seq = blk32(in, 72);
	d->rx_seq = 0;
	d->rpc_seq = 0;
	t0 = ktime_get();
	agpu_boot_wait(d, blk32(in, 68), o);
	o->wait_us = ktime_us_delta(ktime_get(), t0);
	d->info.state = o->verdict == AGPU_BOOT_INIT_DONE ? AGPU_STATE_GSP_UP : AGPU_STATE_FAILED;
	dev_info(&d->pdev->dev, "stage B verdict %u code %u stage %u msgs %u seq %u in %llu+%llu us\n",
		 o->verdict, o->code, o->stage, o->msgs, o->seq_runs, o->mmio_us, o->wait_us);
out:
	b->out = *o;
}
