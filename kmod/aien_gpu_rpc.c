// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * aien_gpu_rpc.c -- the GSP message queues and the CPU sequencer. The Omega units decide (om/seal.om,
 * om/mqput.om, om/mqget.om, om/seq.om); this file only orders memory and touches registers.
 * Memory order, as message_queue_cpu.c (open-gpu-kernel-modules 580.173.02) does it:
 *   send: slots written -> dma_wmb() -> writePtr published -> GSP QUEUE_HEAD(0) = 0 (notify)
 *   recv: writePtr read -> dma_rmb() -> slots read -> dma_mb() -> our readPtr published
 */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/string.h>

#include "aien_gpu.h"
#include "agpu_exec.h"

#define MQ_WRITEPTR 16	/* msgqTxHeader.writePtr */
#define MQ_RXHDR 32	/* msgqRxHeader.readPtr, kept in the other side's queue (SWAP_RX) */

static u32 ld32(const u8 *p)
{
	return READ_ONCE(*(const u32 *)p);
}

static void st32(u8 *p, u32 v)
{
	WRITE_ONCE(*(u32 *)p, v);
}

int agpu_mq_send(struct agpu_dev *d, u32 fn, u32 rpclen, u32 placed, u32 seq, u32 *code)
{
	u64 elems, wr;
	int rc;

	*code = 0;
	rc = agpu_om("seal", agpu_om_seal, (u64)d->msg_tx, fn, rpclen, placed, seq, d->tx_seq, &elems);
	if (rc)
		return rc;
	if (elems >= AGPU_OM_FAIL) {
		*code = elems - AGPU_OM_FAIL;
		return -EINVAL;
	}
	/* GSP's read pointer of our command queue lives in the status queue */
	rc = agpu_om("mqput", agpu_om_mqput, (u64)d->cmdq, (u64)d->msg_tx, elems, ld32(d->statq + MQ_RXHDR), 0, 0,
		     &wr);
	if (rc)
		return rc;
	if (wr >= AGPU_OM_FAIL) {
		*code = 100 + (wr - AGPU_OM_FAIL);
		return wr - AGPU_OM_FAIL == 2 ? -EBUSY : -EIO;
	}
	dma_wmb();
	st32(d->cmdq + MQ_WRITEPTR, (u32)wr);
	d->tx_seq++;
	wmb();				/* the queue in memory before the doorbell (MMIO) */
	iowrite32(0, d->bar0 + AGPU_GSP_QHEAD0);
	return 0;
}

int agpu_mq_recv(struct agpu_dev *d, u32 *code)
{
	u32 wr = ld32(d->statq + MQ_WRITEPTR), rd = ld32(d->cmdq + MQ_RXHDR);
	u64 elems;
	int rc;

	*code = 0;
	dma_rmb();
	rc = agpu_om("mqget", agpu_om_mqget, (u64)d->statq, (u64)d->msg_rx, rd, wr, d->rx_seq, 0, &elems);
	if (rc)
		return rc;
	if (elems >= AGPU_OM_FAIL) {
		*code = elems - AGPU_OM_FAIL;
		return -EIO;
	}
	if (!elems)
		return 0;
	dma_mb();
	st32(d->cmdq + MQ_RXHDR, (rd + (u32)elems) % 63);
	d->rx_seq++;
	return 1;
}

static u32 bar_rd(void *ctx, u32 off)
{
	struct agpu_dev *d = ctx;

	return ioread32(d->bar0 + off);
}

static void bar_wr(void *ctx, u32 off, u32 v)
{
	struct agpu_dev *d = ctx;

	d->touched = true;
	iowrite32(v, d->bar0 + off);
}

/* DELAY_US as asked, up to 1 s per operation (a longer request is a firmware bug we do not follow). */
static void bar_delay(void *ctx, u32 us)
{
	if (us <= 1000)
		udelay(us);
	else
		fsleep(min(us, 1000000u));
}

/* REG_POLL budget: reads after the first, about 1 us apart: 2 s, the order of NVIDIA's
 * GSP_SEQ_BUF_PAYLOAD_REG_POLL default timeout (kgspExecuteSequencerBuffer). */
#define AGPU_SEQ_POLL_BUDGET 2000000u

int agpu_seq_run(struct agpu_dev *d, u32 *code, u32 *ops_done)
{
	struct agpu_regio io = { bar_rd, bar_wr, bar_delay, d, AGPU_SEQ_POLL_BUDGET, { 0 } };
	u64 n;
	int rc;

	*code = 0;
	*ops_done = 0;
	rc = agpu_om("seq", agpu_om_seq, (u64)d->msg_rx, (u64)d->seq_ops, d->bar0_bytes, 0, 0, 0, &n);
	if (rc)
		return rc;
	if (n >= AGPU_OM_FAIL) {
		*code = n - AGPU_OM_FAIL;
		return AGPU_BOOT_SEQ_REFUSED;
	}
	rc = agpu_seq_exec(d->seq_ops, (u32)n, &io, ops_done);
	if (rc) {
		*code = rc;
		return AGPU_BOOT_SEQ_STOPPED;
	}
	return 0;
}
