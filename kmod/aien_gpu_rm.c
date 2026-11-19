// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * aien_gpu_rm.c -- stage C: GSP-RM bring-up after INIT_DONE, one RM client per open file,
 * compute channels, the doorbell page and the unload, in the order of docs/STAGE-C.md (Route B:
 * an RM-managed address space whose page tables kmod/aien_gpu_mem.c writes; no UVM).
 * Every payload is built by an Omega unit (om/rm.om allocations, om/rmchan.om the channel,
 * om/rmctl.om controls, FREE and unload), every reply is checked by om/rmchk.om, and ChIDs,
 * channel arguments, the work-submit token and the unload waits are decided by om/rmarg.om.
 * This file only moves bytes, waits, allocates host memory and touches registers.
 * Status: IMPLEMENTED, host-tested units; hardware NOT_RUN.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "aien_gpu.h"
#include "aien_gpu_mem.h"

extern const char agpu_om_rm[], agpu_om_rmchan[], agpu_om_rmctl[], agpu_om_rmchk[],
	agpu_om_rmarg[];

#define RM_RPC_MS 10000u		/* one RPC's reply deadline (ours) */
#define RM_UNLOAD_POLLS 2000000u	/* MAILBOX0 / CPUCTL reads, about 1 us apart: 2 s each */
#define RM_MAX_CHAN 16u			/* channels per file */
#define RM_CLIENT_BASE 0xc1d80000u	/* RS_CLIENT_HANDLE_BASE + index 0x80000 (resserv.h:135) */
#define RM_OBJ_BASE 0xcaf00000u		/* object handles inside our clients */
#define RM_VA_BYTES BIT_ULL(49)		/* FERMI_VASPACE_A vaSize, covers AGPU_KVA_* */
#define RM_GSP_MAILBOX0 0x00110040u	/* NV_PGSP_FALCON_MAILBOX0 */
#define RM_GSP_CPUCTL 0x00111388u	/* NV_PRISCV_RISCV_CPUCTL of GSP */
#define RM_ERR_BYTES 4096u		/* error notifier: the first 4 KiB of err_handle */

/* Controls (ctrl2080internal.h, ctrl2080fifo.h, ctrl2080ce.h, ctrl0080gpu.h) */
#define CTL_KGR_GET_INFO 0x20800a2au
#define CTL_KGR_CTX_BUFFERS_INFO 0x20800a32u
#define CTL_FIFO_DEVICE_INFO 0x20801112u
#define CTL_FIFO_NUM_CHANNELS 0x20800a61u
#define CTL_CE_FAULT_MTHD_SIZE 0x20802a08u
#define CTL_GET_CLASSLIST_V2 0x00800292u

/* om/rm.om, om/rmchan.om (9), om/rmctl.om (11..17) */
enum { B_SGSI = 1, B_STATIC = 2, B_ROOT = 3, B_DEVICE = 4, B_SUBDEV = 5, B_VAS = 6, B_TSG = 7,
	B_CTXSHARE = 8, B_CHANNEL = 9, B_COMPUTE = 10, B_CONTROL = 11, B_FAULT_MTHD = 12,
	B_PROMOTE = 13, B_RPDES = 14, B_SCHEDULE = 15, B_FREE = 16, B_UNLOAD = 17 };
/* om/rmchk.om */
enum { C_REPLY = 1, C_VERSION = 2, C_STATIC = 3, C_DEVINFO = 4, C_NCHAN = 5, C_CESIZE = 6,
	C_CTXINFO = 7, C_GRINFO = 8, C_CLASSES = 9 };
/* om/rmarg.om */
enum { A_CHID = 1, A_CHID_REL = 2, A_CHANARG = 3, A_TOKEN = 4, A_SUSPENDED = 5, A_HALTED = 6 };

/* Steps of docs/STAGE-C.md as major * 100 + minor */
enum { S_GUEST = 102, S_STATIC = 104, S_GR = 107, S_FIFO = 108, S_CE = 109, S_CLASS = 110,
	S_ROOT = 202, S_SUBDEV = 203, S_VAS = 206, S_TSG = 208, S_CTX = 209, S_CHAN = 210,
	S_COMPUTE = 211, S_SCHED = 214, S_FREE = 500, S_UNLOAD = 501 };

/* Device-wide RM state: what GSP-RM reported at AGPU_RM_INIT. */
struct agpu_rm {
	u32 h_client, h_device, h_subdevice;	/* GSP-RM's internal client */
	u32 runlist, nchan, fault_bytes, max_subctx;
	u32 main_bytes, main_align, patch_bytes, patch_align, pam_bytes, pam_align;
	u32 compute_class, gpfifo_class;
	u32 next_client;
	u64 chid_bm[64];			/* om/rmarg.om ChID bitmap of GR0's runlist */
	struct agpu_kbuf pam, upam;		/* priv access maps (kgraphics_gm200.c:247-296) */
	bool pam_init;				/* promoted with bInitialize once */
	u64 a[64], b[64];			/* argument words of the Omega units */
};

struct agpu_chan {
	u32 h_tsg, h_ctx, h_chan, h_obj;
	u32 chid, token;
	u32 ring_handle, userd_handle, err_handle;
	struct agpu_kbuf inst, fmb[2], main, patch;
	u8 gsp;			/* GSP objects alive: 1 TSG, 2 ctxshare, 4 channel, 8 compute */
	bool held, chid_set;
};

