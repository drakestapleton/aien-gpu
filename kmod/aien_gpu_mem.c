// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * aien_gpu_mem.c -- GPU memory and page tables, Linux glue only. Entry encodings, index math, the walk
 * plan, argument and overlap checks, mmap placement, slices, kernel-VA placement, the reserved-PDE
 * parameters and the TLB invalidate words are Omega units (om/gmmu.om, om/memchk.om, om/rpdes.om);
 * this file allocates and maps pages, stores what the units return and touches the registers.
 *
 * One address space per open file (Route B, docs/STAGE-C.md): an RM-managed FERMI_VASPACE_A whose
 * page tables (ver3, 5 levels, 4 KiB pages) live in dma-coherent system memory written here. User
 * memory is order-0 pages, each dma_map'ed (the IOMMU translates; the GPU sees IOVAs), mapped into the
 * caller at va (normal cacheable memory, as nvidia.ko maps sysmem) and into the GPU at gpu_va == va.
 * Status: IMPLEMENTED; host tests cover the units; hardware NOT_RUN.
 */
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include "aien_gpu.h"
#include "aien_gpu_mem.h"

#define AGPU_INVAL_PDB 0x00b830a0u	/* NV_VIRTUAL_FUNCTION_PRIV_MMU_INVALIDATE_PDB, 0xb80000 + 0x30a0 */
#define AGPU_INVAL_UPPER 0x00b830a4u	/* ..._MMU_INVALIDATE_UPPER_PDB */
#define AGPU_INVAL 0x00b830b0u		/* ..._MMU_INVALIDATE: gb100/dev_vm.h:327-402 */
#define AGPU_PT_BYTES 4096u		/* every page-table page; the root uses 16 bytes of its page */
#define AGPU_LEAF 7u			/* om/gmmu.om op 9: the entry at this level is the leaf */

/* om/gmmu.om ops */
enum { G_IDX = 1, G_PDE = 2, G_PTE = 3, G_TLB = 4, G_PENDING = 5, G_COUNT = 7, G_ESIZE = 8,
	G_NEXT = 9, G_CPUOK = 10 };
/* om/memchk.om ops */
enum { M_ALLOC = 1, M_OVERLAP = 2, M_MMAP = 3, M_HANDLE = 4, M_SLICE = 5, M_CONTIG = 6, M_OFF = 7,
	M_KVA = 8 };

static_assert(PAGE_SIZE == 4096, "GPU PTEs map 4 KiB pages; one CPU page each");

struct agpu_pt {
	u64 *cpu;
	dma_addr_t iova;
	struct agpu_pt **sub;	/* child tables by entry; NULL in a leaf table and in GSP's PD0 */
	u32 nsub;
};

struct agpu_mobj {
	u32 handle, flags, holds;
	u64 va, size, npages;
	struct page **pages;
	dma_addr_t *iova;
};

struct agpu_fmem {
	struct agpu_pt *root;
	u64 ranges[2 * AGPU_MEM_MAX_OBJ];	/* (va, size) of obj[i] at 2i, om/memchk.om op 2 */
	struct agpu_mobj *obj[AGPU_MEM_MAX_OBJ];
	u32 nobj, next_handle;
	u64 kva;			/* next free kernel-buffer VA */
	bool abandoned;			/* GSP may still use this tree: never free its pages */
};

static const u64 agpu_no_ranges[2 * AGPU_MEM_MAX_OBJ];

/* One unit call. A trap is -EPROTO (agpu_om); a refusal is -EINVAL with its code in *code. */
static int om_call(const char *name, const void *unit, u64 op, u64 a, u64 b, u64 c, u64 d, u64 e,
		   u64 *r, u32 *code)
{
	int rc = agpu_om(name, unit, op, a, b, c, d, e, r);

	if (rc)
		return rc;
	if (*r >= AGPU_OM_FAIL) {
		if (code)
			*code = (u32)(*r - AGPU_OM_FAIL);
		return -EINVAL;
	}
	return 0;
}

static int gm(u64 op, u64 a, u64 b, u64 *r)
{
	return om_call("gmmu", agpu_om_gmmu, op, a, b, 0, 0, 0, r, NULL);
}

