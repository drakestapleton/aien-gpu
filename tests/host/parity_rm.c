/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* parity_rm -- the stage-C Omega units kmod/aien_gpu_rm.c runs (om/rm.om, om/rmchan.om, om/rmctl.om,
 * om/rmchk.om, om/rmarg.om) against:
 *  1. NVIDIA's own 580.173.02 structures (MIT SDK headers, NVSRC): every builder's payload is
 *     compared byte for byte with the C struct filled from the same inputs the way the cited
 *     nvidia.ko source fills it (DRF macros from nvmisc.h and the class/ctrl headers);
 *  2. the aienux c9678d7b oracle gsp_rpc_rm_alloc / gsp_rpc_rm_control / gsp_rpc_rm_free /
 *     gsp_rpc_get_static_info: the whole 256 KiB command queue after om build + om/seal.om +
 *     om/mqput.om must equal the oracle's queue;
 *  3. replies built from the same headers for every check, with each refusal class exercised, and
 *     ChID allocation against a C bitmap model.
 * Layouts the SDK does not export (rpc_gsp_rm_alloc_v03_00, rpc_gsp_rm_control_v03_00,
 * rpc_set_guest_system_info_v03_00, GspStaticConfigInfo handles) use the offsets compiled from the
 * RM headers in docs/STAGE-C.md section 0. Prints PARITY-RM PASS checks=N or the first difference. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omhost.h"
#include "gb10_gsp_boot.h"

#include "nvtypes.h"
#include "nvmisc.h"
#include "nvos.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl2080_notification.h"
#include "class/cl9067.h"
#include "class/cl90f1.h"
#include "class/cla06c.h"
#include "class/clc96f.h"
#include "class/clca6f.h"
#include "class/clcdc0.h"
#include "class/clcec0.h"
#include "alloc/alloc_channel.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl0080/ctrl0080gr.h"
#include "ctrl/ctrl0080/ctrl0080fifo.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080fifo.h"
#include "ctrl/ctrl2080/ctrl2080ce.h"
#include "ctrl/ctrl2080/ctrl2080internal.h"
#include "ctrl/ctrla06c.h"
#include "ctrl/ctrl90f1.h"
#include "swref/published/blackwell/gb100/dev_vm.h"

extern const char agpu_om_rm[], agpu_om_rmchan[], agpu_om_rmctl[], agpu_om_rmchk[], agpu_om_rmarg[];

#define FAIL 1000000ull
static unsigned long checks;
static uint8_t msg[65536], want[65536];
static uint64_t a[64];

static void die(const char *what, uint64_t got, uint64_t w)
{
	printf("PARITY-RM FAIL %s: got %llu want %llu\n", what, (unsigned long long)got,
	       (unsigned long long)w);
	exit(1);
}

static void eq(const char *what, uint64_t got, uint64_t w)
{
	checks++;
	if (got != w)
		die(what, got, w);
}

static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint64_t call3(const void *u, uint64_t op, void *m, void *b)
{
	uint64_t st = 0, r = om_call(u, op, P(m), P(b), 0, 0, 0, &st);

	if (st)
		die("omega trap", st, 0);
	return r;
}

/* build with the unit that owns op, as kmod/aien_gpu_rm.c rm_rpc does */
static uint64_t build(uint64_t op)
{
	const void *u = op == 9 ? agpu_om_rmchan : op >= 11 ? agpu_om_rmctl : agpu_om_rm;

	memset(msg, 0xa5, sizeof(msg));		/* stale bytes the builder must overwrite */
	return call3(u, op, msg, a);
}

/* payload at 80 equals want[0..n), and function / rpclen / placed */
static void same(const char *what, uint32_t n, uint64_t fn, uint64_t rlen, uint64_t placed)
{
	checks++;
	if (memcmp(msg + 80, want, n)) {
		for (uint32_t i = 0; i < n; i++)
			if (msg[80 + i] != want[i]) {
				printf("PARITY-RM FAIL %s: payload byte %u omega %02x want %02x\n", what, i,
				       msg[80 + i], want[i]);
				exit(1);
			}
	}
	eq(what, a[48], fn);
	eq(what, a[49], rlen);
	eq(what, a[50], placed);
}

/* rpc_gsp_rm_alloc_v03_00 (32 bytes) then params */
static uint32_t alloc_hdr(uint32_t hc, uint32_t hp, uint32_t ho, uint32_t cls, const void *p, uint32_t n)
{
	memset(want, 0, 32 + n);
	w32(want, hc);
	w32(want + 4, hp);
	w32(want + 8, ho);
	w32(want + 12, cls);
	w32(want + 20, n);
	if (n)
		memcpy(want + 32, p, n);
	return 32 + n;
}

/* rpc_gsp_rm_control_v03_00 (40 bytes) then params */
static uint32_t ctl_hdr(uint32_t hc, uint32_t ho, uint32_t cmd, const void *p, uint32_t n)
{
	memset(want, 0, 40 + n);
	w32(want, hc);
	w32(want + 4, ho);
	w32(want + 8, cmd);
	w32(want + 16, n);
	memcpy(want + 40, p, n);
	return 40 + n;
}

static void set4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	memset(a, 0, sizeof(a));
	a[0] = a0;
	a[1] = a1;
	a[2] = a2;
	a[3] = a3;
}

