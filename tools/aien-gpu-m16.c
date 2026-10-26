// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * aien-gpu-m16 -- the CUDA++ m16 gate over /dev/aien-gpu (stage C), with no NVIDIA user space and no
 * NVIDIA kernel module: one compute channel, one semaphore release of 0x16c0ffee into GPU-uncached
 * memory, doorbell causality.
 * Same pushbuffer and submission as aien-dev/CUDApp m16 (m16/m16_requalify.c, m16/m16_native.c
 * m16_native_build_release, nvrm/nvrm.c nvrm_gp_entry / nvrm_enqueue / nvrm_ring):
 *   subchannel 1 SET_OBJECT <compute class GSP-RM reported>, subchannel 0 SEM_ADDR_LO/HI,
 *   SEM_PAYLOAD_LO/HI, SEM_EXECUTE = RELEASE | RELEASE_WFI (clc96f.h:75-89); one GP entry
 *   (va | length << 42 | 1 << 41, exactly nvrm_gp_entry; see the note at gp_entry); USERD GPPut
 *   (+0x8c, clc96f.h:29-33); doorbell store of the work-submit token at usermode +0x90.
 * Order: NEGATIVE CONTROL first (entry and GPPut written, doorbell withheld: the marker must stay 0
 * for 100 ms), then the doorbell and a bounded wait for the marker.
 *
 *   aien-gpu-m16 [--device /dev/aien-gpu] [--evidence DIR] [--wait-ms N] [--rm-init] [--dry-run]
 *
 * --rm-init: if the device is in GSP_UP, run AGPU_RM_INIT first (root) and save the stage-C bring-up
 *   evidence (rm-init.txt, static-info.bin); with --rm-init-only, stop there.
 * --dry-run: check the arguments, the ABI sizes and the encodings, print what would be sent; no device.
 * Prints exactly one verdict line, AIEN_GPU_M16: PASS | FAIL <why> | ERROR <step> <detail> |
 * DRY-RUN ok, or AIEN_GPU_RMINIT: OK | FAIL ... for --rm-init-only.
 * Exit 0 PASS / DRY-RUN / RMINIT OK, 1 FAIL, 2 ERROR (could not run the gate).
 * Status: IMPLEMENTED, builds and dry-runs on the host; hardware NOT_RUN.
 */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aien_gpu_uapi.h"

#define PAYLOAD 0x16c0ffeeu
#define ENTRIES 1024u
#define FIFO_BYTES 0x10000ull		/* ring (8 KiB) then USERD at USERD_OFF, one object */
#define USERD_OFF 0x2000ull
#define USERD_GPPUT 0x8cu		/* Nvc96fControl.GPPut */
#define DOORBELL 0x90u			/* NVC361_NOTIFY_CHANNEL_PENDING */
#define PAGE 4096ull
#define VA_BASE 0x4000000000ull		/* 256 GiB: above GSP-RM's [4, 4.5) GiB, below VA_LIMIT */
#define VA_LIMIT 0x10000000000ull	/* 2^40, the GP entry address width */

static uint64_t g_next_va = VA_BASE;

static const char *g_ev = "aien-gpu-m16-evidence";
static FILE *g_log;
static int g_rm_only;

static void ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ev(const char *fmt, ...)
{
	va_list ap;

	if (!g_log)
		return;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fflush(g_log);
}

static int verdict(int rc, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int verdict(int rc, const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s: %s\n", g_rm_only ? "AIEN_GPU_RMINIT" : "AIEN_GPU_M16", line);
	ev("verdict=%s\n", line);
	fflush(stdout);
	return rc;
}

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	__sync_synchronize();
#endif
}

/* nvrm_mthd: incrementing method header (SEC_OP 2 in 31:29), count 28:16, subchannel 15:13, method/4 */
static uint32_t mthd(uint32_t subc, uint32_t m, uint32_t count)
{
	return (2u << 28) | (count << 16) | (subc << 13) | (m >> 2);
}

/*
 * nvrm_gp_entry, byte for byte (what the m16 receipt ran on the chip). Note: bit 41 is ENTRY1 bit 9,
 * LEVEL, and clc96f.h:99-101 calls 1 LEVEL_SUBROUTINE while nvrm.c's comment says MAIN; kept as the
 * chip ran it, recorded as an open question in docs/STAGE-C.md.
 */
static uint64_t gp_entry(uint64_t va, uint32_t nwords)
{
	return (va & ~3ull) | ((uint64_t)(nwords & 0x1fffff) << 42) | (1ull << 41);
}