static int mc(u64 op, const u64 *tab, u64 a, u64 b, u64 c, u64 d, u64 *r, u32 *code)
{
	return om_call("memchk", agpu_om_memchk, op, (u64)tab, a, b, c, d, r, code);
}

static void st_set(struct agpu_rm_status *st, u32 verdict, u32 code)
{
	st->verdict = verdict;
	st->code = code;
	st->fn = 0;
	st->step = 0;
}

/* An internal failure as a verdict: host memory, a stuck invalidate, else the errno as the code. */
static void st_errno(struct agpu_rm_status *st, int rc)
{
	if (rc == -ENOMEM)
		st_set(st, AGPU_RM_E_NOMEM, 0);
	else if (rc == -ETIMEDOUT)
		st_set(st, AGPU_RM_E_TIMEOUT, 0);
	else
		st_set(st, AGPU_RM_E_STATE, (u32)-rc);
}

/* ---- page-table pages ---- */

static struct agpu_pt *pt_new(struct agpu_dev *d, u64 lv)
{
	struct agpu_pt *t = kzalloc(sizeof(*t), GFP_KERNEL);
	u64 nx = AGPU_LEAF, n = 0;

	if (!t)
		return NULL;
	if (gm(G_NEXT, lv, 12, &nx) || (nx != AGPU_LEAF && gm(G_COUNT, lv, 0, &n)))
		goto fail;
	t->cpu = dma_alloc_coherent(&d->pdev->dev, AGPU_PT_BYTES, &t->iova, GFP_KERNEL);
	if (!t->cpu)
		goto fail;
	if (nx != AGPU_LEAF) {
		t->sub = kcalloc(n, sizeof(*t->sub), GFP_KERNEL);
		if (!t->sub) {
			dma_free_coherent(&d->pdev->dev, AGPU_PT_BYTES, t->cpu, t->iova);
			goto fail;
		}
		t->nsub = n;
	}
	return t;
fail:
	kfree(t);
	return NULL;
}

/* leak: the GPU may still read the tree; give back only the bookkeeping. */
static void pt_free(struct agpu_dev *d, struct agpu_pt *t, bool leak)
{
	u32 i;

	if (!t)
		return;
	for (i = 0; i < t->nsub; i++)
		pt_free(d, t->sub[i], leak);
	if (!leak)
		dma_free_coherent(&d->pdev->dev, AGPU_PT_BYTES, t->cpu, t->iova);
	kfree(t->sub);
	kfree(t);
}

/*
 * Walk to the table holding va's entry at level `target` (or at the leaf of a 4 KiB mapping, if that
 * comes first), creating missing tables when `create`. Levels strictly increase, so the loop runs at
 * most 7 times. Every new entry is written only after om/gmmu.om op 10 allows the CPU to write it.
 */
static int pt_walk(struct agpu_dev *d, struct agpu_fmem *m, u64 va, u64 target, bool create,
		   struct agpu_pt **tp, u64 *ip)
{
	struct agpu_pt *t = m->root, *c;
	u64 lv = 0, idx, nx, esz, pde, ok;
	int rc, k;

	for (k = 0; k < 7; k++) {
		rc = gm(G_IDX, lv, va, &idx) ?: gm(G_NEXT, lv, 12, &nx);
		if (rc)
			return rc;
		if (lv == target || nx == AGPU_LEAF) {
			*tp = t;
			*ip = idx;
			return 0;
		}
		if (!t->sub || idx >= t->nsub)
			return -EPROTO;		/* GSP's PD0, which this module never walks into */
		if (!t->sub[idx]) {
			if (!create)
				return -ENOENT;
			rc = gm(G_CPUOK, lv, va, &ok) ?: gm(G_ESIZE, lv, 0, &esz);
			if (rc)
				return rc;
			c = pt_new(d, nx);
			if (!c)
				return -ENOMEM;
			rc = gm(G_PDE, c->iova, 2, &pde);	/* 2: SYSTEM_COHERENT_MEMORY */
			if (rc) {
				pt_free(d, c, false);
				return rc;
			}
			if (esz == 16) {		/* dual PDE: big half invalid, small half */
				WRITE_ONCE(t->cpu[2 * idx + 1], pde);
				WRITE_ONCE(t->cpu[2 * idx], 0);
			} else {
				WRITE_ONCE(t->cpu[idx], pde);
			}
			t->sub[idx] = c;
		}
		t = t->sub[idx];
		lv = nx;
	}
	return -EPROTO;
}