/* One open file: its own RM client (spec 2.1-2.6). */
struct agpu_fctx {
	u32 h_client, h_device, h_subdevice, h_vas, next;
	u8 gsp;			/* 1 client, 2 device, 4 subdevice, 8 vaspace, 16 PDEs sent */
	bool vas_tree, failed, pam_va_sent;
	struct agpu_kbuf pam_map;	/* this file's mapping of the device priv access map */
	struct agpu_chan *chan[RM_MAX_CHAN];
};

static void st_set(struct agpu_rm_status *st, u32 verdict, u32 code, u32 fn, u32 step)
{
	st->verdict = verdict;
	st->code = code;
	st->fn = fn;
	st->step = step;
}

/* An Omega call that must not refuse: a trap is -EPROTO, a refusal -EINVAL with *code. */
static int om_one(const char *name, const void *unit, u64 a0, u64 a1, u64 a2, u64 *r, u32 *code)
{
	int rc = agpu_om(name, unit, a0, a1, a2, 0, 0, 0, r);

	*code = 0;
	if (rc)
		return rc;
	if (*r >= AGPU_OM_FAIL) {
		*code = *r - AGPU_OM_FAIL;
		return -EINVAL;
	}
	return 0;
}

static int rm_arg(struct agpu_rm *rm, u64 op, u64 *r, u32 *code)
{
	return om_one("rmarg", agpu_om_rmarg, op, (u64)rm->chid_bm, (u64)rm->a, r, code);
}

static int rm_check(struct agpu_dev *d, u64 op, u64 *a, u64 *r, u32 *code)
{
	return om_one("rmchk", agpu_om_rmchk, op, (u64)d->msg_rx, (u64)a, r, code);
}

static void rpc_fail(struct agpu_rm_status *st, int rc, u32 code, u32 fn, u32 step)
{
	if (rc == -EPROTO)
		st_set(st, AGPU_RM_E_REPLY, 1000, fn, step);
	else if (rc == -EINVAL)
		st_set(st, AGPU_RM_E_ARG, code, fn, step);
	else
		st_set(st, AGPU_RM_E_QUEUE, code, fn, step);
}

/*
 * One synchronous RPC: build (op, inputs in a[]), send with sequence d->rpc_seq, then read the
 * status queue until the reply with the same function and sequence arrives. Sequencer requests
 * on the way are run (as stage B does); every other event is read past. The reply stays in
 * d->msg_rx.
 * minpay: payload bytes the reply must hold. 0 or a filled-in st.
 */
static int rm_rpc(struct agpu_dev *d, u64 op, u64 *a, u32 minpay, struct agpu_rm_status *st,
		  u32 step)
{
	const void *unit = op == B_CHANNEL ? agpu_om_rmchan :
			   op >= B_CONTROL ? agpu_om_rmctl : agpu_om_rm;
	ktime_t end;
	u64 r, *b;
	u32 code, fn, seq, ops;
	int rc;

	rc = agpu_om("rm", unit, op, (u64)d->msg_tx, (u64)a, 0, 0, 0, &r);
	if (rc || r >= AGPU_OM_FAIL) {
		st_set(st, rc ? AGPU_RM_E_REPLY : AGPU_RM_E_ARG,
		       rc ? 1000 : (u32)(r - AGPU_OM_FAIL), 0, step);
		return -EINVAL;
	}
	fn = (u32)a[48];
	seq = d->rpc_seq;
	rc = agpu_mq_send(d, fn, (u32)a[49], (u32)a[50], seq, &code);
	if (rc) {
		rpc_fail(st, rc == -EPROTO ? rc : -EIO, code, fn, step);
		return rc;
	}
	d->rpc_seq++;
	end = ktime_add_ms(ktime_get(), RM_RPC_MS);
	for (;;) {
		rc = agpu_mq_recv(d, &code);
		if (rc < 0) {
			rpc_fail(st, rc == -EPROTO ? rc : -EIO, code, fn, step);
			return rc;
		}
		if (rc == 0) {
			if (ktime_after(ktime_get(), end)) {
				st_set(st, AGPU_RM_E_TIMEOUT, 0, fn, step);
				return -ETIMEDOUT;
			}
			usleep_range(50, 100);
			continue;
		}
		b = memset(d->rm->b, 0, sizeof(d->rm->b));
		b[0] = fn;
		b[1] = seq;
		b[2] = minpay;
		rc = agpu_om("rmchk", agpu_om_rmchk, C_REPLY, (u64)d->msg_rx, (u64)b, 0, 0, 0, &r);
		if (rc) {
			rpc_fail(st, rc, 0, fn, step);
			return rc;
		}
		if (r == 0)
			return 0;
		if (r == 2 || r == 3) {
			st_set(st, r == 2 ? AGPU_RM_E_RESULT : AGPU_RM_E_STATUS,
			       (u32)(r == 2 ? b[9] : b[11]),
			       fn, step);
			return -EIO;
		}
		if (r >= AGPU_OM_FAIL) {
			st_set(st, AGPU_RM_E_REPLY, (u32)(r - AGPU_OM_FAIL), fn, step);
			return -EIO;
		}
		/* r == 1: an event or another reply */
		if (b[8] == AGPU_EV_RUN_CPU_SEQUENCER) {
			rc = agpu_seq_run(d, &code, &ops);
			if (rc) {
				st_set(st, AGPU_RM_E_UNSUPPORTED, 100 + code, (u32)b[8], step);
				return -EIO;
			}
		}
	}
}