static void test_alloc(void)
{
	const uint32_t C = 0xc1d80001u, D = 0xcaf00001u, S = 0xcaf00002u, V = 0xcaf00003u, T = 0xcaf00004u;
	uint32_t n;

	/* SET_GUEST_SYSTEM_INFO (rpc.c:8653-8834; offsets docs/STAGE-C.md 1.2) */
	memset(a, 0, sizeof(a));
	eq("sgsi", build(1), 0);
	memset(want, 0, 792);
	w32(want, 0x2B);
	w32(want + 4, 0x13);
	w32(want + 8, 256);
	w32(want + 12, 256);
	w32(want + 16, 256);
	w32(want + 20, 38369854);
	memcpy(want + 24, "580.173.02", 10);
	memcpy(want + 280, "rel/gpu_drv/r580/r582_69-609", 28);
	memcpy(want + 536, "aien-gpu", 8);
	same("sgsi", 792, 1, 792, 792);
	/* GET_GSP_STATIC_INFO: zeroed 1792 */
	eq("static", build(2), 0);
	memset(want, 0, 1792);
	same("static", 1792, 65, 1792, 1792);

	NV0000_ALLOC_PARAMETERS p0;
	memset(&p0, 0, sizeof(p0));
	p0.hClient = C;
	p0.processID = 4242;
	set4(C, 4242, 0, 0);
	eq("root", build(3), 0);
	n = alloc_hdr(C, 0, C, NV01_ROOT, &p0, sizeof(p0));
	same("root", n, 103, 32, n);

	NV0080_ALLOC_PARAMETERS p1;
	memset(&p1, 0, sizeof(p1));
	p1.deviceId = 0;
	p1.hClientShare = C;
	p1.vaMode = NV_DEVICE_ALLOCATION_VAMODE_OPTIONAL_MULTIPLE_VASPACES;
	set4(C, D, 0, 0);
	eq("device", build(4), 0);
	n = alloc_hdr(C, C, D, NV01_DEVICE_0, &p1, sizeof(p1));
	same("device", n, 103, 32, n);

	NV2080_ALLOC_PARAMETERS p2 = { .subDeviceId = 0 };
	set4(C, D, S, 0);
	eq("subdevice", build(5), 0);
	n = alloc_hdr(C, D, S, NV20_SUBDEVICE_0, &p2, sizeof(p2));
	same("subdevice", n, 103, 32, n);

	NV_VASPACE_ALLOCATION_PARAMETERS p3;
	memset(&p3, 0, sizeof(p3));
	p3.index = NV_VASPACE_ALLOCATION_INDEX_GPU_NEW;
	p3.flags = NV_VASPACE_ALLOCATION_FLAGS_NONE;
	p3.vaSize = 1ull << 49;
	set4(C, D, V, 1ull << 49);
	eq("vaspace", build(6), 0);
	n = alloc_hdr(C, D, V, FERMI_VASPACE_A, &p3, sizeof(p3));
	same("vaspace", n, 103, 32, n);
	set4(C, D, V, (1ull << 49) + 4096);
	eq("vaspace unaligned", build(6), FAIL + 4);
	set4(C, D, V, 0);
	eq("vaspace 0", build(6), FAIL + 4);

	NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS p4;
	memset(&p4, 0, sizeof(p4));
	p4.hVASpace = V;
	p4.engineType = NV2080_ENGINE_TYPE_GRAPHICS;
	set4(C, D, T, V);
	eq("tsg", build(7), 0);
	n = alloc_hdr(C, D, T, KEPLER_CHANNEL_GROUP_A, &p4, sizeof(p4));
	same("tsg", n, 103, 32, n);

	NV_CTXSHARE_ALLOCATION_PARAMETERS p5;
	memset(&p5, 0, sizeof(p5));
	p5.hVASpace = V;
	p5.flags = DRF_DEF(_CTXSHARE, _ALLOCATION_FLAGS, _SUBCONTEXT, _ASYNC);
	set4(C, T, T + 1, V);
	eq("ctxshare", build(8), 0);
	n = alloc_hdr(C, T, T + 1, FERMI_CONTEXT_SHARE_A, &p5, sizeof(p5));
	same("ctxshare", n, 103, 32, n);

	set4(C, T + 2, T + 3, BLACKWELL_COMPUTE_B);
	eq("compute", build(10), 0);
	n = alloc_hdr(C, T + 2, T + 3, BLACKWELL_COMPUTE_B, NULL, 0);
	same("compute", n, 103, 32, 32);

	/* handle refusals */
	set4(0, D, 0, 0);
	eq("device hClient 0", build(4), FAIL + 2);
	set4(C, D, S, 0);
	a[1] = 1ull << 32;
	eq("subdevice handle > 32 bits", build(5), FAIL + 3);
	set4(C, D, T, 0);
	eq("tsg hVas 0", build(7), FAIL + 2);
	set4(C, D, S, 0);
	eq("unknown op", build(9 + 90), FAIL + 1);
}