/* Write 4 KiB PTEs for n pages from va: device addresses iova[k], or base + 4096 k when iova is NULL. */
static int map_pages(struct agpu_dev *d, struct agpu_fmem *m, u64 va, const dma_addr_t *iova,
		     dma_addr_t base, u64 n, u64 fl, u64 *done)
{
	struct agpu_pt *t = NULL;
	u64 k, v, idx, pte, key = ~0ull;
	int rc;

	for (k = 0; k < n; k++) {
		v = va + (k << 12);
		if (v >> 21 != key) {		/* one leaf table covers 2 MiB */
			rc = pt_walk(d, m, v, 6, true, &t, &idx);
			if (rc)
				return rc;
			key = v >> 21;
		}
		rc = gm(G_IDX, 6, v, &idx) ?:
		     gm(G_PTE, iova ? iova[k] : base + (k << 12), fl, &pte);
		if (rc)
			return rc;
		if (READ_ONCE(t->cpu[idx]))
			return -EBUSY;		/* never, after om/memchk.om op 2 */
		WRITE_ONCE(t->cpu[idx], pte);
		*done = k + 1;
		if (!(k & 511))
			cond_resched();
	}
	return 0;
}

static void unmap_pages(struct agpu_dev *d, struct agpu_fmem *m, u64 va, u64 n)
{
	struct agpu_pt *t = NULL;
	u64 k, v, idx, key = ~0ull;

	for (k = 0; k < n; k++) {
		v = va + (k << 12);
		if (v >> 21 != key) {
			key = v >> 21;
			if (pt_walk(d, m, v, 6, false, &t, &idx))
				t = NULL;
		}
		if (t && !gm(G_IDX, 6, v, &idx))
			WRITE_ONCE(t->cpu[idx], 0);
		if (!(k & 511))
			cond_resched();
	}
}

/* ---- TLB ---- */

static int tlb_wait(struct agpu_dev *d)
{
	u64 pending = 1;
	u32 i;
	int rc;

	for (i = 0; i < AGPU_TLB_POLLS; i++) {
		rc = gm(G_PENDING, ioread32(d->bar0 + AGPU_INVAL), 0, &pending);
		if (rc)
			return rc;
		if (!pending)
			return 0;
		udelay(1);
	}
	dev_err(&d->pdev->dev, "TLB invalidate still pending after %u reads: device FAILED\n",
		AGPU_TLB_POLLS);
	d->info.state = AGPU_STATE_FAILED;
	return -ETIMEDOUT;
}

int agpu_tlb_invalidate(struct agpu_dev *d, struct agpu_file *f)
{
	struct agpu_fmem *m = f->mem;
	u64 w[3];
	int rc, i;

	if (!m || !m->root || !d->bar0)
		return -ENOENT;
	for (i = 0; i < 3; i++) {
		rc = gm(G_TLB, m->root->iova, i, &w[i]);
		if (rc)
			return rc;
	}
	dma_wmb();			/* the entries reach memory before the GPU walks again */
	rc = tlb_wait(d);		/* kgmmuInvalidateTlb_GM107: no invalidate already in flight */
	if (rc)
		return rc;
	iowrite32((u32)w[0], d->bar0 + AGPU_INVAL_PDB);
	iowrite32((u32)w[1], d->bar0 + AGPU_INVAL_UPPER);
	iowrite32((u32)w[2], d->bar0 + AGPU_INVAL);
	return tlb_wait(d);
}

/* ---- kernel buffers ---- */

int agpu_kbuf_alloc(struct agpu_dev *d, size_t size, struct agpu_kbuf *b)
{
	memset(b, 0, sizeof(*b));
	if (!size)
		return -EINVAL;
	b->size = PAGE_ALIGN(size);
	b->cpu = dma_alloc_coherent(&d->pdev->dev, b->size, &b->iova, GFP_KERNEL);
	if (!b->cpu) {
		b->size = 0;
		return -ENOMEM;
	}
	return 0;
}

void agpu_kbuf_free(struct agpu_dev *d, struct agpu_kbuf *b)
{
	if (b->cpu)
		dma_free_coherent(&d->pdev->dev, b->size, b->cpu, b->iova);
	memset(b, 0, sizeof(*b));
}

