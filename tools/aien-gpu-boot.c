// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * aien-gpu-boot: stage B from user space. Builds the GSP boot package into aien-gpu's DMA arena and
 * asks the module to start GSP-RM (AGPU_BOOT); the module runs the register steps and waits for
 * INIT_DONE itself (kmod/aien_gpu_boot.c, om/boot.om).
 *
 * The package builder is aien-dev/aienux c9678d7b native/kernel/dev/gb10_gsp_boot.c (L192, C), linked
 * from the pinned clone (build/oracle). It is a recorded host dependency: its Omega successor is a port
 * of gsp_boot_build to a program-mode .om tool (docs/DESIGN.md section 6). The package files come
 * from tools/package.sh (NVIDIA's signed images on loan, checked against their pins).
 *
 *   aien-gpu-boot --pkg DIR --out DIR [--polls N] [--pause-us N] [--wait-ms N] [--plan-only]
 * --plan-only builds into host memory with a stand-in device address instead of the module's arena
 * and stops after om/abi.om has accepted the block (no device needed).
 * Prints one line AIEN_GPU_BOOT: <verdict> ...; exit 0 = INIT_DONE or PLAN-ONLY, 1 = stopped, 2 = refused.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gb10_gsp_boot.h"
#include "aien_gpu_uapi.h"

uint64_t agpu_om_enter(const void *entry, const uint64_t args[6], uint64_t *status);
extern const char agpu_om_abi[];

static const char *g_out;

static void die(int rc, const char *verdict, const char *what)
{
	fprintf(stderr, "aien-gpu-boot: %s\n", what);
	printf("AIEN_GPU_BOOT: %s\n", verdict);
	exit(rc);
}

static uint8_t *load(const char *dir, const char *name, size_t *n, size_t want)
{
	char p[4096];
	struct stat sb;
	uint8_t *b;
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", dir, name);
	f = fopen(p, "rb");
	if (!f || fstat(fileno(f), &sb))
		die(2, "PACKAGE-MISSING", p);
	if (want && (size_t)sb.st_size != want)
		die(2, "PACKAGE-SIZE", p);
	b = malloc(sb.st_size ? sb.st_size : 1);
	if (!b || fread(b, 1, sb.st_size, f) != (size_t)sb.st_size)
		die(2, "PACKAGE-READ", p);
	fclose(f);
	*n = sb.st_size;
	return b;
}

static void out_bytes(const char *name, const void *p, size_t n)
{
	char path[4096];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_out, name);
	f = fopen(path, "wb");
	if (!f || fwrite(p, 1, n, f) != n || fclose(f))
		die(2, "TOOL-ERROR", path);
}