/* NV_CHANNEL_ALLOC_PARAMS as _kchannelSendChannelAllocRpc fills it for a GSP client */
static void chan_inputs(uint32_t chid, uint32_t entries)
{
	memset(a, 0, sizeof(a));
	a[0] = 0xc1d80001u;
	a[1] = 0xcaf00004u;
	a[2] = 0xcaf00006u;
	a[3] = BLACKWELL_CHANNEL_GPFIFO_A;
	a[4] = 0x40000003u;
	a[5] = 0x7f0012340000ull;
	a[6] = entries;
	a[7] = chid;
	a[8] = 0xcaf00005u;
	a[9] = 0xcaf00003u;
	a[10] = 0x40000001u;
	a[11] = 0x2000;
	a[12] = 0xfe001000ull;
	a[13] = 0xfe102200ull;
	a[14] = 0;
	a[15] = 0xfe200000ull;
	a[16] = 0x1800;
	a[17] = 0xfe300000ull;
	a[18] = 4096;
	a[19] = 1;
	a[20] = 777;
	a[21] = 1;
}

static void md(NV_MEMORY_DESC_PARAMS *m, uint64_t base, uint64_t size, uint32_t cache)
{
	m->base = base;
	m->size = size;
	m->addressSpace = 1;	/* ADDR_SYSMEM, g_mem_desc_nvoc.h:115 */
	m->cacheAttrib = cache;	/* NV_MEMORY_CACHED 0 / UNCACHED 1, nv_memory_type.h:30-31 */
}

static void test_channel(void)
{
	static const uint32_t chids[] = { 0, 7, 8, 9, 511, 4095 };
	NV_CHANNEL_ALLOC_PARAMS p;
	uint32_t n;

	for (unsigned k = 0; k < sizeof(chids) / sizeof(chids[0]); k++) {
		uint32_t c = chids[k];

		chan_inputs(c, 1024);
		eq("channel", build(9), 0);
		memset(&p, 0, sizeof(p));
		p.hObjectError = a[4];
		p.hObjectBuffer = 0;
		p.gpFifoOffset = a[5];
		p.gpFifoEntries = a[6];
		p.flags = DRF_DEF(OS04, _FLAGS, _CHANNEL_USERD_INDEX_FIXED, _FALSE) |
			  DRF_DEF(OS04, _FLAGS, _CHANNEL_USERD_INDEX_PAGE_FIXED, _TRUE) |
			  DRF_NUM(OS04, _FLAGS, _CHANNEL_USERD_INDEX_VALUE, c % 8) |
			  DRF_NUM(OS04, _FLAGS, _CHANNEL_USERD_INDEX_PAGE_VALUE, c / 8);
		p.hContextShare = a[8];
		p.hVASpace = a[9];
		p.hUserdMemory[0] = a[10];
		p.userdOffset[0] = a[11];
		md(&p.instanceMem, a[12], 4096, 0);
		md(&p.userdMem, a[13], 512, a[14]);
		md(&p.ramfcMem, a[12], 512, 0);
		md(&p.mthdbufMem, a[15], a[16], 0);
		/* PRIVILEGE 1:0, ERROR_NOTIFIER_TYPE 3:2 = MEMORY 3, ECC_ERROR_NOTIFIER_TYPE 5:4 = NONE 1
		 * (g_kernel_channel_nvoc.h:181-194, 108-118) */
		p.internalFlags = (uint32_t)a[21] | 3u << 2 | 1u << 4;
		md(&p.errorNotifierMem, a[17], a[18], a[19]);
		p.ProcessID = a[20];
		n = alloc_hdr(a[0], a[1], a[2], a[3], &p, sizeof(p));
		same("channel", n, 103, 32, n);
	}
	chan_inputs(4096, 1024);
	eq("chid 4096", build(9), FAIL + 6);
	chan_inputs(5, 1000);
	eq("entries 1000", build(9), FAIL + 5);
	chan_inputs(5, 4);
	eq("entries 4", build(9), FAIL + 5);
	chan_inputs(5, 131072);
	eq("entries 131072", build(9), FAIL + 5);
	chan_inputs(5, 65536);
	eq("entries 65536", build(9), 0);
	chan_inputs(5, 8);
	a[5] += 4;
	eq("ring unaligned", build(9), FAIL + 4);
	chan_inputs(5, 8);
	a[11] = 0x100;
	eq("userd unaligned", build(9), FAIL + 4);
	chan_inputs(5, 8);
	a[12] += 512;
	eq("inst unaligned", build(9), FAIL + 4);
	chan_inputs(5, 8);
	a[14] = 2;
	eq("userd cache attr", build(9), FAIL + 7);
	chan_inputs(5, 8);
	a[18] = 8;
	eq("err notifier small", build(9), FAIL + 4);
	chan_inputs(5, 8);
	a[8] = 0;
	eq("ctxshare 0", build(9), FAIL + 2);
	chan_inputs(5, 8);
	a[21] = 3;
	eq("privilege 3", build(9), FAIL + 3);
	chan_inputs(5, 8);
	eq("channel via rm.om", call3(agpu_om_rm, 9, msg, a), FAIL + 1);
	eq("rmchan other op", call3(agpu_om_rmchan, 3, msg, a), FAIL + 1);
}