int agpu_vas_map_kbuf(struct agpu_file *f, struct agpu_kbuf *b, bool gpu_uncached)
{
	struct agpu_dev *d = f->d;
	struct agpu_fmem *m = f->mem;
	u64 start, done = 0, n;
	int rc;

	if (!m || !b->cpu || b->gpu_va)
		return -EINVAL;
	rc = mc(M_KVA, agpu_no_ranges, m->kva, b->size, 0, 0, &start, NULL);
	if (rc)
		return rc;
	n = b->size >> 12;
	rc = map_pages(d, m, start, NULL, b->iova, n, gpu_uncached ? 1 : 0, &done);
	if (rc) {
		unmap_pages(d, m, start, done);
		agpu_tlb_invalidate(d, f);
		return rc;
	}
	rc = agpu_tlb_invalidate(d, f);
	if (rc)
		return rc;
	b->gpu_va = start;
	m->kva = start + b->size;
	return 0;
}

void agpu_vas_unmap_kbuf(struct agpu_file *f, struct agpu_kbuf *b)
{
	if (!f->mem || !b->gpu_va)
		return;
	unmap_pages(f->d, f->mem, b->gpu_va, b->size >> 12);
	agpu_tlb_invalidate(f->d, f);	/* on a timeout the device is FAILED */
	b->gpu_va = 0;
}

/* ---- the address space ---- */

int agpu_vas_create(struct agpu_file *f)
{
	struct agpu_dev *d = f->d;
	struct agpu_fmem *m;
	struct agpu_pt *pd0;
	u64 idx;
	int rc;

	if (f->mem)
		return 0;
	/* PDEs and the reserved-PDE levels say SYSTEM_COHERENT: true only for a coherent device. */
	if (!dev_is_dma_coherent(&d->pdev->dev))
		return -ENODEV;
	m = kvzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->root = pt_new(d, 0);
	if (!m->root) {
		kvfree(m);
		return -ENOMEM;
	}
	/* Pin PD4..PD0 for GSP's range (gpu_vaspace.c:340-370); GSP-RM owns that PD0's entries. */
	rc = pt_walk(d, m, AGPU_GSP_VA_LO, 4, true, &pd0, &idx);
	if (rc) {
		pt_free(d, m->root, false);
		kvfree(m);
		return rc;
	}
	kfree(pd0->sub);
	pd0->sub = NULL;
	pd0->nsub = 0;
	m->next_handle = 1;
	m->kva = AGPU_KVA_LO;
	f->mem = m;
	return 0;
}
	for (k = 0; !leak && o->pages && o->iova && k < o->npages && o->pages[k]; k++) {
u64 agpu_vas_root_iova(struct agpu_file *f)
{
	return f->mem ? f->mem->root->iova : 0;
}

int agpu_vas_reserved_pdes(struct agpu_file *f, u8 *params, size_t len)
{
	struct agpu_pt *t;
	u64 lv[8] = { 0 }, idx, r;
	int i, rc;

	if (!f->mem || len != AGPU_RPDES_BYTES)
		return -EINVAL;
	t = f->mem->root;
	for (i = 0; i < 5; i++) {
		lv[i] = t->iova;
		if (i == 4)
			break;
		rc = gm(G_IDX, i, AGPU_GSP_VA_LO, &idx);
		if (rc)
			return rc;
		if (!t->sub || idx >= t->nsub || !t->sub[idx])
			return -EPROTO;
		t = t->sub[idx];
	}
	lv[5] = 1;			/* dma-coherent: agpu_vas_create checked it */
	return om_call("rpdes", agpu_om_rpdes, (u64)params, (u64)lv, 0, 0, 0, 0, &r, NULL);
}

/* ---- memory objects ---- */

static int obj_slot(struct agpu_fmem *m, u32 handle)
{
	u32 i;

	for (i = 0; m && i < m->nobj; i++)
		if (m->obj[i]->handle == handle)
			return i;
	return -1;
}

static void obj_free(struct agpu_dev *d, struct agpu_mobj *o, bool leak)
{
	u64 k;

	for (k = 0; !leak && k < o->npages && o->pages[k]; k++) {
		if (o->iova[k])
			dma_unmap_page(&d->pdev->dev, o->iova[k], PAGE_SIZE, DMA_BIDIRECTIONAL);
		put_page(o->pages[k]);	/* a user mapping holds its own reference */
	}
	kvfree(o->iova);
	kvfree(o->pages);
	kfree(o);
}

static void obj_remove(struct agpu_fmem *m, int i)
{
	u32 last = m->nobj - 1;

	m->obj[i] = m->obj[last];
	m->ranges[2 * i] = m->ranges[2 * last];
	m->ranges[2 * i + 1] = m->ranges[2 * last + 1];
	m->obj[last] = NULL;
	m->ranges[2 * last] = 0;
	m->ranges[2 * last + 1] = 0;
	m->nobj = last;
}

static int obj_pages(struct agpu_dev *d, struct agpu_mobj *o)
{
	u64 k;

	o->pages = kvcalloc(o->npages, sizeof(*o->pages), GFP_KERNEL_ACCOUNT);
	o->iova = kvcalloc(o->npages, sizeof(*o->iova), GFP_KERNEL_ACCOUNT);
	if (!o->pages || !o->iova)
		return -ENOMEM;
	for (k = 0; k < o->npages; k++) {
		if (fatal_signal_pending(current))
			return -EINTR;
		o->pages[k] = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NOWARN);
		if (!o->pages[k])
			return -ENOMEM;
		o->iova[k] = dma_map_page(&d->pdev->dev, o->pages[k], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(&d->pdev->dev, o->iova[k])) {
			o->iova[k] = 0;
			return -ENOMEM;
		}
		if (!(k & 511))
			cond_resched();
	}
	return 0;
}