static int build_release(uint32_t *w, uint64_t marker_va, uint32_t cls)
{
	int n = 0;

	w[n++] = mthd(1, 0x0000, 1);		/* SET_OBJECT */
	w[n++] = cls;
	w[n++] = mthd(0, 0x005c, 5);		/* SEM_ADDR_LO .. SEM_EXECUTE */
	w[n++] = (uint32_t)marker_va;
	w[n++] = (uint32_t)(marker_va >> 32);
	w[n++] = PAYLOAD;
	w[n++] = 0;
	w[n++] = 0x1 | (1u << 20);		/* OPERATION_RELEASE | RELEASE_WFI */
	return n;
}

struct mem {
	uint32_t handle;
	uint64_t va, size;
	void *cpu;
};

static int mem_alloc(int fd, uint64_t size, uint32_t flags, struct mem *m, const char *name)
{
	struct agpu_mem_alloc a;
	void *va;

	memset(m, 0, sizeof(*m));
	/*
	 * Below 2^40: a GP entry carries a 40-bit pushbuffer address (ENTRY0 GET 31:2, ENTRY1 GET_HI
	 * 7:0, clc96f.h:95-99), so every object of this gate lives in [VA_BASE, 2^40), above GSP's
	 * [4, 4.5) GiB. MAP_FIXED_NOREPLACE: never over an existing mapping.
	 */
	va = mmap((void *)(uintptr_t)g_next_va, size, PROT_NONE,
		  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if (va == MAP_FAILED)
		return -errno;
	if ((uint64_t)(uintptr_t)va != g_next_va || g_next_va + size > VA_LIMIT) {
		munmap(va, size);
		return -ERANGE;
	}
	g_next_va += size + PAGE;	/* a guard page between objects */
	memset(&a, 0, sizeof(a));
	a.size = size;
	a.va = (uint64_t)(uintptr_t)va;
	a.flags = flags;
	if (ioctl(fd, AGPU_MEM_ALLOC, &a) || a.st.verdict != AGPU_RM_OK) {
		int e = errno;

		ev("mem_alloc.%s verdict=%u code=%u step=%u errno=%d\n", name, a.st.verdict, a.st.code,
		   a.st.step, e);
		munmap(va, size);
		return -EIO;
	}
	m->handle = a.handle;
	m->va = a.va;
	m->size = size;
	m->cpu = mmap(va, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)a.mmap_off);
	if (m->cpu == MAP_FAILED) {
		m->cpu = NULL;
		return -errno;
	}
	ev("mem.%s handle=%u va=0x%llx size=0x%llx flags=%u mmap_off=0x%llx\n", name, a.handle,
	   (unsigned long long)a.va, (unsigned long long)size, flags, (unsigned long long)a.mmap_off);
	return 0;
}

static void mem_free(int fd, struct mem *m)
{
	struct agpu_mem_free f;

	if (m->cpu)
		munmap(m->cpu, m->size);
	if (!m->handle)
		return;
	memset(&f, 0, sizeof(f));
	f.handle = m->handle;
	if (ioctl(fd, AGPU_MEM_FREE, &f) || f.st.verdict != AGPU_RM_OK)
		ev("mem_free handle=%u verdict=%u code=%u\n", m->handle, f.st.verdict, f.st.code);
	m->handle = 0;
}

static int libcuda_mapped(void)
{
	char line[512];
	FILE *f = fopen("/proc/self/maps", "r");
	int hit = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, "libcuda") || strstr(line, "libcudart") || strstr(line, "libnvidia"))
			hit = 1;
	fclose(f);
	return hit;
}

static int rm_init(int fd)
{
	struct agpu_rm_init *r = calloc(1, sizeof(*r));
	char p[1024];
	FILE *f;
	int rc;

	if (!r)
		return -ENOMEM;
	rc = ioctl(fd, AGPU_RM_INIT, r);
	ev("rm_init ioctl=%d errno=%d verdict=%u code=%u fn=%u step=%u vgx=%u.%u\n", rc, rc ? errno : 0,
	   r->st.verdict, r->st.code, r->st.fn, r->st.step, r->vgx_major, r->vgx_minor);
	ev("rm_init h_client=0x%08x h_device=0x%08x h_subdevice=0x%08x gr_runlist=%u num_channels=%u\n",
	   r->h_client, r->h_device, r->h_subdevice, r->gr_runlist, r->num_channels);
	ev("rm_init fault_mthd_bytes=%u gr_main_ctx_bytes=%u max_subctx=%u compute_class=0x%x gpfifo_class=0x%x\n",
	   r->fault_mthd_bytes, r->gr_main_ctx_bytes, r->max_subctx, r->compute_class, r->gpfifo_class);
	snprintf(p, sizeof(p), "%s/static-info.bin", g_ev);
	f = fopen(p, "wb");
	if (f) {
		fwrite(r->static_info, 1, sizeof(r->static_info), f);
		fclose(f);
	}
	rc = rc ? -errno : r->st.verdict == AGPU_RM_OK ? 0 : -(int)(1000 + r->st.verdict);
	free(r);
	return rc;
}