static void test_controls(void)
{
	const uint32_t C = 0xc1d80001u, S = 0xcaf00002u, T = 0xcaf00004u, H = 0xcaf00006u;
	uint8_t z[3712] = { 0 };
	uint32_t n;

	/* generic control with a first word */
	set4(C, S, NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE, sizeof(NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS));
	a[4] = 64;
	eq("devinfo ctl", build(11), 0);
	NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS di;
	memset(&di, 0, sizeof(di));
	di.baseIndex = 64;
	n = ctl_hdr(C, S, NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE, &di, sizeof(di));
	same("devinfo ctl", n, 76, n, n);
	set4(C, S, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_INFO, sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS));
	eq("grinfo ctl", build(11), 0);
	n = ctl_hdr(C, S, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_INFO, z, sizeof(NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS));
	same("grinfo ctl", n, 76, n, n);
	set4(C, S, 1, 65377);
	eq("ctl too big", build(11), FAIL + 10);

	/* PROMOTE_FAULT_METHOD_BUFFERS (kernel_channel_group_api.c:450-489) */
	NVA06C_CTRL_INTERNAL_PROMOTE_FAULT_METHOD_BUFFERS_PARAMS fm;
	memset(&fm, 0, sizeof(fm));
	for (int i = 0; i < 2; i++) {
		fm.methodBufferMemdesc[i].base = 0xfe400000ull + 0x10000ull * i;
		fm.methodBufferMemdesc[i].size = 0x1800;
		fm.methodBufferMemdesc[i].alignment = 1;
		fm.methodBufferMemdesc[i].addressSpace = 1;
		fm.methodBufferMemdesc[i].cpuCacheAttrib = 0;
	}
	fm.numValidEntries = 2;
	set4(C, T, 0xfe400000ull, 0x1800);
	a[4] = 0xfe410000ull;
	a[5] = 0x1800;
	eq("fault mthd", build(12), 0);
	n = ctl_hdr(C, T, NVA06C_CTRL_CMD_INTERNAL_PROMOTE_FAULT_METHOD_BUFFERS, &fm, sizeof(fm));
	same("fault mthd", n, 76, n, n);
	a[5] = 0;
	eq("fault mthd size 0", build(12), FAIL + 4);

	/* PROMOTE_CTX (kgrobjPromoteContext, kgrctxPrepareInitializeCtxBuffer/PromoteCtxBuffer) */
	NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS pc;
	memset(&pc, 0, sizeof(pc));
	pc.engineType = NV2080_ENGINE_TYPE_GR(0);
	pc.hChanClient = C;
	pc.hObject = H;
	pc.entryCount = 4;
	const uint64_t ent[4][6] = {
		{ 0xfe500000ull, 0x1000000000000ull, 0x210000, 0, NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_MAIN, 1 },
		{ 0xfe800000ull, 0x1000000400000ull, 0x8000, 0, NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_PATCH, 1 },
		{ 0xfe900000ull, 0x1000000600000ull, 0x2000, 1, NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_PRIV_ACCESS_MAP, 1 },
		{ 0xfea00000ull, 0, 0x2000, 0, NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_UNRESTRICTED_PRIV_ACCESS_MAP, 3 },
	};
	set4(C, S, H, 4);
	for (int i = 0; i < 4; i++) {
		NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ENTRY *e = &pc.promoteEntry[i];

		e->gpuPhysAddr = ent[i][0];
		e->gpuVirtAddr = ent[i][1];
		e->size = ent[i][2];
		e->physAttr = (ent[i][3] ? DRF_DEF(2080, _CTRL_GPU_INITIALIZE_CTX, _APERTURE, _NCOH_SYS) :
				DRF_DEF(2080, _CTRL_GPU_INITIALIZE_CTX, _APERTURE, _COH_SYS)) |
			      DRF_DEF(2080, _CTRL_GPU_INITIALIZE_CTX, _GPU_CACHEABLE, _NO);
		e->bufferId = ent[i][4];
		e->bInitialize = ent[i][5] & 1;
		e->bNonmapped = ent[i][5] >> 1;
		for (int k = 0; k < 6; k++)
			a[8 + 6 * i + k] = ent[i][k];
	}
	eq("promote", build(13), 0);
	n = ctl_hdr(C, S, NV2080_CTRL_CMD_GPU_PROMOTE_CTX, &pc, sizeof(pc));
	same("promote", n, 76, n, n);
	a[3] = 7;
	eq("promote 7 entries", build(13), FAIL + 8);
	a[3] = 4;
	a[8 + 4] = 13;
	eq("promote buffer id 13", build(13), FAIL + 8);
	a[8 + 4] = 0;
	a[8] += 8;
	eq("promote pa unaligned", build(13), FAIL + 4);

	/* COPY_SERVER_RESERVED_PDES: params as kmod/aien_gpu_mem.c leaves them at 120 */
	NV90F1_CTRL_VASPACE_COPY_SERVER_RESERVED_PDES_PARAMS rp;
	memset(&rp, 0, sizeof(rp));
	rp.pageSize = 0x200000;
	rp.virtAddrLo = 0x100000000ull;
	rp.virtAddrHi = 0x11fffffffull;
	rp.numLevelsToCopy = 5;
	for (int i = 0; i < 5; i++) {
		rp.levels[i].physAddress = 0xfeb00000ull + 0x1000ull * i;
		rp.levels[i].size = i ? 4096 : 16;
		rp.levels[i].aperture = 4;
		rp.levels[i].pageShift = 56 - 9 * i;
	}
	memset(msg, 0xa5, sizeof(msg));
	memcpy(msg + 120, &rp, sizeof(rp));
	set4(C, 0xcaf00003u, S, 0);
	eq("rpdes", call3(agpu_om_rmctl, 14, msg, a), 0);
	rp.hSubDevice = S;
	rp.subDeviceId = 0;
	n = ctl_hdr(C, 0xcaf00003u, NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES, &rp, sizeof(rp));
	same("rpdes", n, 76, n, n);
	rp.virtAddrLo = 0;
	memcpy(msg + 120, &rp, sizeof(rp));
	eq("rpdes range", call3(agpu_om_rmctl, 14, msg, a), FAIL + 9);
	rp.virtAddrLo = 0x100000000ull;
	rp.levels[2].physAddress += 8;
	memcpy(msg + 120, &rp, sizeof(rp));
	eq("rpdes level unaligned", call3(agpu_om_rmctl, 14, msg, a), FAIL + 9);

	/* GPFIFO_SCHEDULE, FREE, UNLOADING_GUEST_DRIVER */
	NVA06C_CTRL_GPFIFO_SCHEDULE_PARAMS sc = { .bEnable = NV_TRUE };
	set4(C, T, 0, 0);
	eq("schedule", build(15), 0);
	n = ctl_hdr(C, T, NVA06C_CTRL_CMD_GPFIFO_SCHEDULE, &sc, sizeof(sc));
	same("schedule", n, 76, n, n);
	NVOS00_PARAMETERS fr;
	memset(&fr, 0, sizeof(fr));
	fr.hRoot = C;
	fr.hObjectParent = 0;
	fr.hObjectOld = T;
	eq("free", build(16), 0);
	memcpy(want, &fr, 16);
	same("free", 16, 10, 16, 16);
	set4(0, 0, 0, 0);
	eq("unload", build(17), 0);
	memset(want, 0, 8);
	same("unload", 8, 47, 8, 8);
	eq("free hClient 0", build(16), FAIL + 2);
	set4(C, T, 0, 0);
	eq("ctl unknown op", build(18), FAIL + 1);
}

