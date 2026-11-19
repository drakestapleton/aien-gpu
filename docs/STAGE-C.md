# Stage C: GSP INIT_DONE to a runnable compute channel (GSP-RM 580.173.02)

Chip: the Spark's GPU is GB20B (docs/STATUS.md, 2026-11-18). This document was written for GB10B.
A HAL audit of every `generated/g_*_nvoc.c` at this tag found one difference from GB10B in what stage C
ports: the work-submit token (step 11 below, corrected). Every other `_GB10B`-named function cited here
is one GB20B also takes, or is cited as GB10B's for contrast. Nothing in stage C has run on the chip.

Status: IMPLEMENTED (kmod/aien_gpu_rm.c; om/rm.om, om/rmchan.om, om/rmctl.om, om/rmchk.om,
om/rmarg.om; tools/aien-gpu-m16.c). Host-tested (tests/host/parity_rm.c, mutants). Hardware NOT_RUN.

Paths are relative to `open-gpu-kernel-modules-580.173.02/src/nvidia/` unless they start with
`common/` (= `src/common/`). Every claim is a READ of source; nothing here has run on the chip.
Struct offsets were compiled from the real headers (aarch64 LP64, same as GSP-RM's RISC-V LP64);
tests/host/parity_rm.c re-checks every payload byte against those headers on each build.

## 0. Procedure, in the order the module runs it

### AGPU_RM_INIT (state GSP_UP -> RM_READY), kmod agpu_rm_init

| Step | RPC (fn) | Payload / reply | Source |
|---|---|---|---|
| 1.2 | SET_GUEST_SYSTEM_INFO (1), sync | 792 B: vgx 0x2B/0x13, three 256-B strings (length 256 each), guestClNum 38369854. Reply must keep 0x2B/0x13 and rpc_result_private 0, else AGPU_RM_E_VERSION (we do not retry with GSP's version) | rpc.c:8653-8834; vgpu_version.h:33-34; nvBldVer.h:46-47 |
| 1.4 | GET_GSP_STATIC_INFO (65) | zeroed 1792 B; reply = GspStaticConfigInfo; copied to agpu_rm_init.static_info | rpc.c:9661-9699 |
| 1.6 | (no RPC) | hInternalClient@1728, Device@1732, Subdevice@1736 nonzero and distinct, else AGPU_RM_E_STATIC | gpu.c:965-972 |
| 1.7a | CONTROL 0x20800a2a KGR_GET_INFO (3712 B) on (hIntClient, hIntSubdevice) | GR0 infoList[0x2C MAX_SUBCONTEXT_COUNT] 1..64 | kernel_graphics.c:1121-1511, 1765 |
| 1.7b | CONTROL 0x20800a32 KGR_GET_CONTEXT_BUFFERS_INFO (1664 B = 8 engines x 26 x {size, alignment}) | GR0 entries: [0] GRAPHICS main, [16] PATCH, [24] PRIV_ACCESS_MAP. main bytes = align4k(size) + 4096 x maxSubctx | ctrl2080internal.h:510-533; ctrl0080fifo.h:121-147; kernel_graphics.c:625-749, 1743-1776 |
| 1.8a | CONTROL 0x20801112 FIFO_GET_DEVICE_INFO_TABLE (3212 B), baseIndex 0, 32, ... while bMore | GR0 = entry with engineData[2 RM_ENGINE_TYPE] == 1 (GR0) and engineData[12 IS_HOST_DRIVEN] != 0; runlist = engineData[3]; also [11] RUNLIST_PRI_BASE, [14] CHRAM_PRI_BASE | kernel_fifo.c:2006-2105; engine_info.h:37-99; ctrl2080fifo.h:618-659 |
| 1.8b | CONTROL 0x20800a61 INTERNAL_FIFO_GET_NUM_CHANNELS {runlistId, numChannels} | 1..4096; bounds the ChID allocator (none before it is read) | kernel_fifo.c:1317-1340; ctrl2080internal.h:1568-1583 |
| 1.9 | CONTROL 0x20802a08 CE_GET_FAULT_METHOD_BUFFER_SIZE {size} | 1..16 MiB | kernel_ce.c:827-848; ctrl2080ce.h:281-287 |
| 1.10 | CONTROL 0x800292 GPU_GET_CLASSLIST_V2 on (hIntClient, hIntDevice) {numClasses, classList[200]} | compute: 0xcec0 COMPUTE_B else 0xcdc0 COMPUTE_A; GPFIFO: 0xca6f GPFIFO_B else 0xc96f GPFIFO_A; none -> AGPU_RM_E_UNSUPPORTED | ctrl0080gpu.h:504-513; g_device_nvoc.c:333 (flags 0x1010b) |

Classlist note: generated/g_gpu_class_list.c:2418-2486 lists BLACKWELL_COMPUTE_A and GPFIFO_A for
GB10B, while the live 35B's nvidia.ko log (serve.log:13747, read by OwnDriver) shows RM_ALLOC of
0xcec0 accepted on this GB10. So the class is read at run time, never assumed. Whether GSP-RM (not
CPU-RM) answers GET_CLASSLIST_V2 for the internal device is UNKNOWN 9.

### Per file, first AGPU_MEM_ALLOC or AGPU_CHAN_CREATE: agpu_rm_fctx_get

Client handle 0xc1d80000 + n (RS_CLIENT_HANDLE_BASE 0xc1d00000 range, resserv.h:135-152; GSP
re-encodes a client handle into that base, rs_server.c:3255-3260); object handles 0xcaf00000 + n.

1. agpu_vas_create (kmod/aien_gpu_mem.c): the page-table tree.
2. GSP_RM_ALLOC NV01_ROOT (hClient, 0, hClient): NV0000_ALLOC_PARAMETERS {hClient, processID}.
3. GSP_RM_ALLOC NV01_DEVICE_0 (0x80): {deviceId 0, hClientShare = hClient, vaMode 0}.
4. GSP_RM_ALLOC NV20_SUBDEVICE_0 (0x2080): {subDeviceId 0}.
5. GSP_RM_ALLOC FERMI_VASPACE_A (0x90f1): index 0 GPU_NEW, flags 0 (RM-owned), vaBase 0,
   vaSize 2^49 (vaspace_api.c:350-385; covers the kernel-buffer range [2^48, 2^48 + 2^40)).
6. GSP_RM_CONTROL 0x90f10106 COPY_SERVER_RESERVED_PDES on hVas: the 184 bytes from
   agpu_vas_reserved_pdes, hSubDevice = our subdevice, checked by om/rmctl.om (2 MiB pages,
   [4 GiB, 4.5 GiB - 1], 1..6 levels, 4 KiB-aligned PAs). Must precede any channel
   (gpu_vaspace.c:4486-4530: GSP swaps its walker levels, root included, for ours).

### AGPU_CHAN_CREATE: agpu_chan_create

Arguments (om/rmarg.om op 3): entries power of two 8..65536, ring_off 8-aligned, userd_off
512-aligned, both below 2^48, handles nonzero 32-bit.

1. Device addresses of ring, USERD (512 B), error notifier (first 4 KiB) via agpu_mem_iova
   (contiguous slices only).
2. ChID: lowest free below numChannels (om/rmarg.om op 1; CPU-RM allocates ChIDs,
   kchannelAllocHwID_GM107, kernel_channel_gm107.c:420).
3. Kernel buffers (agpu_kbuf_alloc, zeroed dma-coherent): instance block 4096 (4 KiB aligned,
   dev_ram.h:36-37), 2 fault-method buffers of the CE size (kernel_channel_group_gv100.c:44-110,
   runqueues = 2), GR main (size from 1.7b), GR patch, and once per device the PRIV_ACCESS_MAP and
   UNRESTRICTED_PRIV_ACCESS_MAP (kgraphics_gm200.c:247-296). An IOVA not meeting GSP's reported
   alignment is refused (AGPU_RM_E_UNSUPPORTED code 2), never re-placed by guess.
4. Map main and patch (and once per file the access map) into the file's VAS (agpu_vas_map_kbuf).
5. GSP_RM_ALLOC KEPLER_CHANNEL_GROUP_A (0xa06c): {hVASpace, engineType 1 GRAPHICS}.
6. CONTROL 0xa06c010a INTERNAL_PROMOTE_FAULT_METHOD_BUFFERS on the TSG, 88 B: 2 x
   NV2080_CTRL_INTERNAL_MEMDESC_INFO {base = IOVA, size, alignment 1, addressSpace 1 SYSMEM,
   cpuCacheAttrib 0 CACHED}, bar2Addr 0, numValidEntries 2 (kernel_channel_group_api.c:450-504;
   ctrla06c.h:329-363; ctrl2080internal.h:388-394).
7. GSP_RM_ALLOC FERMI_CONTEXT_SHARE_A (0x9067): {hVASpace, flags 1 SUBCONTEXT_ASYNC}.
8. GSP_RM_ALLOC <GPFIFO class> under the TSG, NV_CHANNEL_ALLOC_PARAMS 368 B as
   _kchannelSendChannelAllocRpc fills it for a GSP client (kernel_channel.c:562-610, 2665-2846):
   hObjectError = err handle, hObjectBuffer 0, gpFifoOffset = ring GPU VA, entries, flags =
   USERD_INDEX_VALUE ChID%8 (10:8) | USERD_INDEX_PAGE_VALUE ChID/8 (20:12) | PAGE_FIXED (21:21),
   USERD_INDEX_FIXED 0, runqueue 0 (alloc_channel.h:130-205); hContextShare, hVASpace,
   hUserdMemory[0]/userdOffset[0]; memdescs {base, size, addressSpace 1 (ADDR_SYSMEM,
   g_mem_desc_nvoc.h:115), cacheAttrib 0 cached / 1 uncached (nv_memory_type.h:30-31)}:
   instanceMem {inst, 4096, 1, 0}, ramfcMem {inst + 0, 512, 1, 0} (kernel_channel_gm107.c:289-291),
   userdMem {USERD IOVA, 512, 1, 0}, mthdbufMem {fault buffer 0}, errorNotifierMem {IOVA, 4096,
   1, 0}; internalFlags PRIVILEGE 0 USER (1:0) | ERROR_NOTIFIER_TYPE 3 MEMORY (3:2) |
   ECC_ERROR_NOTIFIER_TYPE 1 NONE (5:4) (g_kernel_channel_nvoc.h:108-118, 181-204); ProcessID.
   Ring, USERD and notifier objects are then held (agpu_mem_hold) until GSP freed the channel.
9. CONTROL 0x2080012b GPU_PROMOTE_CTX on (client, subdevice), 560 B: engineType 1, hChanClient,
   hObject = channel, entries {PA, VA, size, physAttr, bufferId, bInitialize, bNonmapped}:
   MAIN(0) and PATCH(2) init + VA; PRIV_ACCESS_MAP(10) init once per device, VA once per VAS;
   UNRESTRICTED_PRIV_ACCESS_MAP(11) init once, nonmapped. physAttr = APERTURE COH_SYS 1 (1:0) |
   GPU_CACHEABLE_NO (2:2) (ctrl2080gpu.h:930-1010, 1095-1102). See "Promote ids" below.
10. GSP_RM_ALLOC <compute class> under the channel, no params.
11. Token = (runlist << 16) | ChID | bit 30 (om/rmarg.om op 4; kfifoGenerateWorkSubmitTokenHal_GB202,
    the HAL GB20B takes, kernel_fifo_gb202.c; gb202/dev_vm.h:27-32: RUNLIST_ID 22:16, VECTOR 11:0,
    RUNLIST_DOORBELL 30:30 ENABLE = 1; runlist above 127 refused).
12. CONTROL 0xa06c0101 GPFIFO_SCHEDULE on the TSG {bEnable 1, 0, 0} (kernel_channel_group_api.c:1065-1191).

Any failure frees in reverse what GSP accepted; a failed FREE keeps (leaks) that channel's memory and
marks the file failed.

### Teardown (spec order, kernel_graphics_object.c:237-280, kernel_channel.c:1217-1273, device.c:236-290)

AGPU_CHAN_DESTROY / file release: FREE (fn 10) compute, channel, ctxshare, TSG; release holds, ChID,
kernel buffers. Then FREE vaspace, subdevice, device, client; then agpu_vas_destroy, or
agpu_vas_abandon if any FREE failed. Driver remove (kgspUnloadRm_IMPL, kernel_gsp.c:4319-4385):
UNLOADING_GUEST_DRIVER (47) {0, 0, 0} (rpc.c:9166-9190), MAILBOX0 0x110040 == 0x80000000 (om/rmarg.om
op 5; kernel_gsp_tu102.c:1226-1249), RISC-V CPUCTL 0x111388 HALTED 4:4 (op 6; kernel_gsp_gh100.c:995-1004;
gh100/dev_riscv_pri.h:62); each poll bounded at 2,000,000 reads about 1 us apart.

### RPC call helper (rm_rpc)

Build -> om/seal.om + om/mqput.om with sequence d->rpc_seq (rpc_seq++ per sync RPC, kernel_gsp.c:405)
-> read the status queue until the message with the same function and sequence (kernel_gsp.c:1813-1817);
CPU-sequencer events run through agpu_seq_run; other events are read past; 10 s deadline ->
AGPU_RM_E_TIMEOUT. Reply checks (om/rmchk.om op 1): rpc_result != 0 -> AGPU_RM_E_RESULT; the RPC's
own status (GSP_RM_ALLOC +16, GSP_RM_CONTROL +12, FREE +12) != 0 -> AGPU_RM_E_STATUS
(rpc.c:11236-11241, 10978-11060, 11311-11354).

### Doorbell page

mmap offset 1 << 40: BAR0 + 0xbb0000, 64 KiB, io_remap_pfn_range, pgprot_noncached, VM_IO |
VM_DONTEXPAND | VM_DONTDUMP, no exec; doorbell word at +0x90 (spec section 4 below).

## Promote ids for a compute object (resolved gap)

GB10B is a GSP client, so kgrobjGetPromoteIds resolves to _FWCLIENT
(g_kernel_graphics_object_nvoc.c:587-591), and gpuIsClientRmAllocatedCtxBufferEnabled is TRUE
(gpu_registry.c:152-155). Compute in a subcontext TSG (not legacy mode) takes the non-3D list
(kernel_graphics_object.c:580-617): MAIN, PM, PATCH, FECS_EVENT, PRIV_ACCESS_MAP,
UNRESTRICTED_PRIV_ACCESS_MAP. Which become entries (kgrobjPromoteContext, :51-163):

- MAIN, PATCH: PA initialized (kgrctxPrepareInitializeCtxBuffer, kernel_graphics_context.c:1689-1855)
  and VA promoted (kgrctxPreparePromoteCtxBuffer, :1861-1950, RM-owned VAS only).
- PM: no entry; its memdesc is NULL because kgrctxShouldPreAllocPmBuffer_PF returns FALSE for an
  RM-owned VAS (:2501-2536).
- FECS_EVENT: no entry; ctxsw logging is disabled for GSP clients without profiling
  (kernel_graphics.c:1079-1083), so the buffer is not allocated (kgraphics_gp100.c:262-264).
- PRIV_ACCESS_MAP: mapped for a user channel (kgrctxGetRegisterAccessMapId_IMPL, :3285-3300 picks
  UNRESTRICTED only for admin/kernel) and promoted; initialized once per device.
- UNRESTRICTED_PRIV_ACCESS_MAP: allocated (kgraphicsIsUnrestrictedAccessMapSupported_PF), initialized
  (bNonmapped, no VA) once per device.
- Global 3D buffers (BUNDLE_CB, PAGEPOOL, ATTRIBUTE_CB, RTV_CB, GFXP_*): not for compute in a
  subcontext TSG (kgrctxMapCtxBuffers :1664-1680).

## Appendix A. Source spec (research, kept with its cites)

**GB10B HAL.** `HalVarIdx = 63` (generated/g_chips2halspec_nvoc.c:114-117). The variant in GSP-client mode is PF_KERNEL_ONLY, idx 1 (g_chips2halspec_nvoc.c:197-200; g_chips2halspec_nvoc.h:97).

HAL bindings were resolved by evaluating the NVOC `chipHal` masks for idx 63 (group 1, bit 31). The results that matter here:

- `PDB_PROP_GPU_ZERO_FB = TRUE` and `ATS_SUPPORTED = TRUE` (g_gpu_nvoc.c:158-167, 214-223).
- `bBar1Disabled = TRUE` and `bCpuVisibleBar2Disabled = TRUE` (g_kern_bus_nvoc.c:333-355).
- `kchannelAllocMem_GM107`, `kfifoGetInstMemInfo_GM107`, `kfifoGenerateWorkSubmitTokenHal_GB100`, `kfifoConstructUsermodeMemdescs_GH100`, `kgmmuCommitTlbInvalidate_GB100`, `kgmmuFmtFamiliesInit_GH100`, `kgspTeardown_GH100` and `kgspWaitForProcessorSuspend_TU102`.

**Live host facts, read-only sysfs, 2026-10-26.**

- The GPU is `000f:01:00.0`, device `0x2e12`.
- BAR0 is `0x24000000-0x27ffffff` (64 MiB).
- The GPU sits in iommu_group 20. The system has 20 groups of type `DMA-FQ` and 1 of type `DMA`. Group 20's own type was not read; the job was cut off by the budget.

## 0. Wire format (shared by every RPC below)

`rpc_message_header_v03_00` is 32 bytes (generated/g_rpc-message-header.h:41-52):

| Offset | Field | Value |
|---|---|---|
| 0 | `header_version` | `0x03000000` (MAJOR_TOT=3 in bits 31:24, rpc_headers.h:56-59) |
| 4 | `signature` | `0x43505256` (rpc_headers.h:61) |
| 8 | `length` | 32 + paramLength (rmapi/rpc_common.c:183) |
| 12 | `function` | RPC function number |
| 16 | `rpc_result` | |
| 20 | `rpc_result_private` | |
| 24 | `sequence` | |
| 28 | `u.spare` / `cpuRmGfid` | |
| 32 | payload | |

The writer is rpc_common.c:154-183.

**GSP_RM_ALLOC (fn 103).** `rpc_gsp_rm_alloc_v03_00`, 32 bytes:

| Offset | Field |
|---|---|
| 0 | `hClient` |
| 4 | `hParent` |
| 8 | `hObject` |
| 12 | `hClass` |
| 16 | `status` |
| 20 | `paramsSize` |
| 24 | `flags` |
| 28 | `reserved[4]` |
| 32 | `params[]` |

- The header length counts only `sizeof(rpc_gsp_rm_alloc_v03_00)`, not the params (rpc.c:11195-11217; prior art in aienux GB10_FIRMWARE_INVENTORY.md §14.2.5).
- For our classes the params are raw C structs, not FINN-serialized. `serverSerializeAllocDown` uses interface `0xA000` (rmapi_finn.c:358-365), and `finnSerializeRoot` has no case for it, so it returns NOT_SUPPORTED and the size is 0 (interface/rmapi/src/g_finn_rm_api.c:602-652, 781-791).

**GSP_RM_CONTROL (fn 76).** `rpc_gsp_rm_control_v03_00`, 40 bytes:

| Offset | Field |
|---|---|
| 0 | `hClient` |
| 4 | `hObject` |
| 8 | `cmd` |
| 12 | `status` |
| 16 | `paramsSize` |
| 20 | `rmapiRpcFlags` |
| 24 | `rmctrlFlags` |
| 28 | `rmctrlAccessRight` |
| 32 | `reserved0` (u64) |
| 40 | `params[]` |

Here the length does count the params (rpc.c:10978-10993).

**FREE (fn 10).** `NVOS00_PARAMETERS_v03_00`, 16 bytes:

| Offset | Field | Value |
|---|---|---|
| 0 | `hRoot` | |
| 4 | `hObjectParent` | 0 |
| 8 | `hObjectOld` | |
| 12 | `status` | |

Source: rpc.c:11311-11354.

**Routing.** The physical RM API for GSP clients is `rpcRmApiAlloc_GSP`/`Control_GSP`/`Free_GSP` (rmapi/rpc_common.c:77). The macros `NV_RM_RPC_ALLOC_OBJECT`, `_ALLOC_CHANNEL`, `_ALLOC_SUBDEVICE`, `_FREE` and `_CONTROL` all go through it when `IS_FW_CLIENT` (inc/kernel/vgpu/rpc.h:223-336, 367-392).

**Layout method.** Offsets were computed by compiling the real 580.173.02 headers with host gcc (aarch64, LP64) and running `offsetof` and `gdb ptype /o`. Inputs and outputs are in `~/handoffs/owndriver-stagec/layout/` (t2.c, t3.c, t4.c, layouts1.txt, layouts2.txt, offsets.txt).

For `GspStaticConfigInfo` the struct body was copied verbatim from inc/kernel/gpu/gsp/gsp_static_config.h:45-169, and `gpu.h` was replaced by two macros:

- `MAX_GROUP_COUNT = 2` (generated/g_gpu_nvoc.h:896).
- `NVGPU_ENGINE_CAPS_MASK_ARRAY_MAX = (0x54-1)/32+1 = 3` (gpu_engine_type.h:118; nvbitmask.h:34-35).

GSP-RM is RISC-V LP64. aarch64 nvidia.ko uses the same headers against the same firmware, so the natural alignment is identical.

## 1. Post-INIT_DONE bring-up (once per GPU)

| # | RPC / action | fn | Payload | Source | Values from |
|---|---|---|---|---|---|
| pre | `GSP_SET_SYSTEM_INFO`, `SET_REGISTRY` | 72, 73 | `GspSystemInfo`, registry table | queued async *before* boot: kernel_gsp.c:3792-3810, 4179 | stage B (not repeated here) |
| 1.1 | wait for event `GSP_INIT_DONE` | 0x1001 | none | kernel_gsp.c:5252-5267 | n/a |
| 1.2 | `SET_GUEST_SYSTEM_INFO` (sync; RPC version handshake) | 1 | `rpc_set_guest_system_info_v03_00`, 792 B (table below) | kernel_gsp.c:4263; rpc.c:8653-8834 | constants: `VGX_MAJOR=0x2B`, `MINOR=0x13` (inc/kernel/vgpu/vgpu_version.h:33-34). The reply may return a different major/minor; the driver retries once with the host value (rpc.c:8764-8784) and then `rpcSetIpVersion` (rpc.c:8821) |
| 1.3 | `SET_GUEST_SYSTEM_INFO_EXT` | 64 | **not sent** in GSP-client mode | macro is "VGPU only" (inc/kernel/vgpu/rpc_vgpu.h:71-78); `gpuGetVgpuRpc` returns NULL unless `IS_VIRTUAL` (gpu/gpu.c:6003-6011) | n/a |
| 1.4 | `GET_GSP_STATIC_INFO` | 65 | zeroed request, length = 32 + 1792; reply payload = `GspStaticConfigInfo` (1792 B) at message offset 32 | kernel_gsp.c:4270; rpc.c:9661-9699 | GSP |
| 1.5 | `INIT_GSP_TRACE_CRASH_BUFFER` | 228 | optional, registry-gated; skip | kernel_gsp.c:3359-3400, 4277 | n/a |
| 1.6 | adopt internal handles; **no RPC** (GSP-RM allocated them itself) | none | `hInternalClient` (@1728), `hInternalDevice` (@1732), `hInternalSubdevice` (@1736) | gpu/gpu.c:965-972 | static info |
| 1.7 | GR static data (CPU-RM caches; needed for ctx-buffer sizes and `GR_GET_INFO`): `GSP_RM_CONTROL` on (`hInternalClient`, `hInternalSubdevice`) | 76 | `0x20800a1f` KGR_GET_CAPS, `0x20800a2a` GET_INFO, `0x20800a26` FLOORSWEEPING_MASKS, `0x20800a32` GET_CONTEXT_BUFFERS_INFO, plus PPC/ZCULL/ROP/SM_ORDER/FECS controls | gpu/gr/kernel_graphics.c:1121-1511 (load), 625-749 (deferred, ctx buffers info); all flagged `ROUTE_TO_PHYSICAL` (g_subdevice_nvoc.c:2041, 2116, 2161, 2221) | GSP |
| 1.8 | FIFO engine table (runlist id of GR0, PBDMA, CHRAM/runlist PRI bases) | 76 | `NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE` `0x20801112`; params 3212 B: `baseIndex`@0, `numEntries`@4, `bMore`@8, `entries[32]`@12 (100 B each: `engineData[16]`@0, `pbdmaIds[2]`@64, `pbdmaFaultIds[2]`@72, `numPbdmas`@80, `engineName[16]`@84); also `INTERNAL_FIFO_GET_NUM_CHANNELS` | gpu/fifo/kernel_fifo.c:2006-2061, 1328; common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080fifo.h:618-659 | GSP |
| 1.9 | CE fault-method-buffer size | 76 | `NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE` → `{u32 size}` | gpu/ce/kernel_ce.c:827-848 | GSP |

`rpc_set_guest_system_info_v03_00` layout (step 1.2):

| Offset | Field | Value |
|---|---|---|
| 0 | `vgxVersionMajorNum` | |
| 4 | `vgxVersionMinorNum` | |
| 8 | `guestDriverVersionBufferLength` | |
| 12 | `guestVersionBufferLength` | |
| 16 | `guestTitleBufferLength` | |
| 20 | `guestClNum` | |
| 24 | `guestDriverVersion[256]` | |
| 280 | `guestVersion[256]` | |
| 536 | `guestTitle[256]` | |

Other CPU-local consequences of the static info and HAL:

- `bSplitVasManagementServerClientRm = TRUE` by default for GSP clients (gpu/gpu_registry.c:171-182). The static-info flag @1693 is consumed only by the vGPU path.
- `bClientRmAllocatedCtxBuffer = TRUE` for GSP clients (gpu_registry.c:152-155), so **CPU-RM allocates the GR context buffers**.
- ZERO_FB sets BROKEN_FB (gpu_registry.c:59-69), which sets inst-loc ALL_COH and `IS_ALL_INST_IN_SYSMEM` with cached instance memory (gpu_registry.c:294-304, 324-334). As a result instance blocks, USERD and method buffers are in **cached sysmem** (gpu/fifo/arch/maxwell/kernel_fifo_gm107.c:61-86).
- BAR2 init is skipped for GSP clients with CPU-visible BAR2 disabled (gpu/bus/arch/maxwell/kern_bus_gm107.c:326-329). So **no `UPDATE_BAR_PDE` (70)**, and `bar1PdeBase`/`bar2PdeBase` are unused on GB10B.

### `GspStaticConfigInfo`, 580.173.02, sizeof = 1792

| Offset | Size | Field | Used by |
|---|---|---|---|
| 0 | 23 | `grCapsBits` | GR caps |
| 24 | 268 | `gidInfo` (`NV2080_CTRL_GPU_GET_GID_INFO_PARAMS`) | `GPU_GET_GID_INFO` (subdevice_ctrl_gpu_kernel.c:3627-3700); gpu_gspclient.c:155-161 |
| 292 | 48 | `SKUInfo` | |
| 344 | 904 | `fbRegionInfoParams` | memmgr FB regions; expected empty on 0-FB (UNKNOWN) |
| 1248 | 80 | `sriovCaps` | |
| 1328 | 4 | `sriovMaxGfid` | |
| 1332 | 12 | `engineCaps[3]` | engine presence → classlist |
| 1344 | 1 | `poisonFuseEnabled` | |
| 1352 | 8 / 8 | `fb_length` / `fbio_mask` | |
| 1368 | 4 / 4 | `fb_bus_width` / `fb_ram_type` | |
| 1376 | 8 | `fbp_mask` | |
| 1384 | 4 | `l2_cache_size` | |
| 1388 | 64 / 64 / 128 | `gpuNameString` / `gpuShortNameString` / `gpuNameString_Unicode` | |
| 1644-1657 | 1 each | `bGpuInternalSku` … `bIsMigSupported` | |
| 1658 / 1660 | 2 / 2 | `RTD3GC6TotalBoardPower` / `PerstDelay` | |
| 1664 / 1672 | 8 / 8 | `bar1PdeBase` / `bar2PdeBase` | not used on GB10B |
| 1680 | 1 | `bVbiosValid` | |
| 1684 / 1688 | 4 / 4 | `vbiosSubVendor` / `SubDevice` | |
| 1692 | 1 | `bPageRetirementSupported` | |
| 1693 | 1 | `bSplitVasBetweenServerClientRm` | |
| 1694 | 1 | `bClRootportNeedsNosnoopWAR` | |
| 1696 / 1704 / 1720 | 8 / 12 / 8 | displayless heads / res / pixels | |
| **1728 / 1732 / 1736** | 4 / 4 / 4 | **`hInternalClient` / `hInternalDevice` / `hInternalSubdevice`** | all internal controls |
| 1740-1743 | 1 each | `bSelfHostedMode`, `bAtsSupported`, `bIsGpuUefi`, `bIsEfiInit` | |
| 1744 | 24 | `ecidInfo[2]` | |
| 1768 | 16 | `fwWprLayoutOffset` | `_kgspSetFwWprLayoutOffset` (kernel_gsp.c:3818) |
| 1784 | 1 | `bSystemRebootRequired` | |

The static info does not carry the classlist or the BAR/vaspace parameters our channel path needs. Classlist and `GR_GET_INFO` are answered by CPU-RM locally:

- classlist: `deviceCtrlCmdGpuGetClasslist`, gpu/device_ctrl.c:56, flags 0x10b with no route (g_device_nvoc.c:183).
- `GR_GET_INFO`: gpu/gr/kernel_graphics.c:3004, flags 0x118 (g_subdevice_nvoc.c:5206).
- `NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2` (0x205): CPU-local, rmapi/client_resource.c:1405, flags 0x109 (g_client_resource_nvoc.c:813).

Our module answers all three itself.

## 2. Per-process object chain (what nvidia.ko does for each client RM call)

Handles are the client's own. CPU-RM uses the same `hClient`/`hObject` values on GSP. Each row is in client order (cudapp/nvrm/nvrm.c:326-766).

| # | Client call | CPU-RM work | GSP RPC(s) | Source |
|---|---|---|---|---|
| 2.1 | `RM_ALLOC NV01_ROOT_CLIENT` | create client locally; **no RPC** | none | resource_list.h:84 (no RPC flag) |
| 2.2 | `RM_ALLOC NV01_DEVICE_0` (`NV0080_ALLOC_PARAMETERS`) | device state | first device of the client: `GSP_RM_ALLOC(hClient, 0, hClient, NV01_ROOT=0x0, NV0000_ALLOC_PARAMETERS)`, then `GSP_RM_ALLOC(hClient, hClient, hDevice, 0x80, NV0080 params)` | gpu/device.c:180-191; rpc.h:46-111 (`_FWCLIENT` macro); rpc.c:11182-11185 |
| 2.3 | `RM_ALLOC NV20_SUBDEVICE_0` | register subdevice | `GSP_RM_ALLOC(..., 0x2080, {subDeviceId = inst})` | gpu/subdevice/subdevice.c:122-127; rpc.h:367-392 |
| 2.4 | `RM_ALLOC NV01_MEMORY_VIRTUAL` (`NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS`) | VA range object | `GSP_RM_ALLOC` mirror | mem_mgr/virt_mem_range.c:125-134 |
| 2.5 | `RM_ALLOC HOPPER_USERMODE_A` (0xc661) under subdevice | memdesc `pRegVF` = BAR0 `[virtualRegPhysOffset + 0x30000, +0x10000)` (`ADDR_REGMEM`); **no RPC** | none | gpu/fifo/usermode_api.c:32-104; gpu/fifo/arch/volta/kernel_fifo_gv100.c:155-172, 353-374; resource_list.h:885 |
| 2.6 | `RM_ALLOC FERMI_VASPACE_A` (`NV_VASPACE_ALLOCATION_PARAMETERS`) | **first** `GSP_RM_ALLOC(..., 0x90f1, params)`; **then** CPU builds its own `gvaspace` (MMU walker). With split VAS it reserves `[4 GiB, 4.5 GiB)` for GSP, pins PDs down to PD0 and sends that range's PDEs | (a) `GSP_RM_ALLOC` (gpu/mem_mgr/vaspace_api.c:227-276); (b) `GSP_RM_CONTROL NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES 0x90f10106` on `hVASpace` | gpu_vaspace.c:314-392, 4039-4157, 4431-4487; range consts g_gpu_vaspace_nvoc.h:99-100 |
| 2.7 | `RM_ALLOC NV01_MEMORY_SYSTEM` (`NV_MEMORY_ALLOCATION_PARAMS`) | pin pages, memdesc; **no RPC** (RPC only `if IS_VIRTUAL`) | none, unless `ATTR2_REGISTER_MEMDESC_TO_PHYS_RM`, which goes to `ALLOC_MEMORY` (§3) | mem_mgr/system_mem.c:461-502; mem.c:486-492 |
| 2.8 | `RM_ALLOC KEPLER_CHANNEL_GROUP_A` (`NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS`) | TSG and grpID; ctx-buf pools; allocates `KERNEL_GRAPHICS_CONTEXT` **locally** (`SKIP_RPC`); allocates fault-method buffers (2 runqueues × CE size, sysmem cached) | (a) `GSP_RM_ALLOC(..., 0xa06c, params)`; (b) `GSP_RM_CONTROL NVA06C_CTRL_CMD_INTERNAL_PROMOTE_FAULT_METHOD_BUFFERS 0xa06c010a` (`ROUTE_TO_PHYSICAL`, g_kernel_channel_group_api_nvoc.c:333) with `methodBufferMemdesc[i].base` = GPU PA | gpu/fifo/kernel_channel_group_api.c:405-504; arch/volta/kernel_channel_group_gv100.c:44-110; g_kernel_fifo_nvoc.h:2048 (runqueues = 2) |
| 2.9 | `RM_ALLOC FERMI_CONTEXT_SHARE_A` (`NV_CTXSHARE_ALLOCATION_PARAMETERS`) | subctx state | generic `GSP_RM_ALLOC` after the constructor (`RS_FLAGS_ALLOC_RPC_TO_ALL`) | resource_list.h:405; rmapi/alloc_free.c:869-929 |
| 2.10 | `RM_ALLOC BLACKWELL_CHANNEL_GPFIFO_A` (`NV_CHANNEL_ALLOC_PARAMS`) | see §2a | `GSP_RM_ALLOC(hClient, hTSG, hChannel, 0xcd6f?, NV_CHANNEL_ALLOC_PARAMS)` with CPU-filled memdescs; sent through `NV_RM_RPC_ALLOC_CHANNEL`, which takes the `IS_FW_CLIENT` branch, i.e. **`GSP_RM_ALLOC`, not `ALLOC_CHANNEL_DMA` (6)** | gpu/fifo/kernel_channel.c:951-960, 2665-2860; rpc.h:245-269 |
| 2.11 | `RM_ALLOC BLACKWELL_COMPUTE_B` (`NV_GR_ALLOCATION_PARAMETERS` optional; client passes NULL) | see §2b | (a) `GSP_RM_CONTROL NV2080_CTRL_CMD_GPU_PROMOTE_CTX 0x2080012b` (`ROUTE_TO_PHYSICAL`, g_subdevice_nvoc.c:316); (b) generic `GSP_RM_ALLOC(..., COMPUTE_B, params)` | gpu/gr/kernel_graphics_object.c:51-163, 172-228; resource_list.h:2113; resource_desc_flags.h:45 |
| 2.12 | `NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN` 0xc36f0108 | computed **on the CPU**: `token = (runlistId << 16) | ChID | 1 << 30` on GB20B (`DOORBELL_RUNLIST_ID` 22:16, `VECTOR` 11:0, `RUNLIST_DOORBELL` 30:30 ENABLE; GB10B's GB100 HAL has no bit 30); then writes the notifier | none (vGPU-only RPC branch) | kernel_channel.c:3302-3368; arch/blackwell/kernel_fifo_gb100.c:89-123; common/inc/swref/published/blackwell/gb100/dev_vm.h:620-625 |
| 2.13 | UVM register channel (externally-owned route only) | see §3 | `GSP_RM_CONTROL PROMOTE_CTX` with VAs | nv_gpu_ops.c:10827-10913 |
| 2.14 | `NVA06C_CTRL_CMD_GPFIFO_SCHEDULE` 0xa06c0101 `{bEnable@0, bSkipSubmit@1, bSkipEnable@2}` (3 B) | checks `kchannelIsSchedulable` for each channel: externally-owned VAS + GR **requires `bIsContextBound`**; sets runlist ids | `GSP_RM_CONTROL` forwarding the same cmd and params | kernel_channel_group_api.c:1065-1191; kernel_channel.c:2189-2227 |

The GPFIFO class number in row 2.10 is not settled from source. Use the client's classlist value; `BLACKWELL_CHANNEL_GPFIFO_A` is `0xc96f` in clc96f.h.

### Alloc param layouts (bytes)

| Struct (header:line) | Size | Fields @offset |
|---|---|---|
| `NV0000_ALLOC_PARAMETERS` (class/cl0000.h:52) | 120 | `hClient`@0, `processID`@4, `processName[100]`@8, `pOsPidInfo`@112 |
| `NV0080_ALLOC_PARAMETERS` (cl0080.h:64) | 56 | `deviceId`@0, `hClientShare`@4, `hTargetClient`@8, `hTargetDevice`@12, `flags`@16, `vaSpaceSize`@24, `vaStartInternal`@32, `vaLimitInternal`@40, `vaMode`@48 |
| `NV2080_ALLOC_PARAMETERS` (cl2080.h:45) | 4 | `subDeviceId`@0 |
| `NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS` (cl0070.h:70) | 24 | `offset`@0, `limit`@8, `hVASpace`@16 |
| `NV_VASPACE_ALLOCATION_PARAMETERS` (nvos.h:3164) | 56 | `index`@0, `flags`@4, `vaSize`@8, `vaStartInternal`@16, `vaLimitInternal`@24, `bigPageSize`@32, `vaBase`@40, `pasid`@48 |
| `NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS` (nvos.h:2911) | 20 | `hObjectError`@0, `hObjectEccError`@4, `hVASpace`@8, `engineType`@12, `bIsCallingContextVgpuPlugin`@16 |
| `NV_CTXSHARE_ALLOCATION_PARAMETERS` (nvos.h:3237) | 12 | `hVASpace`@0, `flags`@4, `subctxId`@8 |
| `NV_MEMORY_DESC_PARAMS` (alloc/alloc_channel.h:37-42) | 24 | `base`@0, `size`@8, `addressSpace`@16, `cacheAttrib`@20 |
| `NV_CHANNEL_ALLOC_PARAMS` (alloc_channel.h:296-342) | 368 | see breakdown below |
| `NV_GR_ALLOCATION_PARAMETERS` (nvos.h:2721) | 16 | `version`@0, `flags`@4, `size`@8, `caps`@12 |
| `NV_HOPPER_USERMODE_A_PARAMS` (nvos.h:3336) | 2 | `bBar1Mapping`@0, `bPriv`@1 (NULL → BAR0 mapping, usermode_api.c:61-98) |

`NV_CHANNEL_ALLOC_PARAMS` field offsets:

| Offset | Field |
|---|---|
| 0 | `hObjectError` |
| 4 | `hObjectBuffer` |
| 8 | `gpFifoOffset` |
| 16 | `gpFifoEntries` |
| 20 | `flags` |
| 24 | `hContextShare` |
| 28 | `hVASpace` |
| 32 | `hUserdMemory[8]` |
| 64 | `userdOffset[8]` |
| 128 | `engineType` |
| 132 | `cid` |
| 136 | `subDeviceId` |
| 140 | `hObjectEccError` |
| 144 | `instanceMem` |
| 168 | `userdMem` |
| 192 | `ramfcMem` |
| 216 | `mthdbufMem` |
| 240 | `hPhysChannelGroup` |
| 244 | `internalFlags` |
| 248 | `errorNotifierMem` |
| 272 | `eccErrorNotifierMem` |
| 296 | `ProcessID` |
| 300 | `SubProcessID` |
| 304 | `encryptIv[3]` |
| 316 | `decryptIv[3]` |
| 328 | `hmacNonce[8]` |
| 360 | `tpcConfigID` |

### 2a. Channel (`kchannelConstruct_IMPL`, gpu/fifo/kernel_channel.c:133-1145), in order

1. Look up the TSG. The client passes one; otherwise CPU-RM allocates an internal one with `SKIP_RPC` (356-409). Get the ctxshare and its VAS (642-709).
2. Error notifier: CPU resolves `hObjectError` to a memdesc and fills `errorNotifierMem = {GPU PA + offset, size - offset, addrSpace, cacheAttrib}`. It also writes `internalFlags.ERROR_NOTIFIER_TYPE` (562-610).
3. ChID: allocated **by CPU-RM** (`kchannelAllocHwID_GM107`, arch/maxwell/kernel_channel_gm107.c:420), per runlist when CHRAM is per-runlist (830-847).
4. Instance memory (`_kchannelAllocOrDescribeInstMem`, 2294-2382; `kchannelAllocMem_GM107`, kernel_channel_gm107.c:163-343):
   - **Instance block**: `NV_RAMIN_ALLOC_SIZE` 4096 B, aligned to `1 << 12` (published/maxwell/gm107/dev_ram.h:36-37). Sysmem cached on GB10B: FBMEM is forced only when `!ZERO_FB` (207-212). Zeroed on allocation.
   - **RAMFC**: a submemdesc at offset 0, `DRF_SIZE(NV_RAMIN_RAMFC)/8` = 512 B (dev_ram.h:38; kernel_channel_gm107.c:289-291).
   - **USERD**: the client's `hUserdMemory[0]` + `userdOffset[0]` (`kchannelCreateUserdMemDescBc_GV100`, arch/volta/kernel_channel_gv100.c:69). Size `1 << 9` = 512 B (kernel_fifo_gm107.c:1545-1555). Zeroed when in sysmem (`kfifoSetupUserD_GM107`, 797-808, called at kernel_channel.c:2362-2375).
   - **GSP does not allocate instance memory in client mode.** It receives base, size and aperture.
5. Join the TSG (894). `kchannelAllocChannel` is a no-op in kernel RM (g_kernel_channel_nvoc.h:1275).
6. RPC (2665-2846). Copies the client fields, then sets:
   - `instanceMem`, `ramfcMem`, `userdMem`, `mthdbufMem[runqueue]` as GPU PA, `memdescGetAddressSpace` and `CpuCacheAttrib`.
   - `flags`: `USERD_INDEX_FIXED = FALSE`, `USERD_INDEX_PAGE_FIXED = TRUE`, `USERD_INDEX_VALUE = ChID % 8`, `USERD_INDEX_PAGE_VALUE = ChID / 8`.
   - `internalFlags.PRIVILEGE`, `ProcessID` and `SubProcessID`.
   - `hObjectBuffer` is forced to 0.

### 2b. Compute object (`_kgrAlloc`, kernel_graphics_object.c:172-228)

1. `kgraphicsInitializeDeferredStaticData` sends `INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO` once (kernel_graphics.c:625-749).
2. `kgraphicsAllocKgraphicsBuffers` allocates the global ctx buffers: bundle CB, pagepool, attribute CB, RTV, FECS event, priv access maps.
3. `kgrctxAllocCtxBuffers` allocates main, patch and PM, all in CPU-allocated sysmem (kernel_graphics_context.c:1115-1446).
4. `kgrctxMapCtxBuffers` maps them into the channel VAS with CPU-written PTEs. It is **skipped for externally owned VAS** (kernel_graphics_context.c:1591-1615).
5. `kgrobjPromoteContext` sends one `PROMOTE_CTX`. Each entry carries the PA to initialize and, unless the VAS is externally owned, the VA (kernel_graphics_object.c:51-163; kgrctxPreparePromoteCtxBuffer at kernel_graphics_context.c:1861-1885). The promote ids for the FW-client HAL are listed at kernel_graphics_object.c:580-617.
6. The generic alloc path sends `GSP_RM_ALLOC(COMPUTE_B)` (alloc_free.c:916). At construct CPU-RM also sets the channel runlist by engine (channel_descendant.c:243).

`NV2080_CTRL_GPU_PROMOTE_CTX_PARAMS` (ctrl2080gpu.h:1000), 560 B:

| Offset | Field |
|---|---|
| 0 | `engineType` |
| 4 | `hClient` |
| 8 | `ChID` |
| 12 | `hChanClient` |
| 16 | `hObject` (channel) |
| 20 | `hVirtMemory` |
| 24 | `virtAddress` |
| 32 | `size` |
| 40 | `entryCount` |
| 48 | `promoteEntry[16]`, 32 B each |

Each entry (ctrl2080gpu.h:930):

| Offset | Field |
|---|---|
| 0 | `gpuPhysAddr` |
| 8 | `gpuVirtAddr` |
| 16 | `size` |
| 24 | `physAttr` |
| 28 | `bufferId` (u16) |
| 30 | `bInitialize` |
| 31 | `bNonmapped` |

## 3. Sysmem, GPU VA and TLB

**User sysmem is never described to GSP.** `NV01_MEMORY_SYSTEM` stays CPU-local (system_mem.c:461). GSP only learns physical addresses that arrive inside channel params and `PROMOTE_CTX`.

`ALLOC_MEMORY` (fn 4) exists only for `memRegisterWithGsp` (mem.c:514-584; class `NV01_MEMORY_LIST_SYSTEM`) and is not needed here. For reference, its `rpc_alloc_memory_v13_01` layout is 56 B:

| Offset | Field |
|---|---|
| 0 | `hClient` |
| 4 | `hDevice` |
| 8 | `hMemory` |
| 12 | `hClass` |
| 16 | `flags` (NVOS02) |
| 20 | `pteAdjust` |
| 24 | `format` (kind) |
| 32 | `length` |
| 40 | `pageCount` |
| 48 | `pteDesc` |

`pte_desc` packs `{idr:2, reserved:14, length:16}` into the first u32, with `pte_pde[i]` (u64) at +8 holding PA >> 12, 4 KiB frames (rpc.c:3458-3501, 2252-2307).

**Route A: externally owned VAS (current client, UVM).**

1. `nvGpuOpsDupAddressSpace` sends `DUP_OBJECT` (fn 21). `rpc_dup_object_v03_00` = `NVOS55_PARAMETERS_v03_00`, 28 B: `hClient`@0, `hParent`@4, `hObject`@8, `hClientSrc`@12, `hObjectSrc`@16, `flags`@20, `status`@24 (vaspace_api.c:474-478; rpc.c:11259-11309).
2. UVM builds its own page tree and calls `nvGpuOpsSetPageDirectory`, which issues `NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY 0x801813` with flags `APERTURE_SYSMEM_COH | ALL_CHANNELS` (nv_gpu_ops.c:8779-8895; uvm_va_space.c:1385-1400).
3. On GB10B (ZERO_FB + GSP client) CPU-RM first commits locally (`gvaspaceExternalRootDirCommit`, migrating the RM-reserved levels into the new root, gpu_vaspace.c:3024-3246). It then forwards `GSP_RM_CONTROL` with `physAddress` replaced by the root's GPU PA (gpu/mem_mgr/dma.c:471-491).
4. Params, 32 B (ctrl0080dma.h:840; gdb layouts1.txt): `physAddress`@0, `numEntries`@8, `flags`@12, `hVASpace`@16, `chId`@20, `subDeviceId`@24, `pasid`@28.
5. UVM writes PTEs itself, through its own internal CE channels (uvm_channel.c:2366). It invalidates TLBs with pushed host methods (uvm_hopper_host.c:228; uvm_blackwell_host.c:83).
6. `UVM_REGISTER_CHANNEL`:
   - `nvGpuOpsRetainChannel` (nv_gpu_ops.c:10126-10303) allocates `UVM_CHANNEL_RETAINER` (forwarded, resource_list.h:393-404), reads the token, and calls `GR_GET_CTX_BUFFER_INFO` (CPU-local).
   - UVM maps each ctx buffer at a VA inside its range (uvm_user_channel.c:488).
   - `nvGpuOpsBindChannelResources` sends `PROMOTE_CTX` with `bufferId` + `gpuVirtAddr` and sets `bIsContextBound` (nv_gpu_ops.c:10827-10913).
   - **Required before `GPFIFO_SCHEDULE`** (kernel_channel.c:2219-2225; kernel_channel_group_api.c:1103-1109).

**Route B: RM-managed VAS (no `IS_EXTERNALLY_OWNED`).**

- CPU-RM owns the page tables. The `gvaspace` MMU walker uses the ver3 format family (`kgmmuFmtFamiliesInit_GH100`, mmu/arch/hopper/kern_gmmu_gh100.c:53; PTE fields in common/inc/swref/published/hopper/gh100/dev_mmu.h:131-177).
- PTE contents:
  - `VALID` 0:0.
  - `APERTURE` 2:1, `SYS_COH = 2` / `SYS_NONCOH = 3`. `SYS_COH` is used when cache snoop is set (virt_mem_allocator_gm107.c:1329-1336).
  - `PCF` 7:3, volatile = `NVOS46_FLAGS_GPU_CACHEABLE_NO`, or `memdesc GpuCacheAttrib == UNCACHED` (1447-1466; 273-296).
  - `KIND` 11:8.
  - `ADDRESS` 51:12.
- `NV_ESC_RM_MAP_MEMORY_DMA` is CPU work only. `NV_RM_RPC_MAP_MEMORY_DMA` is gated off by split VAS (rpc.h:154-166) and by `bRpcAlloc = FALSE` for sysmem (virtual_mem.c:1549-1553).
- The only VAS RPCs are step 2.6 (alloc + `COPY_SERVER_RESERVED_PDES`).
- **TLB invalidate is a direct CPU BAR0 write, not an RPC** (`kgmmuInvalidateTlb_GM107`, mmu/arch/maxwell/kern_gmmu_gm107.c:103-259; `kgmmuCommitTlbInvalidate_GB100`, mmu/arch/blackwell/kern_gmmu_gb100.c:40-62):
  - BAR0 `0xB80000 + 0x30A0` (`PDB`: `APERTURE` 1:1 with SYS=1, `ADDR` 31:4) and `+0x30A4` (`UPPER_PDB`).
  - Then `+0x30B0` `MMU_INVALIDATE` with `ALL_VA` 0:0, `ALL_PDB` 1:1, `HUBTLB_ONLY` 2:2, `TRIGGER` 31:31.
  - Poll for completion (`kgmmuCheckPendingInvalidates_TU102`).
  - Register definitions: common/inc/swref/published/blackwell/gb100/dev_vm.h:327-402; VREG base from kern_gpu_tu102.c:93-100 (`FULL_PHYS_OFFSET` 0xB80000).
  - `rpcInvalidateTlb` (fn 200) is used only when the vGPU trap bit is set (kern_gmmu_gm107.c:146-156, 234-236).

**Recommendation for the first chip run: Route B.** Reasons, all from the source above:

1. It removes UVM entirely: no `DUP_OBJECT`, no `SET_PAGE_DIRECTORY` IOVA reordering, no CE channels for PTE writes, no retain or bind. Promote happens in one `PROMOTE_CTX` at object alloc.
2. `kchannelIsSchedulable` does not demand `bIsContextBound` for RM-owned VAS.
3. Our module already must own a CPU-side GMMU writer (aienux gmmu.c prior art). Route B uses exactly that writer plus a priv-register TLB invalidate.

What Route B costs:

- `COPY_SERVER_RESERVED_PDES` (params 184 B, ctrl90f1.h:332): `hSubDevice`@0, `subDeviceId`@4, `pageSize`@8, `virtAddrLo`@16, `virtAddrHi`@24, `numLevelsToCopy`@32, `levels[6]`@40 (24 B each: `physAddress`@0, `size`@8, `aperture`@16, `pageShift`@20).
- User allocations must keep out of `[0x100000000, 0x120000000)`.

## 4. Doorbell / usermode page

- BAR0 offset is `virtualRegPhysOffset (0xB80000) + DRF_BASE(NV_VIRTUAL_FUNCTION) (0x30000) = 0xBB0000`. Size is `DRF_SIZE(NVC361)` = 0x10000 (64 KiB).
  - Offset sources: kern_gpu_tu102.c:93-100, 271-288; g_gpu_access_nvoc.h:269; published/turing/tu102/dev_vm.h:27-29.
  - Size source: common/inc/swref/published/blackwell/gb100/dev_vm.h:31.
- Doorbell is `+0x90` (`NVC361_NOTIFY_CHANNEL_PENDING`, common/sdk/nvidia/inc/class/clc361.h:33; `NV_VIRTUAL_FUNCTION_DOORBELL` 0x30090, gb100/dev_vm.h:620).
- Our module mmaps BAR0 page `0x24000000 + 0xBB0000` (live BAR0 base above). It is uncached (`ATTR2_GPU_CACHEABLE_NO`; kernel_fifo_gv100.c:363-371).

## 5. Teardown

Client free order follows `RS_FREE_PRIORITY_HIGH` (channel, TSG) and then dependants:

1. COMPUTE_B: generic `FREE` (fn 10) via `bRpcFree` (alloc_free.c:973-985). CPU then unmaps and frees ctx buffers (kernel_graphics_object.c:237-280).
2. Channel: `FREE` (kernel_channel.c:1217-1229). CPU frees instance memory and ChID (1247-1273).
3. Ctxshare and TSG: generic `FREE`.
4. VAS: `FREE` (vaspace_api.c:570-572). Route A also sends `UNSET_PAGE_DIRECTORY` 0x801814 (`{hVASpace@0, subDeviceId@4}`).
5. VIRTUAL range: `FREE`. Sysmem and usermode: local only.
6. Subdevice: `FREE` (subdevice.c:221).
7. Device: `FREE`, and when it is the client's last device, `FREE(hClient, NULL, hClient)` (gpu/device.c:236-290).

Driver unload (`kgspUnloadRm_IMPL`, kernel_gsp.c:4319-4385):

1. `UNLOADING_GUEST_DRIVER` (fn 47). `rpc_unloading_guest_driver_v1F_07` is 8 B: `bInPMTransition`@0, `bGc6Entering`@1, `newLevel`@4; normal unload sends 0/0/0 (rpc.c:9166-9190).
2. Wait for `MAILBOX0 == 0x80000000` (`_kgspIsProcessorSuspended`, gsp/arch/turing/kernel_gsp_tu102.c:1226-1249).
3. `kgspTeardown_GH100` waits for the RISC-V to halt (gsp/arch/hopper/kernel_gsp_gh100.c:995-1004).

## 6. RPC / event numbers used (inc/kernel/vgpu/rpc_global_enums.h)

| Name | Number | Line |
|---|---|---|
| SET_GUEST_SYSTEM_INFO | 1 | 11 |
| ALLOC_MEMORY | 4 | 14 |
| ALLOC_CHANNEL_DMA | 6 (unused in GSP-client mode) | 16 |
| FREE | 10 | 20 |
| DUP_OBJECT | 21 | 31 |
| UNLOADING_GUEST_DRIVER | 47 | 57 |
| SET_PAGE_DIRECTORY | 54 (vGPU; GSP-client uses the control instead) | 64 |
| SET_GUEST_SYSTEM_INFO_EXT | 64 (not sent) | 74 |
| GET_GSP_STATIC_INFO | 65 | 75 |
| UPDATE_BAR_PDE | 70 (not on GB10B) | 80 |
| GSP_SET_SYSTEM_INFO | 72 | 82 |
| SET_REGISTRY | 73 | 83 |
| GSP_RM_CONTROL | 76 | 86 |
| CTRL_GPFIFO_SCHEDULE | 97 (vGPU) | 107 |
| GSP_RM_ALLOC | 103 | 113 |
| CTRL_GPU_PROMOTE_CTX | 111 (vGPU; GSP-client uses GSP_RM_CONTROL) | 121 |
| CTRL_VASPACE_COPY_SERVER_RESERVED_PDES | 116 (vGPU) | 126 |
| CTRL_GPFIFO_GET_WORK_SUBMIT_TOKEN | 186 (vGPU) | 196 |
| INVALIDATE_TLB | 200 (vGPU) | 210 |
| INIT_GSP_TRACE_CRASH_BUFFER | 228 | 238 |
| event GSP_INIT_DONE | 0x1001 | 253 |

## 7. UNKNOWN (only the chip settles these)

1. The vgxVersion GSP 580.173.02 answers to SET_GUEST_SYSTEM_INFO (we refuse anything but
   0x2B/0x13), and whether later RPCs work without the handshake.
2. The real GspStaticConfigInfo bytes on GB10B (handles, empty fbRegionInfo, bSplitVas).
3. Whether nvidia.ko sends another mandatory init control before user allocations succeed (needs an
   RPC trace of nvidia.ko in an authorized window).
4. That GSP accepts IOMMU IOVAs (group 20, type DMA) in instanceMem / userdMem / PDB / promote PAs.
5. GR context-buffer sizes/alignments, max subcontexts and the CE fault-method-buffer size (read at
   run time; an IOVA below GSP's alignment is refused rather than re-placed).
6. GR0's runlist id and numChannels (read at run time; determine the token).
7. Whether CPU writes to NV_VIRTUAL_FUNCTION_PRIV_MMU_INVALIDATE (BAR0 0xb830b0) pass PLM.
8. Whether GSP accepts COMPUTE alloc with our promote set (MAIN, PATCH, PRIV_ACCESS_MAP,
   UNRESTRICTED_PRIV_ACCESS_MAP; no PM, no FECS_EVENT), i.e. which ids are mandatory.
9. The GPFIFO and compute class GSP reports through GET_CLASSLIST_V2 on the internal device
   (source table says COMPUTE_A/GPFIFO_A; nvidia.ko on this GB10 allocated 0xcec0), and whether
   GSP-RM answers that control at all (CPU-RM answers it locally in nvidia.ko).
10. Whether UNLOADING_GUEST_DRIVER + halt leaves GB10B re-bootable without FLR.
11. Whether a client handle in 0xc1d80000.. is accepted for an RM client GSP has not seen from CPU-RM.
12. GP entry LEVEL bit: cudapp nvrm_gp_entry sets ENTRY1 bit 9 (clc96f.h: 1 = LEVEL_SUBROUTINE)
    while its comment says MAIN; tools/aien-gpu-m16 keeps the encoding the m16 receipt ran.
13. Whether GSP rejects a channel whose error notifier is ERROR_NOTIFIER_TYPE_MEMORY at a sysmem IOVA
    without the CPU-RM kernel mapping nvidia.ko has (kernel_channel.c:2080-2097).