/* A control on (hc, ho) with zeroed params of psize bytes, first word v0. */
static int rm_ctl(struct agpu_dev *d, u32 hc, u32 ho, u32 cmd, u32 psize, u32 v0,
		  struct agpu_rm_status *st, u32 step)
{
	u64 *a = d->rm->a;

	memset(a, 0, sizeof(d->rm->a));
	a[0] = hc;
	a[1] = ho;
	a[2] = cmd;
	a[3] = psize;
	a[4] = v0;
	return rm_rpc(d, B_CONTROL, a, 40 + psize, st, step);
}

/* Check the reply in d->msg_rx with om/rmchk.om; a refusal becomes verdict vd. */
static int rm_chk(struct agpu_dev *d, u64 op, u32 vd, struct agpu_rm_status *st, u32 step)
{
	u64 r;
	u32 code;
	int rc = rm_check(d, op, d->rm->a, &r, &code);

	if (rc)
		st_set(st, rc == -EINVAL ? vd : AGPU_RM_E_REPLY, rc == -EINVAL ? code : 1000, 0,
		       step);
	return rc;
}

/* Spec 1.7-1.10: GR, FIFO, CE and class-list facts from GSP-RM's internal client. */
static int rm_init_facts(struct agpu_dev *d, struct agpu_rm_status *st)
{
	struct agpu_rm *rm = d->rm;
	u64 *a = rm->a;
	u32 base;
	int rc;

	rc = rm_ctl(d, rm->h_client, rm->h_subdevice, CTL_KGR_GET_INFO, 3712, 0, st, S_GR) ?:
	     rm_chk(d, C_GRINFO, AGPU_RM_E_REPLY, st, S_GR);
	if (rc)
		return rc;
	rm->max_subctx = a[0];
	rc = rm_ctl(d, rm->h_client, rm->h_subdevice, CTL_KGR_CTX_BUFFERS_INFO, 1664, 0, st, S_GR);
	if (rc)
		return rc;
	a[10] = rm->max_subctx;
	rc = rm_chk(d, C_CTXINFO, AGPU_RM_E_REPLY, st, S_GR);
	if (rc)
		return rc;
	rm->main_bytes = a[0];
	rm->main_align = a[1];
	rm->patch_bytes = a[2];
	rm->patch_align = a[3];
	rm->pam_bytes = a[4];
	rm->pam_align = a[5];
	/* the device-info table, 32 entries per call, to GR0 or the end (kernel_fifo.c:2046) */
	for (base = 0; base < 256; base += 32) {
		rc = rm_ctl(d, rm->h_client, rm->h_subdevice, CTL_FIFO_DEVICE_INFO, 3212, base, st,
			    S_FIFO);
		if (rc)
			return rc;
		a[6] = base;
		rc = rm_chk(d, C_DEVINFO, AGPU_RM_E_REPLY, st, S_FIFO);
		if (rc)
			return rc;
		if (a[0] || !a[2])
			break;
	}
	if (!a[0]) {
		st_set(st, AGPU_RM_E_UNSUPPORTED, 1, 0, S_FIFO);	/* no host-driven GR0 */
		return -ENODEV;
	}
	rm->runlist = a[1];
	rc = rm_ctl(d, rm->h_client, rm->h_subdevice, CTL_FIFO_NUM_CHANNELS, 8, rm->runlist, st,
		    S_FIFO);
	if (rc)
		return rc;
	a[0] = rm->runlist;
	rc = rm_chk(d, C_NCHAN, AGPU_RM_E_REPLY, st, S_FIFO);
	if (rc)
		return rc;
	rm->nchan = a[1];
	rc = rm_ctl(d, rm->h_client, rm->h_subdevice, CTL_CE_FAULT_MTHD_SIZE, 4, 0, st, S_CE) ?:
	     rm_chk(d, C_CESIZE, AGPU_RM_E_REPLY, st, S_CE);
	if (rc)
		return rc;
	rm->fault_bytes = a[0];
	rc = rm_ctl(d, rm->h_client, rm->h_device, CTL_GET_CLASSLIST_V2, 804, 0, st, S_CLASS) ?:
	     rm_chk(d, C_CLASSES, AGPU_RM_E_UNSUPPORTED, st, S_CLASS);
	if (rc)
		return rc;
	rm->compute_class = a[0];
	rm->gpfifo_class = a[1];
	return 0;
}