/* the same message through the oracle and through om build + seal + mqput */
static uint8_t cq_c[GSP_MQ_QUEUE_SIZE], cq_o[GSP_MQ_QUEUE_SIZE], sq[GSP_MQ_QUEUE_SIZE];

static void init_queue(uint8_t *q)
{
	const uint32_t h[8] = { 0, GSP_MQ_QUEUE_SIZE, GSP_MQ_MSG_SIZE, GSP_MQ_MSG_COUNT, 0, GSP_MQ_FLAGS_SWAP_RX,
				GSP_MQ_RXHDR_OFF, GSP_MQ_ENTRY_OFF };
	memset(q, 0, GSP_MQ_QUEUE_SIZE);
	for (int i = 0; i < 8; i++)
		w32(q + 4 * i, h[i]);
}

static void om_put(uint32_t seq, uint32_t txseq)
{
	uint64_t st = 0, elems, wr;

	elems = om_call(agpu_om_seal, P(msg), a[48], a[49], a[50], seq, txseq, &st);
	if (st || elems >= FAIL)
		die("seal", elems, 0);
	wr = om_call(agpu_om_mqput, P(cq_o), P(msg), elems, r32(sq + GSP_MQ_RXHDR_OFF), 0, 0, &st);
	if (st || wr >= FAIL)
		die("mqput", wr, 0);
	w32(cq_o + 16, (uint32_t)wr);
}

static void test_framing(void)
{
	gsp_mq q = { .cmdq = cq_c, .statq = sq };
	const char *why = 0;
	uint32_t seq, txseq = 0;
	NV_CHANNEL_ALLOC_PARAMS cp;

	init_queue(cq_c);
	init_queue(cq_o);
	init_queue(sq);
	for (int round = 0; round < 40; round++) {
		/* alloc (params outside the length), channel, control, free, static info */
		set4(0xc1d80001u, 0xcaf00001u, 0xcaf00002u + round, 0);
		build(5);
		if (gsp_rpc_rm_alloc(&q, a[0], a[1], a[2], NV20_SUBDEVICE_0, msg + 112, 4, &seq, &why))
			die(why, 0, 0);
		om_put(seq, txseq++);
		chan_inputs(round, 512);
		build(9);
		memcpy(&cp, msg + 112, sizeof(cp));
		if (gsp_rpc_rm_alloc(&q, a[0], a[1], a[2], a[3], &cp, sizeof(cp), &seq, &why))
			die(why, 0, 0);
		om_put(seq, txseq++);
		set4(0xc1d80001u, 0xcaf00002u, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO, 1664);
		build(11);
		if (gsp_rpc_rm_control(&q, a[0], a[1], a[2], msg + 120, 1664, &seq, &why))
			die(why, 0, 0);
		om_put(seq, txseq++);
		set4(0xc1d80001u, 0xcaf00002u + round, 0, 0);
		build(16);
		if (gsp_rpc_rm_free(&q, a[0], a[1], &seq, &why))
			die(why, 0, 0);
		om_put(seq, txseq++);
		build(2);
		if (gsp_rpc_get_static_info(&q, 1792, &seq, &why))
			die(why, 0, 0);
		om_put(seq, txseq++);
		w32(sq + GSP_MQ_RXHDR_OFF, r32(cq_c + 16));	/* GSP drained */
		checks++;
		if (memcmp(cq_c, cq_o, sizeof(cq_c)))
			die("framing: command queue differs from the oracle, round", round, 0);
	}
}