static int dry_run(uint32_t wait_ms)
{
	uint32_t w[16];
	int n = build_release(w, 0x4000002000ull, 0xcec0);

	/* the ABI sizes user space and the module share (aien_gpu_uapi.h) */
	if (sizeof(struct agpu_rm_status) != 16 || sizeof(struct agpu_mem_alloc) != 48 ||
	    sizeof(struct agpu_chan_create) != 72 || sizeof(struct agpu_chan_destroy) != 24 ||
	    sizeof(struct agpu_rm_init) != 16 + 4 * 14 + 1792)
		return verdict(2, "ERROR abi struct sizes");
	/* 0x224000001000: computed with Wolfram (BitOr of 0x4000001000, 8 << 42, 1 << 41) */
	if (gp_entry(0x4000001000ull, 8) != 0x224000001000ull)
		return verdict(2, "ERROR gp_entry encoding");
	if (n != 8 || w[0] != 0x20012000u || w[2] != 0x20050017u || w[7] != 0x00100001u)
		return verdict(2, "ERROR method encoding");
	printf("dry-run: methods %08x %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4],
	       w[5], w[6], w[7]);
	printf("dry-run: gp entry for 0x4000001000 x8 = 0x%016llx, wait %u ms\n",
	       (unsigned long long)gp_entry(0x4000001000ull, 8), wait_ms);
	return verdict(0, "DRY-RUN ok entries=%u userd_off=0x%llx wait_ms=%u", ENTRIES,
		       (unsigned long long)USERD_OFF, wait_ms);
}

