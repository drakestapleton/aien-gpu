// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * aien-gpu: AIEN's own GB10 GPU driver (ledger L332). Linux glue only; every decision that is
 * not Linux plumbing is an Omega unit (the om directory, embedded by kmod/agpu_om.S, checked by
 * tools/kcheck.om).
 *
 * Binds the GB10 (10de:2e12) only with allow_bind=1, and only after om/ident.om accepts the
 * PCI function (stage 0, before any MMIO) and its identity registers (stage 1). Nothing is
 * written to the GPU until AGPU_BOOT. NVIDIA's kernel modules must be unloaded first: the PCI
 * core never binds two drivers to one device, and this driver never unbinds another.
 * Status: IMPLEMENTED, host and QEMU tested; hardware NOT_RUN (docs/STATUS.md).
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/firmware.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/hex.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <crypto/sha2.h>

#include "aien_gpu.h"

static bool allow_bind;
module_param(allow_bind, bool, 0444);
MODULE_PARM_DESC(allow_bind, "bind the GB10; only after NVIDIA's modules are unloaded (default 0)");
static uint arena_mib = 192;
module_param(arena_mib, uint, 0444);
MODULE_PARM_DESC(arena_mib, "boot arena in MiB: GSP-RM image, radix3, boot blocks, queues (default 192)");
static uint heap_mib;
module_param(heap_mib, uint, 0444);
MODULE_PARM_DESC(heap_mib, "GSP system-memory heap in MiB; 0 = none, AGPU_BOOT then refuses (window: 512)");
static bool selftest_trap;
module_param(selftest_trap, bool, 0444);
MODULE_PARM_DESC(selftest_trap, "self-test also drives one Omega unit into a trap and checks it is contained");

static struct agpu_dev g;		/* one GB10 per machine */
/* /dev/aien-gpu's mapping, to revoke arena mappings at boot */
static struct address_space *g_mapping;

int agpu_om(const char *unit, const void *entry, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
	    u64 *result)
{
	const u64 a[6] = { a0, a1, a2, a3, a4, a5 };
	u64 status = 0;

	*result = agpu_om_enter(entry, a, &status);
	if (status) {
		pr_err(AGPU_NAME ": omega unit %s trapped, code %llu\n", unit, status);
		return -EPROTO;
	}
	return 0;
}

/* ---- self-test at load: each unit on known answers, from the same vectors as tests/host ---- */

static int st_check(const char *what, bool ok)
{
	if (!ok)
		pr_err(AGPU_NAME ": omega self-test FAIL %s\n", what);
	return ok ? 0 : -EIO;
}