void agpu_rm_init(struct agpu_dev *d, struct agpu_rm_init *o)
{
	struct agpu_rm_status *st = &o->st;
	struct agpu_rm *rm;
	u64 *a;
	int rc;

	st_set(st, 0, 0, 0, 0);
	if (d->info.state != AGPU_STATE_GSP_UP || d->rm) {
		st_set(st, AGPU_RM_E_STATE, d->info.state, 0, 0);
		return;
	}
	rm = kzalloc_obj(*rm);
	if (!rm) {
		st_set(st, AGPU_RM_E_NOMEM, 0, 0, 0);
		return;
	}
	d->rm = rm;
	a = rm->a;
	/* 1.2: the RPC version handshake; another version than 580.173.02's is refused */
	memset(a, 0, sizeof(rm->a));
	rc = rm_rpc(d, B_SGSI, a, 8, st, S_GUEST);
	if (!rc) {
		rc = rm_chk(d, C_VERSION, AGPU_RM_E_VERSION, st, S_GUEST);
		o->vgx_major = a[0];
		o->vgx_minor = a[1];
	}
	if (rc)
		goto failed;
	/* 1.4: GspStaticConfigInfo; 1.6: adopt the internal handles (no RPC) */
	memset(a, 0, sizeof(rm->a));
	rc = rm_rpc(d, B_STATIC, a, 1792, st, S_STATIC);
	if (rc)
		goto failed;
	memcpy(o->static_info, d->msg_rx + 80, sizeof(o->static_info));
	rc = rm_chk(d, C_STATIC, AGPU_RM_E_STATIC, st, S_STATIC);
	o->h_client = a[0];
	o->h_device = a[1];
	o->h_subdevice = a[2];
	if (rc)
		goto failed;
	rm->h_client = a[0];
	rm->h_device = a[1];
	rm->h_subdevice = a[2];
	rc = rm_init_facts(d, st);
	o->gr_runlist = rm->runlist;
	o->num_channels = rm->nchan;
	o->fault_mthd_bytes = rm->fault_bytes;
	o->gr_main_ctx_bytes = rm->main_bytes;
	o->max_subctx = rm->max_subctx;
	o->compute_class = rm->compute_class;
	o->gpfifo_class = rm->gpfifo_class;
	if (rc)
		goto failed;
	d->info.state = AGPU_STATE_RM_READY;
	st_set(st, AGPU_RM_OK, 0, 0, 0);
	dev_info(&d->pdev->dev, "stage C ready: runlist %u channels %u compute %04x gpfifo %04x main ctx %u\n",
		 rm->runlist, rm->nchan, rm->compute_class, rm->gpfifo_class, rm->main_bytes);
	return;
failed:
	/* The queues may hold a half-read exchange: no later RPC is trusted on this load. */
	d->info.state = AGPU_STATE_FAILED;
	dev_warn(&d->pdev->dev, "stage C init stopped: verdict %u code %u fn %u step %u\n",
		 st->verdict,
		 st->code, st->fn, st->step);
}

static u32 fx_handle(struct agpu_fctx *fx)
{
	return RM_OBJ_BASE + ++fx->next;
}

static int rm_free(struct agpu_dev *d, u32 hc, u32 h, struct agpu_rm_status *st)
{
	u64 *a = d->rm->a;

	memset(a, 0, sizeof(d->rm->a));
	a[0] = hc;
	a[1] = h;
	return rm_rpc(d, B_FREE, a, 16, st, S_FREE);
}

/* An allocation with inputs a0..a3 (om/rm.om ops 3..8, 10). */
static int rm_alloc4(struct agpu_dev *d, u64 op, u64 a0, u64 a1, u64 a2, u64 a3,
		     struct agpu_rm_status *st, u32 step)
{
	u64 *a = d->rm->a;

	memset(a, 0, sizeof(d->rm->a));
	a[0] = a0;
	a[1] = a1;
	a[2] = a2;
	a[3] = a3;
	return rm_rpc(d, op, a, 32, st, step);
}

int agpu_rm_fctx_get(struct agpu_file *f, struct agpu_rm_status *st)
{
	struct agpu_dev *d = f->d;
	struct agpu_fctx *fx = f->rm;
	u64 *a;
	int rc;

	if (d->info.state != AGPU_STATE_RM_READY || !d->rm) {
		st_set(st, AGPU_RM_E_STATE, d->info.state, 0, 0);
		return -EINVAL;
	}
	if (fx && fx->failed) {
		st_set(st, AGPU_RM_E_STATE, 100, 0, 0);
		return -EINVAL;
	}
	if (fx && (fx->gsp & 16))
		return 0;
	if (!fx) {
		fx = kzalloc_obj(*fx);
		if (!fx) {
			st_set(st, AGPU_RM_E_NOMEM, 0, 0, 0);
			return -ENOMEM;
		}
		f->rm = fx;
		fx->h_client = RM_CLIENT_BASE + (d->rm->next_client++ & 0x7ffff);
		fx->h_device = fx_handle(fx);
		fx->h_subdevice = fx_handle(fx);
		fx->h_vas = fx_handle(fx);
	}
	fx->failed = true;	/* until every step below is done */
	rc = agpu_vas_create(f);
	if (rc) {
		st_set(st, rc == -ENOMEM ? AGPU_RM_E_NOMEM : AGPU_RM_E_STATE, -rc, 0, S_VAS);
		return rc;
	}
	fx->vas_tree = true;
	rc = rm_alloc4(d, B_ROOT, fx->h_client, task_tgid_nr(current), 0, 0, st, S_ROOT);
	if (rc)
		return rc;
	fx->gsp |= 1;
	rc = rm_alloc4(d, B_DEVICE, fx->h_client, fx->h_device, 0, 0, st, S_ROOT);
	if (rc)
		return rc;
	fx->gsp |= 2;
	rc = rm_alloc4(d, B_SUBDEV, fx->h_client, fx->h_device, fx->h_subdevice, 0, st, S_SUBDEV);
	if (rc)
		return rc;
	fx->gsp |= 4;
	/* 2.6 (a) then (b): GSP_RM_ALLOC of the vaspace, then GSP's [4 GiB, 4.5 GiB) PDEs */
	rc = rm_alloc4(d, B_VAS, fx->h_client, fx->h_device, fx->h_vas, RM_VA_BYTES, st, S_VAS);
	if (rc)
		return rc;
	fx->gsp |= 8;
	rc = agpu_vas_reserved_pdes(f, d->msg_tx + 120, AGPU_RPDES_BYTES);
	if (rc) {
		st_set(st, AGPU_RM_E_STATE, -rc, 0, S_VAS);
		return rc;
	}
	a = d->rm->a;
	memset(a, 0, sizeof(d->rm->a));
	a[0] = fx->h_client;
	a[1] = fx->h_vas;
	a[2] = fx->h_subdevice;
	rc = rm_rpc(d, B_RPDES, a, 40, st, S_VAS);
	if (rc)
		return rc;
	fx->gsp |= 16;
	fx->failed = false;
	return 0;
}