static int usage(void)
{
	fprintf(stderr, "usage: aien-gpu-m16 [--device PATH] [--evidence DIR] [--wait-ms 1..60000] "
			"[--rm-init | --rm-init-only] [--dry-run]\n");
	return verdict(2, "ERROR arguments");
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/aien-gpu";
	struct mem fifo = { 0 }, pb = { 0 }, marker = { 0 }, err = { 0 };
	struct agpu_chan_create cc;
	struct agpu_chan_destroy cd;
	struct agpu_info info;
	uint32_t wait_ms = 2000, w[16], *db_word;
	volatile uint32_t *mk;
	int do_init = 0, dry = 0, fd, rc, n, i, result;
	uint64_t t0, t1;
	void *db = MAP_FAILED;
	char p[1024];
	long v;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run")) {
			dry = 1;
		} else if (!strcmp(argv[i], "--rm-init")) {
			do_init = 1;
		} else if (!strcmp(argv[i], "--rm-init-only")) {
			do_init = 1;
			g_rm_only = 1;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			dev = argv[++i];
		} else if (!strcmp(argv[i], "--evidence") && i + 1 < argc) {
			g_ev = argv[++i];
		} else if (!strcmp(argv[i], "--wait-ms") && i + 1 < argc) {
			char *end;

			v = strtol(argv[++i], &end, 10);
			if (*end || v < 1 || v > 60000)
				return usage();
			wait_ms = (uint32_t)v;
		} else {
			return usage();
		}
	}
	if (dry)
		return dry_run(wait_ms);

	if (mkdir(g_ev, 0755) && errno != EEXIST)
		return verdict(2, "ERROR evidence-dir %s", strerror(errno));
	snprintf(p, sizeof(p), "%s/%s.txt", g_ev, g_rm_only ? "rm-init" : "m16");
	g_log = fopen(p, "w");
	if (!g_log)
		return verdict(2, "ERROR evidence-file %s", strerror(errno));
	ev("tool=aien-gpu-m16 device=%s wait_ms=%u pid=%d\n", dev, wait_ms, (int)getpid());
	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return verdict(2, "ERROR open %s", strerror(errno));
	if (ioctl(fd, AGPU_INFO, &info))
		return verdict(2, "ERROR info %s", strerror(errno));
	ev("info abi=%u state=%u boot0=0x%08x boot42=0x%08x\n", info.abi, info.state, info.boot0, info.boot42);
	if (info.abi != AGPU_ABI_VERSION)
		return verdict(2, "ERROR abi %u", info.abi);
	if (do_init && info.state == AGPU_STATE_GSP_UP) {
		rc = rm_init(fd);
		if (rc)
			return verdict(g_rm_only ? 1 : 2, "%s rm-init %d", g_rm_only ? "FAIL" : "ERROR", rc);
		if (ioctl(fd, AGPU_INFO, &info))
			return verdict(2, "ERROR info %s", strerror(errno));
	}
	if (g_rm_only)
		return verdict(info.state == AGPU_STATE_RM_READY ? 0 : 1, "%s state=%u",
			       info.state == AGPU_STATE_RM_READY ? "OK" : "FAIL", info.state);
	if (info.state != AGPU_STATE_RM_READY)
		return verdict(2, "ERROR state %u (want %u RM_READY)", info.state, AGPU_STATE_RM_READY);

	rc = mem_alloc(fd, FIFO_BYTES, AGPU_MEM_GPU_UNCACHED, &fifo, "fifo") ?:
	     mem_alloc(fd, PAGE, AGPU_MEM_GPU_UNCACHED, &pb, "pushbuffer") ?:
	     mem_alloc(fd, PAGE, AGPU_MEM_GPU_UNCACHED, &marker, "marker") ?:
	     mem_alloc(fd, PAGE, AGPU_MEM_GPU_UNCACHED, &err, "notifier");
	if (rc) {
		result = verdict(2, "ERROR mem-alloc %d", rc);
		goto out;
	}
	memset(&cc, 0, sizeof(cc));
	cc.ring_handle = fifo.handle;
	cc.entries = ENTRIES;
	cc.ring_off = 0;
	cc.userd_handle = fifo.handle;
	cc.userd_off = USERD_OFF;
	cc.err_handle = err.handle;
	rc = ioctl(fd, AGPU_CHAN_CREATE, &cc);
	ev("chan_create ioctl=%d verdict=%u code=%u fn=%u step=%u chid=%u token=0x%08x channel=%u compute_class=0x%x doorbell_off=0x%llx\n",
	   rc, cc.st.verdict, cc.st.code, cc.st.fn, cc.st.step, cc.chid, cc.token, cc.channel,
	   cc.compute_class, (unsigned long long)cc.doorbell_off);
	if (rc || cc.st.verdict != AGPU_RM_OK) {
		result = verdict(2, "ERROR chan-create verdict=%u code=%u step=%u", cc.st.verdict,
				 cc.st.code, cc.st.step);
		goto out;
	}
	db = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)cc.doorbell_off);
	if (db == MAP_FAILED) {
		result = verdict(2, "ERROR doorbell-mmap %s", strerror(errno));
		goto destroy;
	}
	db_word = (uint32_t *)((uint8_t *)db + DOORBELL);
	mk = (volatile uint32_t *)marker.cpu;
	*mk = 0;
	n = build_release(w, marker.va, cc.compute_class);
	memcpy(pb.cpu, w, n * sizeof(uint32_t));
	barrier();

	/* NEGATIVE CONTROL: entry and GPPut written, doorbell withheld */
	((volatile uint64_t *)fifo.cpu)[0] = gp_entry(pb.va, n);
	barrier();
	*(volatile uint32_t *)((uint8_t *)fifo.cpu + USERD_OFF + USERD_GPPUT) = 1;
	barrier();
	usleep(100000);
	ev("negative_control marker_after_100ms=0x%08x\n", *mk);
	if (*mk != 0) {
		result = verdict(1, "FAIL negative-control marker=0x%08x without doorbell", *mk);
		goto destroy;
	}
	/* POSITIVE: the doorbell */
	t0 = now_ns();
	*(volatile uint32_t *)db_word = cc.token;
	barrier();
	t1 = t0 + (uint64_t)wait_ms * 1000000ull;
	while (*mk != PAYLOAD && now_ns() < t1)
		usleep(50);
	ev("positive marker=0x%08x after_us=%llu\n", *mk, (unsigned long long)((now_ns() - t0) / 1000));
	rc = libcuda_mapped();
	ev("libcuda_mapped=%d\n", rc);
	if (*mk != PAYLOAD)
		result = verdict(1, "FAIL timeout marker=0x%08x after %u ms", *mk, wait_ms);
	else if (rc != 0)
		result = verdict(1, "FAIL libcuda-mapped=%d", rc);
	else
		result = verdict(0, "PASS marker=0x%08x chid=%u token=0x%08x class=0x%x", *mk, cc.chid,
				 cc.token, cc.compute_class);
destroy:
	if (db != MAP_FAILED)
		munmap(db, 0x10000);
	memset(&cd, 0, sizeof(cd));
	cd.channel = cc.channel;
	if (ioctl(fd, AGPU_CHAN_DESTROY, &cd) || cd.st.verdict != AGPU_RM_OK)
		fprintf(stderr, "aien-gpu-m16: channel destroy verdict %u code %u\n", cd.st.verdict,
			cd.st.code);
out:
	mem_free(fd, &err);
	mem_free(fd, &marker);
	mem_free(fd, &pb);
	mem_free(fd, &fifo);
	close(fd);
	if (g_log)
		fclose(g_log);
	return result;
}
