/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* aien_gpu.h -- module-internal declarations. */
#ifndef AIEN_GPU_H
#define AIEN_GPU_H

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include "aien_gpu_uapi.h"

#define AGPU_NAME "aien-gpu"
/* NVIDIA's signed GSP-RM, on loan (docs/BOUNDARY.md) */
#define AGPU_FW "nvidia/580.173.02/gsp_ga10x.bin"
#define AGPU_FW_BYTES 75012080u
#define AGPU_FW_SHA256 "32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963"
#define AGPU_DMA_BITS 40	/* nvidia.ko logs "Using 40-bit DMA addresses" for this GPU */
#define AGPU_PMC_BOOT_0 0x00000000u	/* nv_ref.h, NV_PMC_BOOT_0 */
#define AGPU_PMC_BOOT_42 0x00000a00u	/* nv_ref.h:150, NV_PMC_BOOT_42 */
/* GSP QUEUE_HEAD(0): command-queue notify, kgspSetCmdQueueHead_TU102 */
#define AGPU_GSP_QHEAD0 0x00110c00u
#define AGPU_MQ_BYTES 262144u		/* GSP_MQ_QUEUE_SIZE */
/* GSP_ELEM_SIZE_MAX: the largest message, staging buffer size */
#define AGPU_MSG_BYTES 65536u
#define AGPU_SEQ_MAX_OPS 2048u
#define AGPU_OM_FAIL 1000000u		/* Omega unit refusals: [FAIL, FAIL_MAX) */
#define AGPU_OM_FAIL_MAX 1000100u

/* GSP events read from the status queue (rpc_global_enums.h, gb10_gsp_boot.h) */
#define AGPU_EV_INIT_DONE 0x1001u
#define AGPU_EV_RUN_CPU_SEQUENCER 0x1002u

/* The Omega units, kmod/agpu_om.S */
u64 agpu_om_enter(const void *entry, const u64 args[6], u64 *status);
extern const char agpu_om_ident[], agpu_om_boot[], agpu_om_seq[], agpu_om_seal[], agpu_om_mqput[],
	agpu_om_mqget[], agpu_om_abi[];

/*
 * One call into an Omega unit. A trap (status != 0) is logged and returned as -EPROTO: the units
 * are written so that no input reaches a trap, so a trap means a defect, and the device stops.
 */
int agpu_om(const char *unit, const void *entry, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
	    u64 *result);

struct agpu_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	u64 bar0_bytes;
	struct mutex lock;		/* every chip operation, the queues and the state */
	struct agpu_info info;
	void *arena;			/* boot arena, dma-coherent, mmap offset 0 */
	dma_addr_t arena_iova;
	size_t arena_bytes;
	void *heap;			/* GSP system-memory heap, dma-coherent, device only */
	dma_addr_t heap_iova;
	size_t heap_bytes;
	/* a register was written: keep all device memory until reboot */
	bool touched;
	/* message queue, valid from AGPU_STATE_GSP_UP */
	u8 *cmdq, *statq;
	u32 tx_seq, rx_seq, rpc_seq;
	u8 *msg_tx, *msg_rx;		/* AGPU_MSG_BYTES staging buffers (vmalloc) */
	u64 *seq_ops;			/* 6 * AGPU_SEQ_MAX_OPS */
	struct agpu_boot_out boot;	/* last stage B verdict */
	/* stage C device state, kmod/aien_gpu_rm.c (NULL before RM_INIT) */
	struct agpu_rm *rm;
	struct agpu_gmmu *gmmu;		/* GPU page-table state, kmod/aien_gpu_mem.c */
};

/* mmap offsets of /dev/aien-gpu (bytes; vm_pgoff is this >> PAGE_SHIFT) */
#define AGPU_MMAP_ARENA 0ull			/* boot arena, root, before AGPU_BOOT only */
#define AGPU_MMAP_DOORBELL BIT_ULL(40)	/* BAR0 usermode page, 64 KiB, uncached */
#define AGPU_MMAP_MEM_BASE (2ull << 40)	/* + handle << 36: one AGPU_MEM_ALLOC object */
/* 0xb80000 + NV_VIRTUAL_FUNCTION 0x30000 (spec section 4) */
#define AGPU_USERMODE_BAR0 0x00bb0000u
#define AGPU_USERMODE_BYTES 0x10000u

/*
 * One open file of /dev/aien-gpu = one RM client of its own (handles never shared between
 * files).
 */
struct agpu_file {
	struct agpu_dev *d;
	/* per-file RM objects, kmod/aien_gpu_rm.c (NULL until first use) */
	struct agpu_fctx *rm;
	/* per-file memory objects and address space, kmod/aien_gpu_mem.c */
	struct agpu_fmem *mem;
};

void agpu_boot(struct agpu_dev *d, struct agpu_boot *b);	/* d->lock held */
/* Read one status-queue message into d->msg_rx: 1 read, 0 empty, negative on a refusal (*code). */
int agpu_mq_recv(struct agpu_dev *d, u32 *code);
/* Seal d->msg_tx (payload at byte 80) and put it on the command queue, then notify GSP. */
int agpu_mq_send(struct agpu_dev *d, u32 fn, u32 rpclen, u32 placed, u32 seq, u32 *code);
/*
 * Run a sequencer request held in d->msg_rx. 0 done, else a verdict (AGPU_BOOT_SEQ_*) with
 * *code.
 */
int agpu_seq_run(struct agpu_dev *d, u32 *code, u32 *ops);

/*
 * Stage C and memory handlers take kernel copies of the argument blocks: main.c copies in before
 * taking d->lock and copies out after dropping it, so d->lock is never held across a user-memory
 * fault (mmap takes d->lock under mmap_lock; a fault under d->lock would invert that order).
 */
/* ---- stage C, kmod/aien_gpu_rm.c (all called with d->lock held) ---- */
void agpu_rm_init(struct agpu_dev *d, struct agpu_rm_init *a);
void agpu_chan_create(struct agpu_file *f, struct agpu_chan_create *a);
void agpu_chan_destroy(struct agpu_file *f, struct agpu_chan_destroy *a);
/* Create this file's client, device, subdevice and address space on first use (spec 2.1-2.6). */
int agpu_rm_fctx_get(struct agpu_file *f, struct agpu_rm_status *st);
void agpu_rm_file_release(struct agpu_file *f);	/* channels and RM objects, in the spec 5 order */
/* UNLOADING_GUEST_DRIVER at remove, if GSP is up */
void agpu_rm_unload(struct agpu_dev *d);
int agpu_doorbell_mmap(struct agpu_file *f, struct vm_area_struct *vma);

/* ---- GPU memory and page tables, kmod/aien_gpu_mem.c (all called with d->lock held) ---- */
void agpu_mem_alloc(struct agpu_file *f, struct agpu_mem_alloc *a);
void agpu_mem_free(struct agpu_file *f, struct agpu_mem_free *a);
int agpu_mem_mmap(struct agpu_file *f, struct vm_area_struct *vma);
void agpu_mem_file_release(struct agpu_file *f);

#endif