/* ---- channels ---- */

static void chan_kbufs_free(struct agpu_file *f, struct agpu_chan *c)
{
	struct agpu_dev *d = f->d;

	agpu_vas_unmap_kbuf(f, &c->main);
	agpu_vas_unmap_kbuf(f, &c->patch);
	agpu_kbuf_free(d, &c->main);
	agpu_kbuf_free(d, &c->patch);
	agpu_kbuf_free(d, &c->inst);
	agpu_kbuf_free(d, &c->fmb[0]);
	agpu_kbuf_free(d, &c->fmb[1]);
}

/*
 * Spec 5.1-5.3: compute object, channel, ctxshare, TSG. GSP may still DMA into the buffers of an
 * object whose FREE failed: those are then kept (leaked) and the file is marked failed.
 */
static int chan_teardown(struct agpu_file *f, struct agpu_chan *c, struct agpu_rm_status *st)
{
	struct agpu_dev *d = f->d;
	struct agpu_fctx *fx = f->rm;
	static const u8 bit[4] = { 8, 4, 2, 1 };
	u32 h[4] = { c->h_obj, c->h_chan, c->h_ctx, c->h_tsg };
	u32 code;
	u64 r;
	int i, rc = 0;

	for (i = 0; i < 4 && !rc; i++) {
		if (!(c->gsp & bit[i]))
			continue;
		rc = rm_free(d, fx->h_client, h[i], st);
		if (!rc)
			c->gsp &= ~bit[i];
	}
	if (rc) {
		fx->failed = true;
		return rc;	/* buffers, ChID and holds stay: GSP may still use them */
	}
	if (c->held) {
		agpu_mem_hold(f, c->ring_handle, -1);
		agpu_mem_hold(f, c->userd_handle, -1);
		agpu_mem_hold(f, c->err_handle, -1);
		c->held = false;
	}
	if (c->chid_set) {
		d->rm->a[0] = d->rm->nchan;
		d->rm->a[1] = c->chid;
		rm_arg(d->rm, A_CHID_REL, &r, &code);
		c->chid_set = false;
	}
	chan_kbufs_free(f, c);
	return 0;
}

static int kbuf_aligned(struct agpu_dev *d, size_t bytes, u32 align, struct agpu_kbuf *b,
			struct agpu_rm_status *st)
{
	int rc = agpu_kbuf_alloc(d, bytes, b);

	if (rc) {
		st_set(st, AGPU_RM_E_NOMEM, 0, 0, S_CHAN);
		return rc;
	}
	if (align > 1 && (b->iova & (align - 1))) {
		/* GSP's alignment is above what the allocation gave */
		st_set(st, AGPU_RM_E_UNSUPPORTED, 2, 0, S_COMPUTE);
		return -EINVAL;
	}
	return 0;
}

/* The device-wide priv access maps (allocated once, kgraphicsAllocGrGlobalCtxBuffers_GM200). */
static int pam_get(struct agpu_dev *d, struct agpu_rm_status *st)
{
	struct agpu_rm *rm = d->rm;
	int rc;

	if (!rm->pam_bytes || rm->pam.cpu)
		return 0;
	rc = kbuf_aligned(d, rm->pam_bytes, rm->pam_align, &rm->pam, st) ?:
	     kbuf_aligned(d, rm->pam_bytes, rm->pam_align, &rm->upam, st);
	if (rc) {
		agpu_kbuf_free(d, &rm->pam);
		agpu_kbuf_free(d, &rm->upam);
	}
	return rc;
}

/*
 * 2.11: PROMOTE_CTX for a compute object on an RM-owned address space (kgrobjPromoteContext,
 * kgrobjGetPromoteIds_FWCLIENT non-3D set; kgrctxPrepareInitializeCtxBuffer and
 * kgrctxPreparePromoteCtxBuffer): MAIN and PATCH initialized and promoted at their VA; PM not
 * allocated (kgrctxShouldPreAllocPmBuffer_PF is false for an RM-owned VAS); FECS_EVENT not
 * allocated (ctxsw logging off); PRIV_ACCESS_MAP mapped (user privilege,
 * kgrctxGetRegisterAccessMapId),
 * initialized once per device, its VA promoted once per address space;
 * UNRESTRICTED_PRIV_ACCESS_MAP initialized once per device, never mapped.
 */