static void st_put32(u8 *p, u32 v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static int agpu_selftest(void)
{
	u64 r, st[32] = { 0 }, act[4] = { 0 };
	u8 blk[128] = { 0 };
	u8 *msg, *q;
	u64 *ops;
	int rc = 0, i;

	rc |= agpu_om("ident", agpu_om_ident, 0x10de, 0x2e12, 64ull << 20, 0x1bb000a1,
		      0x1bba1000, 1, &r) ?: st_check("ident GB20B", r == 0);
	rc |= agpu_om("ident", agpu_om_ident, 0x10de, 0x2e12, 64ull << 20, 0x1b0000a1,
		      (0x1aull << 24) | (0xbull << 20), 1, &r) ?:
	      st_check("ident GB10B refused", r == 6);
	rc |= agpu_om("ident", agpu_om_ident, 0x1b36, 0x0005, 4096, 0, 0, 0, &r) ?:
	      st_check("ident other vendor", r == 1);
	/* a valid AGPU_BOOT block (tests/host/unit_test.c good_boot) */
	st_put32(blk, 1);
	st_put32(blk + 8, 0x1000);
	st_put32(blk + 16, 868);
	st_put32(blk + 24, 0x2000);
	st_put32(blk + 28, 0x8);		/* FMC params at 0x8_0000_2000 */
	st_put32(blk + 32, 0x100000);
	st_put32(blk + 40, 0x140000);
	for (i = 0; i < 4; i++)
		st_put32(blk + 48 + 4 * i, 1000);
	st_put32(blk + 64, 10);
	st_put32(blk + 68, 30000);
	rc |= agpu_om("abi", agpu_om_abi, 1, (u64)blk, 0x800000000ull, 64ull << 20, 0, 0, &r) ?:
	      st_check("abi accept", r == 0);
	blk[0] = 2;
	rc |= agpu_om("abi", agpu_om_abi, 1, (u64)blk, 0x800000000ull, 64ull << 20, 0, 0, &r) ?:
	      st_check("abi version", r == 2);
	if (rc)
		return -EIO;

	msg = vzalloc(AGPU_MSG_BYTES);
	q = vzalloc(AGPU_MQ_BYTES);
	ops = vzalloc(6 * AGPU_SEQ_MAX_OPS * sizeof(u64));
	if (!msg || !q || !ops) {
		rc = -ENOMEM;
		goto out;
	}
	/* boot: an empty packet stops at stage 1 (arg) with no register action */
	rc |= agpu_om("boot", agpu_om_boot, (u64)st, (u64)msg, 0, (u64)act, 0, 0, &r) ?:
	      st_check("boot arg", r == 1 && st[12] == 1);
	/* seal: one element; the folded XOR over the rounded length is then 0 */
	rc |= agpu_om("seal", agpu_om_seal, (u64)msg, 10, 16, 16, 0, 0, &r) ?:
	      st_check("seal", r == 1);
	if (!rc) {
		u64 x = 0;
		__le64 w;

		for (i = 0; i < 96; i += 8) {
			memcpy(&w, msg + i, 8);
			x ^= le64_to_cpu(w);
		}
		rc |= st_check("seal checksum", ((u32)(x >> 32) ^ (u32)x) == 0);
	}
	/*
	 * mqput into an empty queue, then mqget on a queue whose header is GSP's shape and
	 * empty
	 */
	rc |= agpu_om("mqput", agpu_om_mqput, (u64)q, (u64)msg, 1, 0, 0, 0, &r) ?:
	      st_check("mqput", r == 1);
	memset(q, 0, 32);
	st_put32(q + 4, AGPU_MQ_BYTES);
	st_put32(q + 8, 4096);
	st_put32(q + 12, 63);
	st_put32(q + 24, 32);
	st_put32(q + 28, 4096);
	rc |= agpu_om("mqget", agpu_om_mqget, (u64)q, (u64)msg, 5, 5, 0, 0, &r) ?:
	      st_check("mqget empty", r == 0);
	/* seq: one REG_WRITE of GSP QUEUE_HEAD(0) */
	memset(msg, 0, 256);
	st_put32(msg + 56, 32 + 40 + 16);
	st_put32(msg + 80, 4);
	st_put32(msg + 84, 3);
	st_put32(msg + 120, 0);
	st_put32(msg + 124, AGPU_GSP_QHEAD0);
	st_put32(msg + 128, 7);
	rc |= agpu_om("seq", agpu_om_seq, (u64)msg, (u64)ops, 64ull << 20, 0, 0, 0, &r) ?:
	      st_check("seq", r == 1 && ops[0] == 0 && ops[1] == AGPU_GSP_QHEAD0 && ops[2] == 7);
	if (!rc && selftest_trap) {
		u64 status = 0;
		const u64 a[6] = { (u64)st, (u64)msg, 0, (u64)act, 0, 0 };

		/*
		 * A state the driver never builds: packet index past the 1024-byte view,
		 * trap 3 (bounds).
		 */
		memset(st, 0, sizeof(st));
		st[0] = 23; st[1] = 2000; st[9] = 1024;
		r = agpu_om_enter(agpu_om_boot, a, &status);
		pr_info(AGPU_NAME ": omega trap contained code=%llu\n", status);
		rc |= st_check("trap", status == 3 && r == 0);
		rc |= agpu_om("ident", agpu_om_ident, 0x10de, 0x2e12, 64ull << 20, 0, 0, 0, &r) ?:
		      st_check("call after trap", r == 0);
	}
out:
	vfree(ops);
	vfree(q);
	vfree(msg);
	if (!rc)
		pr_info(AGPU_NAME ": omega self-test PASS units=7\n");
	return rc ? -EIO : 0;
}

/* ---- the character device ---- */

static long agpu_info_ioctl(struct agpu_info __user *u)
{
	struct agpu_info i;

	mutex_lock(&g.lock);
	i = g.info;
	mutex_unlock(&g.lock);
	return copy_to_user(u, &i, sizeof(i)) ? -EFAULT : 0;
}

/*
 * Copy part of the boot arena out (GSP's log buffers after a start-up attempt). The range is
 * checked under the lock; the copy runs after it (the arena lives until remove).
 */
static long agpu_arena_read_ioctl(struct agpu_arena_read __user *u)
{
	u8 blk[128] = { 0 };
	struct agpu_arena_read a;
	u64 verdict;
	u8 *src;
	long rc;

	if (copy_from_user(&a, u, sizeof(a)))
		return -EFAULT;
	memcpy(blk, &a, sizeof(a));
	mutex_lock(&g.lock);
	rc = agpu_om("abi", agpu_om_abi, 2, (u64)blk, g.arena_iova, g.arena ? g.arena_bytes : 0,
		     0, 0, &verdict);
	if (!rc && verdict)
		rc = -EINVAL;
	src = (u8 *)g.arena;
	mutex_unlock(&g.lock);
	if (!rc && copy_to_user(u64_to_user_ptr(a.buf), src + a.off, a.len))
		rc = -EFAULT;
	return rc;
}

/* Every block is copied in before d->lock and out after it (see aien_gpu.h). */
static long agpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct agpu_file *f = file->private_data;
	void __user *u = (void __user *)arg;
	size_t n = _IOC_SIZE(cmd);
	void *k;
	long rc = 0;

	switch (cmd) {
	case AGPU_INFO:
		return agpu_info_ioctl(u);
	case AGPU_ARENA_READ:
		return capable(CAP_SYS_ADMIN) ? agpu_arena_read_ioctl(u) : -EPERM;
	case AGPU_BOOT:
	case AGPU_RM_INIT:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		break;
	case AGPU_MEM_ALLOC:
	case AGPU_MEM_FREE:
	case AGPU_CHAN_CREATE:
	case AGPU_CHAN_DESTROY:
		break;
	default:
		return -ENOTTY;
	}
	k = kvzalloc(n, GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	if (copy_from_user(k, u, n)) {
		kvfree(k);
		return -EFAULT;
	}
	/* the arena is GSP's from now on: revoke user mappings */
	if (cmd == AGPU_BOOT && g_mapping)
		unmap_mapping_range(g_mapping, AGPU_MMAP_ARENA, g.arena_bytes, 1);
	mutex_lock(&g.lock);
	switch (cmd) {
	case AGPU_BOOT:
		agpu_boot(&g, k);
		break;
	case AGPU_RM_INIT:
		agpu_rm_init(&g, k);
		break;
	case AGPU_MEM_ALLOC:
		agpu_mem_alloc(f, k);
		break;
	case AGPU_MEM_FREE:
		agpu_mem_free(f, k);
		break;
	case AGPU_CHAN_CREATE:
		agpu_chan_create(f, k);
		break;
	case AGPU_CHAN_DESTROY:
		agpu_chan_destroy(f, k);
		break;
	}
	mutex_unlock(&g.lock);
	if (copy_to_user(u, k, n))
		rc = -EFAULT;
	kvfree(k);
	return rc;
}

static int agpu_open(struct inode *inode, struct file *file)
{
	struct agpu_file *f;

	if (!g.pdev)
		return -ENODEV;
	f = kzalloc_obj(*f, GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->d = &g;
	file->private_data = f;
	g_mapping = file->f_mapping;
	return 0;
}

static int agpu_release(struct inode *inode, struct file *file)
{
	struct agpu_file *f = file->private_data;

	mutex_lock(&g.lock);
	/* channels and RM objects first: GSP stops using the memory */
	agpu_rm_file_release(f);
	agpu_mem_file_release(f);
	mutex_unlock(&g.lock);
	kfree(f);
	return 0;
}

/*
 * Offset 0: the boot arena, root, only before the start-up. 1 << 40: the doorbell page.
 * 2 << 40 and up: memory objects.
 */
static int agpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct agpu_file *f = file->private_data;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	size_t len = vma->vm_end - vma->vm_start;
	int rc;

	mutex_lock(&g.lock);
	if (off >= AGPU_MMAP_MEM_BASE)
		rc = agpu_mem_mmap(f, vma);
	else if (off == AGPU_MMAP_DOORBELL)
		rc = agpu_doorbell_mmap(f, vma);
	else if (off != AGPU_MMAP_ARENA || !capable(CAP_SYS_ADMIN))
		rc = -EPERM;
	else if (g.info.state != AGPU_STATE_BOUND || len > g.arena_bytes || !g.arena)
		rc = -EINVAL;
	else
		rc = dma_mmap_coherent(&g.pdev->dev, vma, g.arena, g.arena_iova, len);
	mutex_unlock(&g.lock);
	return rc;
}

static const struct file_operations agpu_fops = {
	.owner = THIS_MODULE,
	.open = agpu_open,
	.release = agpu_release,
	.unlocked_ioctl = agpu_ioctl,
	.mmap = agpu_mmap,
};

static struct miscdevice agpu_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AGPU_NAME,
	.fops = &agpu_fops,
	.mode = 0600,
};