void agpu_mem_alloc(struct agpu_file *f, struct agpu_mem_alloc *a)
{
	struct agpu_dev *d = f->d;
	struct agpu_fmem *m;
	struct agpu_mobj *o;
	u64 off, r, done = 0;
	u32 code = 0;
	int rc;

	a->handle = 0;
	a->mmap_off = 0;
	st_set(&a->st, 0, 0);
	if (d->info.state != AGPU_STATE_RM_READY) {
		st_set(&a->st, AGPU_RM_E_STATE, d->info.state);
		return;
	}
	rc = mc(M_ALLOC, agpu_no_ranges, a->size, a->va, a->flags, 0, &r, &code);
	if (rc) {
		st_set(&a->st, AGPU_RM_E_ARG, rc == -EINVAL ? code : (u32)-rc);
		return;
	}
	if (agpu_rm_fctx_get(f, &a->st))
		return;
	m = f->mem;
	if (!m) {
		st_errno(&a->st, -ENOENT);
		return;
	}
	rc = mc(M_OVERLAP, m->ranges, m->nobj, a->va, a->size, 0, &r, &code) ?:
	     mc(M_OFF, m->ranges, m->next_handle, 0, 0, 0, &off, &code);
	if (rc) {
		st_set(&a->st, AGPU_RM_E_ARG, rc == -EINVAL ? code : (u32)-rc);
		return;
	}
	o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (!o) {
		st_errno(&a->st, -ENOMEM);
		return;
	}
	o->handle = m->next_handle;
	o->flags = a->flags;
	o->va = a->va;
	o->size = a->size;
	o->npages = a->size >> 12;
	rc = obj_pages(d, o);
	if (!rc)
		rc = map_pages(d, m, o->va, o->iova, 0, o->npages, o->flags, &done);
	if (rc) {
		unmap_pages(d, m, o->va, done);
		/* a stuck invalidate leaves the device FAILED; the pages then stay with the GPU */
		obj_free(d, o, done && agpu_tlb_invalidate(d, f));
		st_errno(&a->st, rc);
		return;
	}
	rc = agpu_tlb_invalidate(d, f);
	if (rc) {
		obj_free(d, o, true);
		st_errno(&a->st, rc);
		return;
	}
	m->obj[m->nobj] = o;
	m->ranges[2 * m->nobj] = o->va;
	m->ranges[2 * m->nobj + 1] = o->size;
	m->nobj++;
	m->next_handle++;
	a->handle = o->handle;
	a->mmap_off = off;
	st_set(&a->st, AGPU_RM_OK, 0);
}