static int promote(struct agpu_file *f, struct agpu_chan *c, struct agpu_rm_status *st)
{
	struct agpu_dev *d = f->d;
	struct agpu_rm *rm = d->rm;
	struct agpu_fctx *fx = f->rm;
	u64 *a = rm->a, *e;
	u32 n = 0;
	int rc;

	memset(a, 0, sizeof(rm->a));
	a[0] = fx->h_client;
	a[1] = fx->h_subdevice;
	a[2] = c->h_chan;
	e = a + 8;
	e[0] = c->main.iova; e[1] = c->main.gpu_va; e[2] = c->main.size; e[4] = 0; e[5] = 1;
	e += 6;
	e[0] = c->patch.iova; e[1] = c->patch.gpu_va; e[2] = rm->patch_bytes; e[4] = 2; e[5] = 1;
	e += 6;
	n = 2;
	if (rm->pam.cpu && (!rm->pam_init || !fx->pam_va_sent)) {
		e[0] = rm->pam_init ? 0 : rm->pam.iova;
		e[1] = fx->pam_map.gpu_va;
		e[2] = rm->pam_init ? 0 : rm->pam_bytes;
		e[4] = 10;
		e[5] = rm->pam_init ? 0 : 1;
		e += 6;
		n++;
	}
	if (rm->upam.cpu && !rm->pam_init) {
		/* bInitialize, bNonmapped */
		e[0] = rm->upam.iova;
		e[2] = rm->pam_bytes;
		e[4] = 11;
		e[5] = 3;
		n++;
	}
	a[3] = n;
	rc = rm_rpc(d, B_PROMOTE, a, 40 + 560, st, S_COMPUTE);
	if (rc)
		return rc;
	rm->pam_init = true;
	fx->pam_va_sent = true;
	return 0;
}

static int chan_buffers(struct agpu_file *f, struct agpu_chan *c, struct agpu_rm_status *st)
{
	struct agpu_dev *d = f->d;
	struct agpu_rm *rm = d->rm;
	struct agpu_fctx *fx = f->rm;
	int rc;

	rc = kbuf_aligned(d, 4096, 4096, &c->inst, st) ?:
	     kbuf_aligned(d, rm->fault_bytes, 4096, &c->fmb[0], st) ?:
	     kbuf_aligned(d, rm->fault_bytes, 4096, &c->fmb[1], st) ?:
	     kbuf_aligned(d, rm->main_bytes, rm->main_align, &c->main, st) ?:
	     kbuf_aligned(d, rm->patch_bytes, rm->patch_align, &c->patch, st) ?:
	     pam_get(d, st);
	if (rc)
		return rc;
	rc = agpu_vas_map_kbuf(f, &c->main, false) ?: agpu_vas_map_kbuf(f, &c->patch, false);
	if (!rc && rm->pam.cpu && !fx->pam_map.gpu_va) {
		fx->pam_map = rm->pam;
		fx->pam_map.gpu_va = 0;
		rc = agpu_vas_map_kbuf(f, &fx->pam_map, false);
		if (rc)
			memset(&fx->pam_map, 0, sizeof(fx->pam_map));
	}
	if (rc)
		st_set(st, rc == -ENOMEM ? AGPU_RM_E_NOMEM : AGPU_RM_E_STATE, -rc, 0, S_COMPUTE);
	return rc;
}

/* Spec 2.10: NV_CHANNEL_ALLOC_PARAMS with the CPU-filled memory descriptors (om/rmchan.om). */
static int chan_alloc(struct agpu_file *f, struct agpu_chan *c, struct agpu_chan_create *in,
		      const dma_addr_t *iova, u64 ring_va, struct agpu_rm_status *st)
{
	struct agpu_dev *d = f->d;
	struct agpu_fctx *fx = f->rm;
	u64 *a = d->rm->a;

	memset(a, 0, sizeof(d->rm->a));
	a[0] = fx->h_client;
	a[1] = c->h_tsg;
	a[2] = c->h_chan;
	a[3] = d->rm->gpfifo_class;
	a[4] = c->err_handle;
	a[5] = ring_va;
	a[6] = in->entries;
	a[7] = c->chid;
	a[8] = c->h_ctx;
	a[9] = fx->h_vas;
	a[10] = c->userd_handle;
	a[11] = in->userd_off;
	a[12] = c->inst.iova;
	a[13] = iova[1];
	a[14] = 0;		/* user memory is CPU-cached sysmem (NV_MEMORY_CACHED) */
	a[15] = c->fmb[0].iova;
	a[16] = d->rm->fault_bytes;
	a[17] = iova[2];
	a[18] = RM_ERR_BYTES;
	a[19] = 0;
	a[20] = task_tgid_nr(current);
	a[21] = 0;		/* NV_KERNELCHANNEL_ALLOC_INTERNALFLAGS_PRIVILEGE_USER */
	return rm_rpc(d, B_CHANNEL, a, 32, st, S_CHAN);
}

static int chan_build(struct agpu_file *f, struct agpu_chan *c, struct agpu_chan_create *in,
		      struct agpu_rm_status *st)
{
	struct agpu_dev *d = f->d;
	struct agpu_rm *rm = d->rm;
	struct agpu_fctx *fx = f->rm;
	dma_addr_t iova[3];
	u64 gva[3], r, *a = rm->a;
	u32 code;
	int rc;