/* ---- replies ---- */

static void reply(uint32_t fn, uint32_t seq, uint32_t result, uint32_t paylen)
{
	memset(msg, 0, sizeof(msg));
	w32(msg + 48, 0x03000000u);
	w32(msg + 52, 0x43505256u);
	w32(msg + 56, 32 + paylen);
	w32(msg + 60, fn);
	w32(msg + 64, result);
	w32(msg + 72, seq);
}

static uint64_t chk(uint64_t op)
{
	return call3(agpu_om_rmchk, op, msg, a);
}

static void test_replies(void)
{
	/* op 1: the reply of our RPC, another message, rpc_result, status fields */
	reply(103, 7, 0, 32);
	set4(103, 7, 32, 0);
	eq("reply ok", chk(1), 0);
	set4(103, 8, 32, 0);
	eq("reply other seq", chk(1), 1);
	set4(76, 7, 32, 0);
	eq("reply other fn", chk(1), 1);
	eq("reply event fn out", a[8], 103);
	set4(103, 7, 40, 0);
	eq("reply short", chk(1), FAIL + 1);
	reply(103, 7, 0x57, 32);
	set4(103, 7, 32, 0);
	eq("reply rpc_result", chk(1), 2);
	eq("reply rpc_result out", a[9], 0x57);
	reply(103, 7, 0, 32);
	w32(msg + 80 + 16, 0x1f);
	set4(103, 7, 32, 0);
	eq("alloc status", chk(1), 3);
	eq("alloc status out", a[11], 0x1f);
	reply(76, 9, 0, 44);
	w32(msg + 80 + 12, 0x22);
	set4(76, 9, 44, 0);
	eq("control status", chk(1), 3);
	reply(10, 9, 0, 16);
	w32(msg + 80 + 12, 0x0b);
	set4(10, 9, 16, 0);
	eq("free status", chk(1), 3);
	reply(0x1002, 0, 0, 64);
	set4(76, 0, 0, 0);
	eq("sequencer event", chk(1), 1);
	eq("sequencer fn", a[8], 0x1002);

	/* op 2: version handshake (vgpu_version.h:33-34) */
	reply(1, 0, 0, 792);
	w32(msg + 80, 0x2B);
	w32(msg + 84, 0x13);
	eq("version ok", chk(2), 0);
	w32(msg + 84, 0x14);
	eq("version minor", chk(2), FAIL + 2);
	eq("version minor out", a[1], 0x14);
	w32(msg + 84, 0x13);
	w32(msg + 68, 0x55);
	eq("version private", chk(2), FAIL + 2);

	/* op 3: GspStaticConfigInfo handles at 1728/1732/1736 */
	reply(65, 1, 0, 1792);
	w32(msg + 80 + 1728, 0xc1e00002u);
	w32(msg + 80 + 1732, 0x5c000001u);
	w32(msg + 80 + 1736, 0x5c000002u);
	eq("static ok", chk(3), 0);
	eq("static client", a[0], 0xc1e00002u);
	eq("static subdevice", a[2], 0x5c000002u);
	w32(msg + 80 + 1732, 0);
	eq("static zero", chk(3), FAIL + 4);
	w32(msg + 80 + 1732, 0x5c000002u);
	eq("static equal", chk(3), FAIL + 5);
	w32(msg + 56, 32 + 1791);
	eq("static short", chk(3), FAIL + 3);

	/* op 4: device-info table (ENGINE_INFO_TYPE_* indices, engine_info.h:37-99) */
	NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS di;
	memset(&di, 0, sizeof(di));
	di.baseIndex = 32;
	di.numEntries = 9;
	di.bMore = NV_TRUE;
	di.entries[3].engineData[2] = 1;	/* GR0, but not host-driven: skipped */
	di.entries[3].engineData[3] = 1;
	di.entries[6].engineData[2] = 1;	/* RM_ENGINE_TYPE_GR0 */
	di.entries[6].engineData[3] = 12;	/* RUNLIST */
	di.entries[6].engineData[11] = 0x8000;	/* RUNLIST_PRI_BASE */
	di.entries[6].engineData[12] = 1;	/* IS_HOST_DRIVEN_ENGINE */
	di.entries[6].engineData[14] = 0x9000;	/* CHRAM_PRI_BASE */
	reply(76, 3, 0, 40 + sizeof(di));
	memcpy(msg + 120, &di, sizeof(di));
	set4(0, 0, 0, 0);
	a[6] = 32;
	eq("devinfo", chk(4), 0);
	eq("devinfo found", a[0], 1);
	eq("devinfo runlist", a[1], 12);
	eq("devinfo more", a[2], 1);
	eq("devinfo chram", a[4], 0x9000);
	eq("devinfo runlist base", a[5], 0x8000);
	di.entries[6].engineData[12] = 0;
	memcpy(msg + 120, &di, sizeof(di));
	a[6] = 32;
	eq("devinfo none", chk(4), 0);
	eq("devinfo none found", a[0], 0);
	di.entries[6].engineData[12] = 1;
	di.entries[6].engineData[3] = 64;
	memcpy(msg + 120, &di, sizeof(di));
	a[6] = 32;
	eq("devinfo runlist 64", chk(4), FAIL + 7);
	di.entries[6].engineData[3] = 12;
	di.numEntries = 33;
	memcpy(msg + 120, &di, sizeof(di));
	a[6] = 32;
	eq("devinfo 33", chk(4), FAIL + 6);
	di.numEntries = 9;
	memcpy(msg + 120, &di, sizeof(di));
	a[6] = 0;
	eq("devinfo base", chk(4), FAIL + 6);

	/* op 5, 6 */
	NV2080_CTRL_INTERNAL_FIFO_GET_NUM_CHANNELS_PARAMS nc = { .runlistId = 12, .numChannels = 2048 };
	reply(76, 3, 0, 48);
	memcpy(msg + 120, &nc, sizeof(nc));
	set4(12, 0, 0, 0);
	eq("nchan", chk(5), 0);
	eq("nchan out", a[1], 2048);
	nc.numChannels = 4097;
	memcpy(msg + 120, &nc, sizeof(nc));
	eq("nchan 4097", chk(5), FAIL + 8);
	nc.numChannels = 64;
	memcpy(msg + 120, &nc, sizeof(nc));
	set4(11, 0, 0, 0);
	eq("nchan runlist", chk(5), FAIL + 8);
	NV2080_CTRL_CE_GET_FAULT_METHOD_BUFFER_SIZE_PARAMS ce = { .size = 0x1800 };
	memcpy(msg + 120, &ce, sizeof(ce));
	eq("ce size", chk(6), 0);
	eq("ce size out", a[0], 0x1800);
	ce.size = 0;
	memcpy(msg + 120, &ce, sizeof(ce));
	eq("ce size 0", chk(6), FAIL + 9);

	/* op 8, 7: GR info and context-buffer sizes (kgraphicsGetMainCtxBufferSize) */
	static NV2080_CTRL_INTERNAL_STATIC_KGR_GET_INFO_PARAMS gi;
	memset(&gi, 0, sizeof(gi));
	gi.engineInfo[0].infoList[NV0080_CTRL_GR_INFO_INDEX_MAX_SUBCONTEXT_COUNT].data = 64;
	reply(76, 3, 0, 40 + sizeof(gi));
	memcpy(msg + 120, &gi, sizeof(gi));
	eq("grinfo", chk(8), 0);
	eq("grinfo out", a[0], 64);
	gi.engineInfo[0].infoList[NV0080_CTRL_GR_INFO_INDEX_MAX_SUBCONTEXT_COUNT].data = 0;
	memcpy(msg + 120, &gi, sizeof(gi));
	eq("grinfo 0", chk(8), FAIL + 11);
	static NV2080_CTRL_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_PARAMS cb;
	NV2080_CTRL_INTERNAL_ENGINE_CONTEXT_BUFFER_INFO *e = cb.engineContextBuffersInfo[0].engine;
	memset(&cb, 0, sizeof(cb));
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS].size = 0x1ff123;
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS].alignment = 0x1000;
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS_PATCH].size = 0x9000;
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS_PATCH].alignment = 0x1000;
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS_PRIV_ACCESS_MAP].size = 0x2000;
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS_PRIV_ACCESS_MAP].alignment = 0x10000;
	reply(76, 3, 0, 40 + sizeof(cb));
	memcpy(msg + 120, &cb, sizeof(cb));
	memset(a, 0, sizeof(a));
	a[10] = 64;
	eq("ctxinfo", chk(7), 0);
	eq("ctxinfo main", a[0], ((0x1ff123u + 4095u) & ~4095u) + 4096u * 64);
	eq("ctxinfo patch", a[2], 0x9000);
	eq("ctxinfo pam", a[4], 0x2000);
	eq("ctxinfo pam align", a[5], 0x10000);
	a[10] = 65;
	eq("ctxinfo subctx 65", chk(7), FAIL + 11);
	a[10] = 1;
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS].alignment = 0x3000;
	memcpy(msg + 120, &cb, sizeof(cb));
	eq("ctxinfo align", chk(7), FAIL + 10);
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS].alignment = 0;
	e[NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_ENGINE_ID_GRAPHICS_PATCH].size = 0;
	memcpy(msg + 120, &cb, sizeof(cb));
	eq("ctxinfo patch 0", chk(7), FAIL + 10);

	/* op 9: class list (read at run time; the source's GB10B table and the 580 runtime differ) */
	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS cl;
	static const uint32_t sets[4][5] = {
		{ BLACKWELL_COMPUTE_A, BLACKWELL_CHANNEL_GPFIFO_A, 0xc661, 0, 0 },
		{ BLACKWELL_COMPUTE_B, BLACKWELL_COMPUTE_A, BLACKWELL_CHANNEL_GPFIFO_B, BLACKWELL_CHANNEL_GPFIFO_A, 0 },
		{ BLACKWELL_COMPUTE_A, BLACKWELL_COMPUTE_B, BLACKWELL_CHANNEL_GPFIFO_A, BLACKWELL_CHANNEL_GPFIFO_B, 0 },
		{ BLACKWELL_COMPUTE_A, 0xc661, 0x90f1, 0, 0 },
	};
	static const uint64_t outs[4][2] = {
		{ BLACKWELL_COMPUTE_A, BLACKWELL_CHANNEL_GPFIFO_A },
		{ BLACKWELL_COMPUTE_B, BLACKWELL_CHANNEL_GPFIFO_B },
		{ BLACKWELL_COMPUTE_B, BLACKWELL_CHANNEL_GPFIFO_B },
		{ 0, 0 },
	};
	for (int s = 0; s < 4; s++) {
		memset(&cl, 0, sizeof(cl));
		for (int i = 0; i < 5 && sets[s][i]; i++)
			cl.classList[cl.numClasses++] = sets[s][i];
		reply(76, 3, 0, 40 + sizeof(cl));
		memcpy(msg + 120, &cl, sizeof(cl));
		eq("classes", chk(9), s == 3 ? FAIL + 12 : 0);
		if (s < 3) {
			eq("classes compute", a[0], outs[s][0]);
			eq("classes gpfifo", a[1], outs[s][1]);
		}
	}
	cl.numClasses = 201;
	memcpy(msg + 120, &cl, sizeof(cl));
	eq("classes 201", chk(9), FAIL + 6);
	eq("check unknown op", chk(10), FAIL + 13);
}