/* ---- PCI ---- */

static void agpu_fw_check(struct pci_dev *pdev)
{
	const struct firmware *fw;
	u8 sum[SHA256_DIGEST_SIZE];
	char hex[2 * SHA256_DIGEST_SIZE + 1];

	if (request_firmware(&fw, AGPU_FW, &pdev->dev)) {
		dev_warn(&pdev->dev, "firmware %s missing: AGPU_BOOT will refuse\n", AGPU_FW);
		return;
	}
	g.info.fw_bytes = fw->size;
	sha256(fw->data, fw->size, sum);
	bin2hex(hex, sum, sizeof(sum));
	hex[sizeof(hex) - 1] = 0;
	g.info.fw_ok = fw->size == AGPU_FW_BYTES && !strcmp(hex, AGPU_FW_SHA256);
	release_firmware(fw);
	dev_info(&pdev->dev, "firmware %s %llu bytes sha256 %s %s\n", AGPU_FW, g.info.fw_bytes, hex,
		 g.info.fw_ok ? "matches the pin" : "DOES NOT match the pin");
}

static void agpu_free_mem(void)
{
	if (g.touched && (g.arena || g.heap)) {
		/*
		 * GSP may still read and write these: never hand them back to Linux under a
		 * running DMA master. Kept (leaked) until the reboot the window script does
		 * anyway.
		 */
		dev_warn(&g.pdev->dev, "GSP was started: arena and heap kept until reboot\n");
		g.arena = NULL;
		g.heap = NULL;
	}
	if (g.heap)
		dma_free_coherent(&g.pdev->dev, g.heap_bytes, g.heap, g.heap_iova);
	if (g.arena)
		dma_free_coherent(&g.pdev->dev, g.arena_bytes, g.arena, g.arena_iova);
	g.heap = NULL;
	g.arena = NULL;
	vfree(g.seq_ops);
	vfree(g.msg_rx);
	vfree(g.msg_tx);
	g.seq_ops = NULL;
	g.msg_rx = NULL;
	g.msg_tx = NULL;
}