void agpu_mem_free(struct agpu_file *f, struct agpu_mem_free *a)
{
	struct agpu_dev *d = f->d;
	struct agpu_fmem *m = f->mem;
	struct agpu_mobj *o;
	int i, rc;

	if (d->info.state != AGPU_STATE_RM_READY) {
		st_set(&a->st, AGPU_RM_E_STATE, d->info.state);
		return;
	}
	i = obj_slot(m, a->handle);
	if (i < 0) {
		st_set(&a->st, AGPU_RM_E_ARG, 15);
		return;
	}
	o = m->obj[i];
	if (o->holds) {
		st_set(&a->st, AGPU_RM_E_STATE, 16);
		return;
	}
	unmap_pages(d, m, o->va, o->npages);
	rc = agpu_tlb_invalidate(d, f);
	obj_remove(m, i);
	obj_free(d, o, rc != 0);
	if (rc)
		st_errno(&a->st, rc);
	else
		st_set(&a->st, AGPU_RM_OK, 0);
}

int agpu_mem_hold(struct agpu_file *f, u32 handle, int delta)
{
	int i = obj_slot(f->mem, handle);
	struct agpu_mobj *o;

	if (i < 0)
		return -ENOENT;
	o = f->mem->obj[i];
	if (delta < 0 && o->holds < (u32)-delta)
		return -EINVAL;
	o->holds += delta;
	return 0;
}

int agpu_mem_iova(struct agpu_file *f, u32 handle, u64 off, u64 len, dma_addr_t *iova, u64 *gpu_va)
{
	int i = obj_slot(f->mem, handle), rc;
	struct agpu_mobj *o;
	u64 p, r;

	if (i < 0)
		return -ENOENT;
	o = f->mem->obj[i];
	rc = mc(M_SLICE, agpu_no_ranges, off, len, o->size, 0, &r, NULL);
	if (rc)
		return rc;
	for (p = off >> 12; p < (off + len - 1) >> 12; p++) {
		rc = mc(M_CONTIG, agpu_no_ranges, o->iova[p], o->iova[p + 1], 0, 0, &r, NULL);
		if (rc)
			return rc;
	}
	*iova = o->iova[off >> 12] + (off & 4095);
	*gpu_va = o->va + off;
	return 0;
}

int agpu_mem_mmap(struct agpu_file *f, struct vm_area_struct *vma)
{
	struct agpu_fmem *m = f->mem;
	struct agpu_mobj *o;
	unsigned long n;
	u64 h, r;
	int i, rc;

	if (!m)
		return -EINVAL;
	rc = mc(M_HANDLE, m->ranges, (u64)vma->vm_pgoff << PAGE_SHIFT, 0, 0, 0, &h, NULL);
	if (rc)
		return rc;
	i = obj_slot(m, (u32)h);
	if (i < 0)
		return -EINVAL;
	o = m->obj[i];
	rc = mc(M_MMAP, m->ranges, vma->vm_start, vma->vm_end - vma->vm_start, o->va, o->size, &r,
		NULL);
	if (rc)
		return rc;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	n = o->npages;
	return vm_insert_pages(vma, vma->vm_start, o->pages, &n);
}

/* Tear the address space down. The GPU must no longer use it (GSP-RM freed the vaspace); stale TLB
 * entries tagged with this root are invalidated before its pages can be reused. */
void agpu_vas_destroy(struct agpu_file *f)
{
	struct agpu_dev *d = f->d;
	struct agpu_fmem *m = f->mem;
	bool leak;
	u32 i;

	if (!m)
		return;
	leak = m->abandoned || d->info.state != AGPU_STATE_RM_READY || agpu_tlb_invalidate(d, f);
	if (leak)
		dev_warn(&d->pdev->dev, "GPU address space kept until reboot (%u objects)\n", m->nobj);
	for (i = 0; i < m->nobj; i++)
		obj_free(d, m->obj[i], leak);
	pt_free(d, m->root, leak);
	kvfree(m);
	f->mem = NULL;
}

void agpu_vas_abandon(struct agpu_file *f)
{
	if (f->mem)
		f->mem->abandoned = true;
}

void agpu_mem_file_release(struct agpu_file *f)
{
	agpu_vas_destroy(f);	/* no-op when RM already destroyed it */
}