/* ---- om/rmarg.om ---- */
static uint64_t bm[64];

static uint64_t arg(uint64_t op)
{
	uint64_t st = 0, r = om_call(agpu_om_rmarg, op, P(bm), P(a), 0, 0, 0, &st);

	if (st)
		die("rmarg trap", st, 0);
	return r;
}

static void test_args(void)
{
	static uint8_t model[4096];
	uint32_t rng = 99;

	memset(bm, 0, sizeof(bm));
	set4(0, 0, 0, 0);
	eq("chid n 0", arg(1), FAIL + 1);
	a[0] = 4097;
	eq("chid n 4097", arg(1), FAIL + 1);
	a[0] = 3;
	eq("chid 0", arg(1), 0);
	eq("chid 1", arg(1), 1);
	eq("chid 2", arg(1), 2);
	eq("chid full", arg(1), FAIL + 2);
	a[1] = 1;
	eq("chid release 1", arg(2), 0);
	eq("chid release 1 again", arg(2), FAIL + 3);
	a[1] = 3;
	eq("chid release 3 >= n", arg(2), FAIL + 3);
	eq("chid reuse 1", arg(1), 1);
	/* random allocate/release against a byte model, lowest-free order */
	memset(bm, 0, sizeof(bm));
	memset(model, 0, sizeof(model));
	for (int t = 0; t < 20000; t++) {
		rng = rng * 1103515245u + 12345u;
		uint32_t c = (rng >> 8) % 4096;
		a[0] = 4096;
		if ((rng >> 4) & 1) {
			uint32_t w = 0;

			while (w < 4096 && model[w])
				w++;
			uint64_t r = arg(1);
			if (w == 4096) {
				eq("chid model full", r, FAIL + 2);
			} else {
				eq("chid model", r, w);
				model[w] = 1;
			}
		} else {
			a[1] = c;
			eq("chid model release", arg(2), model[c] ? 0 : FAIL + 3);
			model[c] = 0;
		}
	}
	/* AGPU_CHAN_CREATE arguments */
	set4(1024, 0x10000, 0x2000, 1);
	a[4] = 2;
	a[5] = 3;
	eq("args ok", arg(3), 0);
	a[0] = 1000;
	eq("args entries", arg(3), FAIL + 4);
	a[0] = 1024;
	a[1] = 0x10004;
	eq("args ring", arg(3), FAIL + 5);
	a[1] = 1ull << 48;
	eq("args ring 2^48", arg(3), FAIL + 5);
	a[1] = 0x10000;
	a[2] = 0x2100;
	eq("args userd", arg(3), FAIL + 6);
	a[2] = 0x2000;
	a[5] = 0;
	eq("args handle 0", arg(3), FAIL + 7);
	a[5] = 1ull << 32;
	eq("args handle 33 bits", arg(3), FAIL + 7);
	/* token: DOORBELL RUNLIST_ID 22:16, VECTOR 11:0 (gb100/dev_vm.h:620-625) */
	static const uint32_t tok[][2] = { { 0, 0 }, { 12, 5 }, { 63, 4095 }, { 1, 2048 } };
	for (unsigned i = 0; i < sizeof(tok) / sizeof(tok[0]); i++) {
		set4(tok[i][0], tok[i][1], 0, 0);
		eq("token", arg(4), DRF_NUM(_VIRTUAL, _FUNCTION_DOORBELL, _RUNLIST_ID, tok[i][0]) |
				    DRF_NUM(_VIRTUAL, _FUNCTION_DOORBELL, _VECTOR, tok[i][1]));
	}
	set4(64, 0, 0, 0);
	eq("token runlist 64", arg(4), FAIL + 8);
	set4(0, 4096, 0, 0);
	eq("token chid 4096", arg(4), FAIL + 8);
	/* unload waits */
	set4(0x80000000u, 0, 0, 0);
	eq("suspended", arg(5), 1);
	set4(0x80000001u, 0, 0, 0);
	eq("not suspended", arg(5), 0);
	set4(0x10, 0, 0, 0);
	eq("halted", arg(6), 1);
	set4(0x80, 0, 0, 0);
	eq("not halted", arg(6), 0);
	eq("arg unknown op", arg(7), FAIL + 9);
}

int main(void)
{
	test_alloc();
	test_channel();
	test_controls();
	test_framing();
	test_replies();
	test_args();
	printf("PARITY-RM PASS checks=%lu\n", checks);
	return 0;
}