static int agpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	u64 verdict;
	int rc;

	if (!allow_bind) {
		dev_info(&pdev->dev, "refusing: allow_bind=0\n");
		return -ENODEV;
	}
	if (g.pdev) {
		dev_info(&pdev->dev, "refusing: already bound to %s\n", pci_name(g.pdev));
		return -EBUSY;
	}
	rc = agpu_om("ident", agpu_om_ident, pdev->vendor, pdev->device, pci_resource_len(pdev, 0),
		     0, 0, 0, &verdict);
	if (rc)
		return rc;
	if (verdict) {
		dev_info(&pdev->dev, "refusing %04x:%04x: not a GB10 (ident %llu)\n", pdev->vendor,
			 pdev->device, verdict);
		return -ENODEV;
	}
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(AGPU_DMA_BITS));
	if (rc)
		return rc;
	g.bar0 = pcim_iomap(pdev, 0, 0);
	if (!g.bar0)
		return -ENOMEM;
	g.bar0_bytes = pci_resource_len(pdev, 0);
	g.info.boot0 = ioread32(g.bar0 + AGPU_PMC_BOOT_0);
	g.info.boot42 = ioread32(g.bar0 + AGPU_PMC_BOOT_42);
	rc = agpu_om("ident", agpu_om_ident, pdev->vendor, pdev->device, g.bar0_bytes, g.info.boot0,
		     g.info.boot42, 1, &verdict);
	if (rc)
		return rc;
	if (verdict) {
		dev_info(&pdev->dev, "refusing: identity registers BOOT_0 %08x BOOT_42 %08x (ident %llu)\n",
			 g.info.boot0, g.info.boot42, verdict);
		return -ENODEV;
	}
	pci_set_master(pdev);
	g.pdev = pdev;
	g.info.abi = AGPU_ABI_VERSION;
	g.info.bar0_bytes = g.bar0_bytes;
	g.info.dma_bits = AGPU_DMA_BITS;
	agpu_fw_check(pdev);

	g.arena_bytes = (size_t)arena_mib << 20;
	g.heap_bytes = (size_t)heap_mib << 20;
	g.msg_tx = vzalloc(AGPU_MSG_BYTES);
	g.msg_rx = vzalloc(AGPU_MSG_BYTES);
	g.seq_ops = vzalloc(6 * AGPU_SEQ_MAX_OPS * sizeof(u64));
	if (g.arena_bytes)
		g.arena = dma_alloc_coherent(&pdev->dev, g.arena_bytes, &g.arena_iova, GFP_KERNEL);
	if (g.heap_bytes)
		g.heap = dma_alloc_coherent(&pdev->dev, g.heap_bytes, &g.heap_iova, GFP_KERNEL);
	if (!g.msg_tx || !g.msg_rx || !g.seq_ops || (g.arena_bytes && !g.arena) ||
	    (g.heap_bytes && !g.heap)) {
		rc = -ENOMEM;
		goto fail;
	}
	g.info.arena_bytes = g.arena_bytes;
	g.info.arena_iova = g.arena_iova;
	g.info.heap_bytes = g.heap_bytes;
	g.info.heap_iova = g.heap_iova;
	g.info.state = AGPU_STATE_BOUND;
	rc = misc_register(&agpu_misc);
	if (rc)
		goto fail;
	dev_info(&pdev->dev,
		 "bound: BOOT_0 %08x BOOT_42 %08x BAR0 %llu bytes, arena %zu at %pad, heap %zu at %pad\n",
		 g.info.boot0, g.info.boot42, g.bar0_bytes, g.arena_bytes, &g.arena_iova,
		 g.heap_bytes, &g.heap_iova);
	return 0;
