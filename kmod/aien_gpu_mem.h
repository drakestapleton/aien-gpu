/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/*
 * aien_gpu_mem.h -- GPU memory and page tables (kmod/aien_gpu_mem.c, om/gmmu.om, om/memchk.om,
 * om/rpdes.om). Route B of docs/STAGE-C.md: an RM-managed GPU address space per file whose page
 * tables this module writes (ver3 format, 5 levels, 57-bit VA; kgmmuFmtInitLevels_GB10X), with
 * [4 GiB, 4.5 GiB) handed to GSP-RM by NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES.
 * Every function here is called with d->lock held.
 * Status: IMPLEMENTED, host-tested encoders and checks; hardware NOT_RUN.
 */
#ifndef AIEN_GPU_MEM_H
#define AIEN_GPU_MEM_H

#include <linux/bits.h>
#include <linux/types.h>

struct agpu_dev;
struct agpu_file;

/* The Omega units of this file (kmod/agpu_om.S) */
extern const char agpu_om_gmmu[], agpu_om_memchk[], agpu_om_rpdes[];

/* SPLIT_VAS_SERVER_RM_MANAGED_VA_START and _SIZE, generated/g_gpu_vaspace_nvoc.h:99-100 */
#define AGPU_GSP_VA_LO 0x100000000ull
#define AGPU_GSP_VA_BYTES 0x20000000ull
#define AGPU_KVA_LO BIT_ULL(48)		/* kernel buffers: above every user address (va < 2^48) */
#define AGPU_KVA_BYTES BIT_ULL(40)
#define AGPU_RPDES_BYTES 184u		/* NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS */
#define AGPU_MEM_MAX_OBJ 256u		/* memory objects per file */
#define AGPU_TLB_POLLS 100000u		/* register reads before an invalidate counts as stuck */

/*
 * Kernel-owned zeroed system memory for RM (instance block, fault-method buffers, GR context
 * buffers): one dma-coherent block, so iova is contiguous. gpu_va is 0 until agpu_vas_map_kbuf.
 */
struct agpu_kbuf {
	void *cpu;
	dma_addr_t iova;
	size_t size;
	u64 gpu_va;
};

/*
 * All return 0 or a negative errno: -ENOMEM (host memory, DMA mapping or page-table pages),
 * -EINVAL (refused by an Omega check), -ENOENT (no such object or range), -EBUSY (address in
 * use), -ETIMEDOUT (TLB invalidate never completed; the device is then left FAILED), -EPROTO (an
 * Omega unit trapped), -ENODEV (the device is not DMA-coherent, see agpu_vas_create).
 */
int agpu_kbuf_alloc(struct agpu_dev *d, size_t size, struct agpu_kbuf *b);
void agpu_kbuf_free(struct agpu_dev *d, struct agpu_kbuf *b);

/*
 * The page-table tree of the file's address space: root (PD4) plus the levels down to the PD0
 * instance covering GSP's range, pinned until agpu_vas_destroy. A second create returns 0.
 */
int agpu_vas_create(struct agpu_file *f);
/*
 * Frees the tree, the kernel range and any user objects left. Call it only after GSP-RM freed
 * the FERMI_VASPACE_A object: GSP writes the pinned PD0 page until then. If that free failed,
 * call agpu_vas_abandon instead. agpu_mem_file_release calls it too (a no-op the second time).
 */
void agpu_vas_destroy(struct agpu_file *f);
/* GSP may still walk or write this tree: never give its pages (or user pages) back to Linux. */
void agpu_vas_abandon(struct agpu_file *f);
u64 agpu_vas_root_iova(struct agpu_file *f);	/* 0 before agpu_vas_create */

/*
 * Assigns b->gpu_va in [AGPU_KVA_LO, AGPU_KVA_LO + AGPU_KVA_BYTES), aligned to the buffer size
 * rounded up to a power of two (at most 2 MiB), maps it with 4 KiB PTEs and invalidates. After a
 * failed invalidate in agpu_vas_unmap_kbuf the device is FAILED: keep the buffer (leak it).
 */
int agpu_vas_map_kbuf(struct agpu_file *f, struct agpu_kbuf *b, bool gpu_uncached);
void agpu_vas_unmap_kbuf(struct agpu_file *f, struct agpu_kbuf *b);

/*
 * Fills NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS (len must be AGPU_RPDES_BYTES) as
 * _gvaspacePopulatePDEentries (gpu_vaspace.c:5221-5259) does: subDeviceId 0, hSubDevice 0,
 * pageSize 2 MiB, [4 GiB, 4.5 GiB - 1], 5 levels root first.
 */
int agpu_vas_reserved_pdes(struct agpu_file *f, u8 *params, size_t len);

/*
 * A channel uses part of object `handle` (ring, USERD, error notifier): delta +1 when it starts,
 * -1 when GSP-RM freed it. AGPU_MEM_FREE refuses a held object (AGPU_RM_E_STATE, code 16), so
 * GSP never writes into freed pages. -ENOENT for an unknown handle, -EINVAL on an underflow.
 */
int agpu_mem_hold(struct agpu_file *f, u32 handle, int delta);

/*
 * Device address of [off, off + len) of user memory object `handle`. Refuses an empty slice,
 * one outside the object, and one crossing a page boundary where the IOVAs are not contiguous.
 */
int agpu_mem_iova(struct agpu_file *f, u32 handle, u64 off, u64 len, dma_addr_t *iova,
		  u64 *gpu_va);

/*
 * Invalidate the GPU TLBs for this file's address space (BAR0 0xb830a0/a4/b0;
 * kgmmuInvalidateTlb_GM107 + kgmmuCommitTlbInvalidate_GB100), polling at most AGPU_TLB_POLLS
 * reads for a free invalidate unit before and for completion after.
 */
int agpu_tlb_invalidate(struct agpu_dev *d, struct agpu_file *f);

#endif