static void put32(uint8_t *b, unsigned int o, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		b[o + i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *b, unsigned int o, uint64_t v)
{
	put32(b, o, (uint32_t)v);
	put32(b, o + 4, (uint32_t)(v >> 32));
}

static const char *verdict_name(uint32_t v)
{
	static const char *n[] = { "?", "INIT-DONE", "REFUSED", "MMIO-STOPPED", "QUEUE-ERROR", "SEQ-REFUSED",
				   "SEQ-STOPPED", "TIMEOUT", "STATE" };
	return v < sizeof(n) / sizeof(n[0]) ? n[v] : "?";
}

int main(int argc, char **argv)
{
	const char *pkg = NULL;
	uint32_t polls = 50000, pause_us = 100, wait_ms = 30000, app_version;
	int plan_only = 0, fd = -1;
	size_t n_elf, n_sig, n_bl, n_fmc, n_h, n_k, n_s, n_si, n_app;
	gsp_boot_in in = { 0 };
	static gsp_boot_plan plan;
	static struct agpu_boot req;
	struct agpu_info info = { 0 };
	uint8_t *base, *si;
	uint64_t cot_off, st, verdict;
	const char *why = NULL;
	char *app;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--plan-only")) {
			plan_only = 1;
			continue;
		}
		if (!strcmp(a, "--info")) {	/* AGPU_INFO as one line; touches nothing */
			fd = open("/dev/aien-gpu", O_RDWR);
			if (fd < 0 || ioctl(fd, AGPU_INFO, &info))
				die(2, "NO-DEVICE", strerror(errno));
			printf("AIEN_GPU_INFO: abi=%u state=%u boot0=%#x boot42=%#x bar0=%llu fw_bytes=%llu fw_ok=%u arena=%#llx@%#llx heap=%#llx@%#llx dma_bits=%u boot_attempts=%u\n",
			       info.abi, info.state, info.boot0, info.boot42, (unsigned long long)info.bar0_bytes,
			       (unsigned long long)info.fw_bytes, info.fw_ok, (unsigned long long)info.arena_bytes,
			       (unsigned long long)info.arena_iova, (unsigned long long)info.heap_bytes,
			       (unsigned long long)info.heap_iova, info.dma_bits, info.boot_attempts);
			return 0;
		}
		if (!v)
			die(2, "USAGE", a);
		i++;
		if (!strcmp(a, "--pkg"))
			pkg = v;
		else if (!strcmp(a, "--out"))
			g_out = v;
		else if (!strcmp(a, "--polls"))
			polls = strtoul(v, NULL, 0);
		else if (!strcmp(a, "--pause-us"))
			pause_us = strtoul(v, NULL, 0);
		else if (!strcmp(a, "--wait-ms"))
			wait_ms = strtoul(v, NULL, 0);
		else
			die(2, "USAGE", a);
	}
	if (!pkg || !g_out)
		die(2, "USAGE", "need --pkg DIR --out DIR");
	if (mkdir(g_out, 0755) && errno != EEXIST)
		die(2, "TOOL-ERROR", g_out);

	in.rm_elf = load(pkg, "rm_elf.bin", &n_elf, 0);
	in.rm_sig = load(pkg, "rm_sig.bin", &n_sig, 0);
	in.bootloader = load(pkg, "bootloader.bin", &n_bl, GSP_GB10B_BOOTLOADER_SIZE);
	in.fmc = load(pkg, "fmc.bin", &n_fmc, 0);
	in.fmc_hash = load(pkg, "fmc_hash.bin", &n_h, GSP_COT_HASH_SIZE);
	in.fmc_pubkey = load(pkg, "fmc_pkey.bin", &n_k, GSP_COT_KEY_SIZE);
	in.fmc_sig = load(pkg, "fmc_sig.bin", &n_s, GSP_COT_SIG_SIZE);
	si = load(pkg, "system_info.bin", &n_si, GSP_SYSTEM_INFO_SIZE);
	app = (char *)load(pkg, "app_version.txt", &n_app, 0);
	app_version = strtoul(app, NULL, 10);
	in.rm_elf_size = n_elf;
	in.rm_sig_size = n_sig;
	in.bootloader_size = n_bl;
	in.fmc_size = n_fmc;
	in.bl_code_off = GSP_GB10B_BL_CODE_OFF;
	in.bl_data_off = GSP_GB10B_BL_DATA_OFF;
	in.bl_manifest_off = GSP_GB10B_BL_MANIFEST_OFF;

	if (plan_only) {
		/* host memory and stand-in device addresses below 40 bits */
		info.arena_bytes = 192ull << 20;
		info.arena_iova = 0x0000004000000000ull;
		info.heap_bytes = 512ull << 20;
		info.heap_iova = 0x0000005000000000ull;
		info.dma_bits = 40;
		base = aligned_alloc(GSP_PAGE, info.arena_bytes);
		if (!base)
			die(2, "TOOL-ERROR", "no host memory for the arena");
		memset(base, 0, info.arena_bytes);
	} else {
		fd = open("/dev/aien-gpu", O_RDWR);
		if (fd < 0 || ioctl(fd, AGPU_INFO, &info))
			die(2, "NO-DEVICE", strerror(errno));
		if (info.state != AGPU_STATE_BOUND || !info.fw_ok || !info.heap_bytes || !info.arena_bytes)
			die(2, "NOT-READY", "module not in state BOUND with firmware, heap and arena");
		base = mmap(NULL, info.arena_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (base == MAP_FAILED)
			die(2, "NO-DEVICE", "mmap arena");
	}
	/* Arena page 0: the zeroed notify-op surface (kernel_gsp.c:3489-3526); its address goes into
	 * GspSystemInfo.notifyOpSharedSurfacePhysAddr (offset 48, gsp_static_config.h:180) unless set. */
	memset(base, 0, GSP_PAGE);
	{
		uint64_t notify = 0;

		for (int i = 0; i < 8; i++)
			notify |= (uint64_t)si[48 + i] << (8 * i);
		if (!notify)
			put64(si, 48, info.arena_iova);
	}
	in.system_info = si;
	in.sysmem_heap_pa = info.heap_iova;
	in.sysmem_heap_size = info.heap_bytes;
	in.dma_bits = info.dma_bits;
	out_bytes("system_info.sent.bin", si, GSP_SYSTEM_INFO_SIZE);

	gsp_arena arena;
	if (gsp_arena_init(&arena, base + GSP_PAGE, info.arena_iova + GSP_PAGE, info.arena_bytes - GSP_PAGE))
		die(2, "BUILD-REFUSED", "arena not 4 KiB aligned");
	if (gsp_boot_build(&arena, &in, &plan, &why))
		die(2, "BUILD-REFUSED", why);
	/* the COT packet goes into the arena right after the built blocks, where the module reads it */
	cot_off = GSP_PAGE + ((arena.used + GSP_PAGE - 1) & ~(uint64_t)(GSP_PAGE - 1));
	if (cot_off + GSP_EMEM_CHANNEL_SIZE > info.arena_bytes)
		die(2, "BUILD-REFUSED", "no room for the COT packet");
	memcpy(base + cot_off, plan.cot_packet, plan.cot_packet_len);
	out_bytes("cot_packet.bin", plan.cot_packet, plan.cot_packet_len);

	memset(req.in, 0, sizeof(req.in));
	put32(req.in, 0, 1);
	put64(req.in, 8, cot_off);
	put32(req.in, 16, plan.cot_packet_len);
	put32(req.in, 20, app_version);
	put64(req.in, 24, plan.fmc_params.pa);
	put64(req.in, 32, plan.cmdq_pa - info.arena_iova);
	put64(req.in, 40, plan.statq_pa - info.arena_iova);
	for (int i = 0; i < 4; i++)
		put32(req.in, 48 + 4 * i, polls);
	put32(req.in, 64, pause_us);
	put32(req.in, 68, wait_ms);
	put32(req.in, 72, plan.tx_seq);
	out_bytes("boot-block.bin", req.in, sizeof(req.in));
	/* the same Omega check the module runs, so a bad block is caught before the device */
	{
		const uint64_t a[6] = { 1, (uint64_t)(uintptr_t)req.in, info.arena_iova, info.arena_bytes, 0, 0 };

		st = 0;
		verdict = agpu_om_enter(agpu_om_abi, a, &st);
		if (st || verdict) {
			fprintf(stderr, "om/abi.om status %llu verdict %llu\n", (unsigned long long)st,
				(unsigned long long)verdict);
			die(2, "BLOCK-REFUSED", "om/abi.om refused the boot block");
		}
	}
	if (plan_only) {
		printf("AIEN_GPU_BOOT: PLAN-ONLY arena used %#llx of %#llx, cot %u bytes at %#llx, app_version %u, tx_seq %u\n",
		       (unsigned long long)arena.used, (unsigned long long)arena.cap, plan.cot_packet_len,
		       (unsigned long long)cot_off, app_version, plan.tx_seq);
		return 0;
	}
	if (ioctl(fd, AGPU_BOOT, &req))
		die(2, "IOCTL-ERROR", strerror(errno));
	out_bytes("boot-out.bin", &req.out, sizeof(req.out));
	/* evidence, pass or fail: GSP-RM's log buffers and the RPC queues, copied out by AGPU_ARENA_READ
	 * (the arena mapping was revoked when the start-up began) */
	for (unsigned int i = 0; i <= GSP_GB10B_LOG_COUNT; i++) {
		gsp_region r = i < GSP_GB10B_LOG_COUNT ? plan.log[i] : plan.rpc_shared;
		struct agpu_arena_read rd = { r.pa - info.arena_iova, r.size, 0 };
		uint8_t *buf = malloc(r.size ? r.size : 1);
		char name[64];

		rd.buf = (uint64_t)(uintptr_t)buf;
		snprintf(name, sizeof(name), i < GSP_GB10B_LOG_COUNT ? "gsp-log-%u.bin" : "rpc-shared.bin", i);
		if (buf && r.size && !ioctl(fd, AGPU_ARENA_READ, &rd))
			out_bytes(name, buf, r.size);
		free(buf);
	}
	printf("AIEN_GPU_BOOT: %s code=%u stage=%u polls=%u,%u,%u,%u sec2_mb0=%#x gsp_mb0=%#x cpuctl=%#x msgs=%u last_fn=%#x seq_runs=%u mmio_us=%llu wait_us=%llu\n",
	       verdict_name(req.out.verdict), req.out.code, req.out.stage, req.out.polls[0], req.out.polls[1],
	       req.out.polls[2], req.out.polls[3], req.out.sec2_mb0, req.out.gsp_mb0, req.out.cpuctl, req.out.msgs,
	       req.out.last_fn, req.out.seq_runs, (unsigned long long)req.out.mmio_us,
	       (unsigned long long)req.out.wait_us);
	return req.out.verdict == AGPU_BOOT_INIT_DONE ? 0 : 1;
}