fail:
	agpu_free_mem();
	g.pdev = NULL;
	memset(&g.info, 0, sizeof(g.info));
	return rc;
}

static void agpu_remove(struct pci_dev *pdev)
{
	misc_deregister(&agpu_misc);
	mutex_lock(&g.lock);
	agpu_rm_unload(&g);	/* UNLOADING_GUEST_DRIVER and halt wait, when GSP-RM is up */
	agpu_free_mem();
	g.pdev = NULL;
	g_mapping = NULL;
	memset(&g.info, 0, sizeof(g.info));
	mutex_unlock(&g.lock);
}

static const struct pci_device_id agpu_ids[] = {
	{ PCI_DEVICE(0x10de, 0x2e12) },	/* GB10, lspci -nn on the Spark 2026-10-21 */
	{ }
};
MODULE_DEVICE_TABLE(pci, agpu_ids);

static struct pci_driver agpu_driver = {
	.name = AGPU_NAME,
	.id_table = agpu_ids,
	.probe = agpu_probe,
	.remove = agpu_remove,
};

static int __init agpu_init(void)
{
	int rc;

	mutex_init(&g.lock);
	rc = agpu_selftest();
	if (rc)
		return rc;
	return pci_register_driver(&agpu_driver);
}

static void __exit agpu_exit(void)
{
	pci_unregister_driver(&agpu_driver);
}

module_init(agpu_init);
module_exit(agpu_exit);
MODULE_DESCRIPTION("AIEN GB10 GPU driver: our kernel module, NVIDIA's signed GSP firmware");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_FIRMWARE(AGPU_FW);