	rc = agpu_mem_iova(f, in->ring_handle, in->ring_off, (u64)in->entries * 8, &iova[0],
			   &gva[0]) ?:
	     agpu_mem_iova(f, in->userd_handle, in->userd_off, 512, &iova[1], &gva[1]) ?:
	     agpu_mem_iova(f, in->err_handle, 0, RM_ERR_BYTES, &iova[2], &gva[2]);
	if (rc) {
		st_set(st, AGPU_RM_E_ARG, 100 + (u32)-rc, 0, S_CHAN);
		return rc;
	}
	a[0] = rm->nchan;
	rc = rm_arg(rm, A_CHID, &r, &code);
	if (rc) {
		st_set(st, AGPU_RM_E_UNSUPPORTED, 10 + code, 0, S_CHAN);
		return rc;
	}
	c->chid = r;
	c->chid_set = true;
	rc = chan_buffers(f, c, st);
	if (rc)
		return rc;
	/* 2.8 TSG, then its fault-method buffers (one per runqueue, 2 on GB10B) */
	rc = rm_alloc4(d, B_TSG, fx->h_client, fx->h_device, c->h_tsg, fx->h_vas, st, S_TSG);
	if (rc)
		return rc;
	c->gsp |= 1;
	memset(a, 0, sizeof(rm->a));
	a[0] = fx->h_client;
	a[1] = c->h_tsg;
	a[2] = c->fmb[0].iova;
	a[3] = rm->fault_bytes;
	a[4] = c->fmb[1].iova;
	a[5] = rm->fault_bytes;
	rc = rm_rpc(d, B_FAULT_MTHD, a, 40 + 88, st, S_TSG);
	if (rc)
		return rc;
	/* 2.9 context share, 2.10 channel */
	rc = rm_alloc4(d, B_CTXSHARE, fx->h_client, c->h_tsg, c->h_ctx, fx->h_vas, st, S_CTX);
	if (rc)
		return rc;
	c->gsp |= 2;
	rc = chan_alloc(f, c, in, iova, gva[0], st);
	if (rc)
		return rc;
	c->gsp |= 4;
	rc = agpu_mem_hold(f, c->ring_handle, 1) ?: agpu_mem_hold(f, c->userd_handle, 1) ?:
	     agpu_mem_hold(f, c->err_handle, 1);
	if (rc) {
		st_set(st, AGPU_RM_E_STATE, -rc, 0, S_CHAN);
		return rc;
	}
	c->held = true;
	/* 2.11: promote, then the compute object */
	rc = promote(f, c, st);
	if (rc)
		return rc;
	rc = rm_alloc4(d, B_COMPUTE, fx->h_client, c->h_chan, c->h_obj, rm->compute_class, st,
		       S_COMPUTE);
	if (rc)
		return rc;
	c->gsp |= 8;
	/* 2.12: the token on the CPU; 2.14: schedule the TSG */
	a[0] = rm->runlist;
	a[1] = c->chid;
	a[2] = (((d->info.boot42 >> 24) & 0x3fu) == 0x1bu) ? 1 : 0;
	rc = rm_arg(rm, A_TOKEN, &r, &code);
	if (rc) {
		st_set(st, AGPU_RM_E_UNSUPPORTED, 20 + code, 0, S_SCHED);
		return rc;
	}
	c->token = r;
	memset(a, 0, sizeof(rm->a));
	a[0] = fx->h_client;
	a[1] = c->h_tsg;
	return rm_rpc(d, B_SCHEDULE, a, 43, st, S_SCHED);
}

void agpu_chan_create(struct agpu_file *f, struct agpu_chan_create *in)
{
	struct agpu_rm_status *st = &in->st;
	struct agpu_dev *d = f->d;
	struct agpu_fctx *fx;
	struct agpu_chan *c;
	struct agpu_rm_status st2;
	u32 code, slot;
	u64 r;
	int rc;

	st_set(st, 0, 0, 0, 0);
	if (agpu_rm_fctx_get(f, st))
		return;
	fx = f->rm;
	d->rm->a[0] = in->entries;
	d->rm->a[1] = in->ring_off;
	d->rm->a[2] = in->userd_off;
	d->rm->a[3] = in->ring_handle;
	d->rm->a[4] = in->userd_handle;
	d->rm->a[5] = in->err_handle;
	rc = rm_arg(d->rm, A_CHANARG, &r, &code);
	if (rc) {
		st_set(st, AGPU_RM_E_ARG, code, 0, S_CHAN);
		return;
	}
	for (slot = 0; slot < RM_MAX_CHAN && fx->chan[slot]; slot++)
		;
	if (slot == RM_MAX_CHAN) {
		st_set(st, AGPU_RM_E_STATE, 101, 0, S_CHAN);
		return;
	}
	c = kzalloc_obj(*c);
	if (!c) {
		st_set(st, AGPU_RM_E_NOMEM, 0, 0, S_CHAN);
		return;
	}
	c->h_tsg = fx_handle(fx);
	c->h_ctx = fx_handle(fx);
	c->h_chan = fx_handle(fx);
	c->h_obj = fx_handle(fx);
	c->ring_handle = in->ring_handle;
	c->userd_handle = in->userd_handle;
	c->err_handle = in->err_handle;
	fx->chan[slot] = c;
	rc = chan_build(f, c, in, st);
	if (rc) {
		/* undo what GSP accepted; the first failure stays the reported one */
		if (!chan_teardown(f, c, &st2)) {
			fx->chan[slot] = NULL;
			kfree(c);
		}
		return;
	}
	in->chid = c->chid;
	in->token = c->token;
	in->doorbell_off = AGPU_MMAP_DOORBELL;
	in->channel = slot + 1;
	in->compute_class = d->rm->compute_class;
	st_set(st, AGPU_RM_OK, 0, 0, 0);
}

void agpu_chan_destroy(struct agpu_file *f, struct agpu_chan_destroy *in)
{
	struct agpu_fctx *fx = f->rm;
	struct agpu_chan *c;

	st_set(&in->st, 0, 0, 0, 0);
	if (!fx || in->reserved || !in->channel || in->channel > RM_MAX_CHAN ||
	    !fx->chan[in->channel - 1]) {
		st_set(&in->st, AGPU_RM_E_ARG, 1, 0, 0);
		return;
	}
	c = fx->chan[in->channel - 1];
	if (chan_teardown(f, c, &in->st))
		return;
	fx->chan[in->channel - 1] = NULL;
	kfree(c);
	st_set(&in->st, AGPU_RM_OK, 0, 0, 0);
}

/* Spec 5: channels, then vaspace, subdevice, device, client; then the page-table tree. */
void agpu_rm_file_release(struct agpu_file *f)
{
	struct agpu_dev *d = f->d;
	struct agpu_fctx *fx = f->rm;
	struct agpu_rm_status st;
	bool stuck = false;
	u32 h[4];
	static const u8 bit[4] = { 8, 4, 2, 1 };
	int i;

	if (!fx)
		return;
	if (d->info.state == AGPU_STATE_RM_READY) {
		for (i = 0; i < RM_MAX_CHAN; i++) {
			if (!fx->chan[i])
				continue;
			if (chan_teardown(f, fx->chan[i], &st)) {
				/* GSP may still use this channel's memory: keep it */
				stuck = true;
				continue;
			}
			kfree(fx->chan[i]);
			fx->chan[i] = NULL;
		}
		h[0] = fx->h_vas;
		h[1] = fx->h_subdevice;
		h[2] = fx->h_device;
		h[3] = fx->h_client;
		for (i = 0; i < 4 && !stuck; i++) {
			if (!(fx->gsp & bit[i]))
				continue;
			if (rm_free(d, fx->h_client, h[i], &st))
				stuck = true;
			else
				fx->gsp &= ~bit[i];
		}
	} else {
		/* no RPC possible: GSP keeps whatever it had */
		stuck = fx->gsp || fx->vas_tree;
		for (i = 0; i < RM_MAX_CHAN; i++)
			stuck |= !!fx->chan[i];
	}
	agpu_vas_unmap_kbuf(f, &fx->pam_map);
	if (fx->vas_tree) {
		if (stuck)
			agpu_vas_abandon(f);
		else
			agpu_vas_destroy(f);
	}
	if (stuck) {
		dev_warn(&d->pdev->dev, "file release: GSP objects of client %08x kept; memory kept until reboot\n",
			 fx->h_client);
		/* the channel records and their buffers are leaked on purpose */
	}
	kfree(fx);
	f->rm = NULL;
}

/* Spec 5, driver unload (kgspUnloadRm_IMPL): UNLOADING_GUEST_DRIVER, MAILBOX0 0x80000000, halt. */
void agpu_rm_unload(struct agpu_dev *d)
{
	struct agpu_rm_status st;
	u64 r;
	u32 code, i, mb = 0, ctl = 0;
	int rc;

	if (d->info.state == AGPU_STATE_GSP_UP || d->info.state == AGPU_STATE_RM_READY) {
		if (!d->rm) {
			d->rm = kzalloc_obj(*d->rm);
			if (!d->rm)
				return;
		}
		memset(d->rm->a, 0, sizeof(d->rm->a));
		rc = rm_rpc(d, B_UNLOAD, d->rm->a, 0, &st, S_UNLOAD);
		if (rc) {
			dev_warn(&d->pdev->dev, "unload: UNLOADING_GUEST_DRIVER verdict %u code %u\n",
				 st.verdict, st.code);
		} else {
			for (i = 0; i < RM_UNLOAD_POLLS; i++) {
				mb = ioread32(d->bar0 + RM_GSP_MAILBOX0);
				d->rm->a[0] = mb;
				if (!rm_arg(d->rm, A_SUSPENDED, &r, &code) && r == 1)
					break;
				udelay(1);
			}
			for (i = 0; i < RM_UNLOAD_POLLS; i++) {
				ctl = ioread32(d->bar0 + RM_GSP_CPUCTL);
				d->rm->a[0] = ctl;
				if (!rm_arg(d->rm, A_HALTED, &r, &code) && r == 1)
					break;
				udelay(1);
			}
			dev_info(&d->pdev->dev, "unload: MAILBOX0 %08x CPUCTL %08x\n", mb, ctl);
		}
		d->info.state = AGPU_STATE_FAILED;
	}
	/*
	 * The priv access maps are kept (leaked) once a register was written: GSP may have DMA'd
	 * into them (the agpu_free_mem rule). The CPU-only state goes.
	 */
	if (d->rm && !d->touched) {
		agpu_kbuf_free(d, &d->rm->pam);
		agpu_kbuf_free(d, &d->rm->upam);
	}
	kfree(d->rm);
	d->rm = NULL;
}

/*
 * Spec 4: BAR0 [0xbb0000, +64 KiB), the usermode page with the doorbell at +0x90; uncached device
 * memory, never executable, never dumped.
 */
int agpu_doorbell_mmap(struct agpu_file *f, struct vm_area_struct *vma)
{
	struct agpu_dev *d = f->d;
	phys_addr_t pa;

	if (d->info.state != AGPU_STATE_RM_READY)
		return -EINVAL;
	if (vma->vm_end - vma->vm_start != AGPU_USERMODE_BYTES || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	if (AGPU_USERMODE_BAR0 + AGPU_USERMODE_BYTES > d->bar0_bytes)
		return -EINVAL;
	pa = pci_resource_start(d->pdev, 0) + AGPU_USERMODE_BAR0;
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	vm_flags_clear(vma, VM_MAYEXEC);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start, pa >> PAGE_SHIFT, AGPU_USERMODE_BYTES,
				  vma->vm_page_prot);
}
