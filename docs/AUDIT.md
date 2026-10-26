# L332 audit: where the own-GPU-driver work stands, and what CUDA++ needs from a driver

Written 2026-10-26 for the aien-gpu repository (local at /tmp/owndrv/aien-gpu, not pushed). Every claim names its
source. Status words follow omega-communication: PROPOSED, IMPLEMENTED, EXERCISED, QUALIFIED, MERGED, DEPLOYED.
NOT_RUN means nothing ran; it is never a pass.

## 1. Where L332 lives

| What | Where | Source |
|---|---|---|
| Prior L332 branch | aien-dev/aienux `feat/own-gpu-driver`, head `c9678d7b5286b15cfdb92a648264de6d3e9e72ac` ("L332 rung 0: cap spark-window leases at 20 min per L405") | `git -C /tmp/owndrv/aienux log -1`, origin `https://github.com/aien-dev/aienux.git` |
| Pull request | aien-dev/aienux#441 "Own GB10 GPU driver, simulator gates": OPEN, not draft, merge state DIRTY (conflicts with main), 76 files, last updated 2026-10-23T17:10Z. Body: "Source and simulator gates only. This merge does not load the module and does not reboot." One open CodeQL finding: file created with mode 0666 at `native/kernel/tools/ck_gsp_sysinfo_encode.c:271` | `pr://aien-dev/aienux/441` read 2026-10-26 |
| Worktree | `~/workspace/aienos-own-gpu-driver`, also at `c9678d7b`, with one untracked directory `evidence/own-gpu-driver/rung0/a-20261023T171250Z/` (not in the PR) | `git -C ~/workspace/aienos-own-gpu-driver log -1`, `git status --short` |
| Related branches named in the handoff | `feat/devc-gpu` (worktree `~/workspace/aienos-devc-gpu`, devc GPU contracts), L193 GPU MMU simulator `feat/gb10-gpu-mmu-sim` | ~/handoffs/2026-10-21-own-gpu-driver.md:42,46; ~/handoffs/2026-10-24-nvidia-driver-out-omega.md:15 |
| Plain-English doc | `docs/GPU_DRIVER.md` (110 lines) on the branch | aienux@c9678d7b |
| Firmware facts | `docs/GB10_FIRMWARE_INVENTORY.md` (859 lines; PR adds 130) | aienux@c9678d7b; PR #441 file list |
| Code | `native/kernel/gpu/` (gmmu, gpu_chan, gpu_mem, gsp_mmio, gsp_seq, two simulators, devc contracts `gb10_chan.devc`, `gb10_gsp.devc` and generated C), `native/kernel/dev/gb10_gsp_boot.{c,h}`, out-of-tree Linux module `native/linux/aien-gpu/` (aien_gpu.c, aien_gsp_boot.c, aien_gpu_uapi.h, Kbuild) | `ls` of the clone |
| Ownership history | 2026-10-24 Drake: "fully remove the NVIDIA drivers from the Spark ... Replace it with our own language, Omega"; L332 taken over by orchestrator job d04927b9 after being idle since 2026-10-23T17:12Z | ~/handoffs/2026-10-24-nvidia-driver-out-omega.md:1-5 |
| This repository | /tmp/owndrv/aien-gpu, branch `main` with no commits yet; present: `om/boot.om`, `om/ident.om`, `om/seq.om`, `kmod/agpu_om.S` | `git -C /tmp/owndrv/aien-gpu log` ("does not have any commits yet"), `ls` on 2026-10-26 |

### 1.1 Status per rung

The rungs are defined in docs/GPU_DRIVER.md:72-87 (aienux@c9678d7b).

| Rung | What it is | Status | Evidence |
|---|---|---|---|
| 0 | Measure what NVIDIA's driver costs (driver calls per word, idle CPU, start-up calls) | NOT_RUN | Three window attempts, none opened (1.3). GPU_DRIVER.md:27-30 still says "pending the window" |
| 1 | Driver core in contract language, proven on the simulator | IMPLEMENTED; EXERCISED on the host simulator only. Hardware NOT_RUN | rung1 receipts (1.2) |
| 2, window 0 | Boot once with NVIDIA modules held back, load aien-gpu, read chip identity, give the GPU back | Scripts IMPLEMENTED; window NOT_RUN | window0 logs show the pre-check only (1.2) |
| 2, window 1 | Our start-up of NVIDIA's signed GSP program to INIT-DONE | IMPLEMENTED (module built, package built, 56 simulator checks per GPU_DRIVER.md:84); hardware NOT_RUN; gated on window 0 | rung2 build and sim receipts (1.2) |
| 2, CPU sequencer | Our copy of `kgspExecuteSequencerBuffer` (`native/kernel/gpu/gsp_seq.c`) | IMPLEMENTED; EXERCISED on host (13 checks + one mutant); hardware NOT_RUN | GPU_DRIVER.md:97-106; rung1/gsp-seq-20261022.txt |
| 3 | Same core inside AienOS natively (L318) | PROPOSED | GPU_DRIVER.md:87 |

Nothing in L332 is QUALIFIED, MERGED or DEPLOYED. The live driver is still NVIDIA's: `lsmod` on 2026-10-26 shows
`nvidia` (955 references), `nvidia_uvm`, `nvidia_modeset`, `nvidia_drm`; `/sys/module/nvidia/version` = `580.173.02`.

### 1.2 Receipts that exist (all under evidence/own-gpu-driver/ at aienux@c9678d7b)

| File | What it proves | What it does not prove |
|---|---|---|
| `rung1/gates-20261021T2300Z.txt` | At head 884596e2 on the Spark: devc 9/9 tests; generated channel contract identical to hand C; `test_gpu_sim` PASS 225 checks; contract trace 391 lines identical hand vs contract; contract mutants 10/10 killed; `test_gsp_mmio` PASS 67 checks; gsp_mmio mutants 20/20 killed; freestanding builds | Anything about the chip. The simulator is our model of the GB10, written from NVIDIA source, not the hardware |
| `rung1/gates-20261021T2158Z.txt` | Earlier run of the same gate script | Same limits |
| `rung1/gsp-seq-20261022.txt` | `GSP_SEQ_PASS checks=13`; a mutant with the save-index check removed fails the test | That GSP-RM ever sends a sequencer request on GB10B at cold boot (open question, ~/handoffs/2026-10-21-own-gpu-driver.md:136) |
| `rung2/build-receipts-20261022T0108Z.txt` | Module `aien_gpu.ko` built (`kmod-rc=0`, sha256 `aee06949cb1fd40dd7efc68c0be7f4e4df4ab0b36ce2c7bd01afd99e41c9df0a`, vermagic 7.0.0-1019-nvidia, params allow_bind arena_mib heap_mib allow_boot); GspSystemInfo encoder selftest 15 checks / 72 fields; layout matches NVIDIA's header text (70 comparisons, sizeof 936); window-1 package built from real firmware with pinned hashes | That the module was ever loaded. It was not |
| `rung2/sim-test-20261022T0120Z.txt` | Window-1 tool against the simulator, plain and ASan/UBSan: INIT-DONE, SEQUENCER-REQUESTED, silent, junk queue, E_BUSY, E_FMC, E_RISCV, plan-only and refused-build cases all behave as designed | That SEC2 or GSP-FMC accept the package |
| `rung2/window0-20261021T215956Z.log` | Pre-check run at commit d5891389: line 9 `PRE FAIL: nvidia modules in initramfs: the modprobe.d file would not hold them back` | Window 0 itself |
| `rung2/window0-20261021T220118Z.log` | Second pre-check: GPU holders listed (two aien-serve-http, nvidia-persistenced pid 1816); line 14 `checks only (no --go): PASS` | Window 0 itself; no `--go` run exists. Module hashed in this log is `da858c00...`, an older build than the window-1 module above |
| `rung2/system_info.txt` | The GspSystemInfo values chosen, with sources | That GSP-RM accepts them |
| `rung0/a-20261021T011957Z/window.open` | spark-window queued at place 6 for 724 s, then `Terminated`, `window open failed` | Any rung-0 measurement |
| `rung0/a-20261021T215416Z/window.open` | `spark-window: refused, the chip is reserved: L312 L354 ...` then `window open failed` | Any rung-0 measurement |
| worktree only: `rung0/a-20261023T171250Z/window.open` (107 lines, untracked) | spark-window waited about 18 minutes for the quiet lock, then `holder died before taking the lock`, `window open failed` | Any rung-0 measurement |

The `bin.sha256` files pin the test binary each attempt would have run; they are not results.

### 1.3 What never ran, and why

- **Rung 0.** All three window requests failed to open: queued and terminated (2026-10-21T01:19Z), refused because
  the chip was reserved for other lanes (2026-10-21T21:54Z), holder died while waiting for the quiet lock
  (2026-10-23T17:12Z). Sources: the three `window.open` files above.
- **Window 0.** Only the pre-check ran. The first pre-check failed because the NVIDIA modules are in the initramfs, so a
  modprobe.d file alone would not keep them out; the second passed in checks-only mode. The window needs a
  coordinator-scheduled reboot with every GPU user stopped (RUNBOOK.md:1-25), including the live 35B on 18091, and none
  was scheduled. ~/handoffs/2026-10-24-nvidia-driver-out-omega.md:15: "Rung 0 + window 0 (L390, reboot) never ran."
- **Window 1.** Built and simulator-tested, but gated on window 0 passing (GPU_DRIVER.md:84-86). Its stated risk: if
  the restart afterwards does not fully reset the GPU, it needs a power cycle at the wall (GPU_DRIVER.md:82-83).

## 2. The CUDA++ client's exact driver surface

Client: aien-dev/CUDApp main `780a0af1f08c1786ea53a4e8702ca8bb1396fdbb`, `nvrm/nvrm.c` (908 lines, licence
Apache-2.0 WITH LLVM-exception, nvrm.c:1). It includes NVIDIA's MIT headers straight from the open module tree
(`Makefile:20-23`, `-I$(NVIDIA_OPEN)/...`; includes at nvrm.c:24-38). It loads no NVIDIA user-space library.
Numbers below are from open-gpu-kernel-modules 580.173.02 headers (grep of `#define` in `src/` and `kernel-open/`).

### 2.1 Device files

| File | Opened at | Use |
|---|---|---|
| `/dev/nvidiactl` | nvrm.c:299 (control fd); nvrm.c:243 (per-mapping fd for system memory) | every NV_ESC ioctl; target of RM_MAP_MEMORY mmaps of system memory |
| `/dev/nvidia%u` (minor from CARD_INFO) | nvrm.c:233-237 | per-mapping fd, registered with NV_ESC_REGISTER_FD, then mmap of device objects (USERMODE page) |
| `/dev/nvidia-uvm` (opened twice) | nvrm.c:300-301 | UVM ioctls; second fd only for UVM_MM_INITIALIZE (nvrm.c:309-312) |

### 2.2 NVIDIA RM escapes (`/dev/nvidiactl`, request `_IOC(READ|WRITE, 'F', nr, size)`, nvrm.c:52,130-131)

| Escape | nr | Line(s) |
|---|---|---|
| NV_ESC_CARD_INFO | 200 (NV_IOCTL_BASE + 0) | 316 |
| NV_ESC_REGISTER_FD | 201 (NV_IOCTL_BASE + 1) | 237 |
| NV_ESC_RM_ALLOC | 0x2B | 154 (wrapper `rm_alloc`, 136) |
| NV_ESC_RM_FREE | 0x29 | 177 |
| NV_ESC_RM_CONTROL | 0x2A | 219 (wrapper `rm_control`) |
| NV_ESC_RM_MAP_MEMORY | 0x4E | 253 |
| NV_ESC_RM_MAP_MEMORY_DMA | 0x57 | 516 |
| NV_ESC_RM_UNMAP_MEMORY_DMA | 0x58 | 205 |

### 2.3 UVM ioctls (`/dev/nvidia-uvm`; on Linux `UVM_IOCTL_BASE(i)` is `i`, uvm_ioctl.h:40)

| Ioctl | Number | Line(s) |
|---|---|---|
| UVM_INITIALIZE | 0x30000001 | 308 |
| UVM_MM_INITIALIZE | 75 | 312 (failure tolerated) |
| UVM_REGISTER_GPU | 37 | 394 |
| UVM_REGISTER_GPU_VASPACE | 25 | 402 |
| UVM_CREATE_EXTERNAL_RANGE | 73 | 504 |
| UVM_MAP_EXTERNAL_ALLOCATION | 33 | 531 |
| UVM_FREE | 34 | 193 |
| UVM_REGISTER_CHANNEL | 27 | 759 |
| UVM_UNREGISTER_CHANNEL | 28 | 780 |
| UVM_UNREGISTER_GPU_VASPACE | 26 | 874 |
| UVM_UNREGISTER_GPU | 38 | 881 |

### 2.4 RM classes allocated (NV_ESC_RM_ALLOC)

| Class | Number | Parent | Line |
|---|---|---|---|
| NV01_ROOT_CLIENT | 0x41 | none | 304 |
| NV01_DEVICE_0 | 0x80 | root | 336 |
| NV20_SUBDEVICE_0 | 0x2080 | device | 339 |
| NV01_MEMORY_VIRTUAL | 0x70 | device | 343 |
| HOPPER_USERMODE_A | 0xC661 | subdevice | 362 (presence checked in class list, 356) |
| FERMI_VASPACE_A | 0x90F1 | device | 381 |
| NV01_MEMORY_SYSTEM | 0x3E | device | 587 |
| KEPLER_CHANNEL_GROUP_A | 0xA06C | device | 701 |
| FERMI_CONTEXT_SHARE_A | 0x9067 | channel group | 706 |
| BLACKWELL_CHANNEL_GPFIFO_A | 0xC96F | channel group | 726 (checked 355) |
| BLACKWELL_COMPUTE_B | 0xCEC0 | GPFIFO channel | 735 (checked 354) |

### 2.5 Control commands (NV_ESC_RM_CONTROL)

| Command | Number | Object | Line |
|---|---|---|---|
| NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2 | 0x205 | root | 329 |
| NV0080_CTRL_CMD_GPU_GET_CLASSLIST | 0x800201 | device | 348, 352 |
| NV2080_CTRL_CMD_GR_GET_INFO (SM version) | 0x20801201 | subdevice | 373 |
| NV2080_CTRL_CMD_GPU_GET_GID_INFO | 0x2080014A | subdevice | 387 |
| NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN | 0xC36F0108 | GPFIFO channel | 744 |
| NVA06C_CTRL_CMD_GPFIFO_SCHEDULE | 0xA06C0101 | channel group | 766 |

### 2.6 Mappings

- RM_MAP_MEMORY then `mmap(MAP_SHARED)` on the per-mapping fd: `map_to_cpu`, nvrm.c:242-262.
- USERMODE page (doorbell): 0x10000 bytes of HOPPER_USERMODE_A, nvrm.c:363.
- Buffers: a PROT_NONE anonymous reservation fixes the CPU address (nvrm.c:566), NV01_MEMORY_SYSTEM is allocated
  with GPU cacheability chosen by the caller (nvrm.c:580-587; uncached variant `nvrm_alloc_gpu_uncached`, nvrm.c:554),
  mapped to that address through `/dev/nvidiactl` (nvrm.c:592), then mapped for the GPU at the same address with
  RM_MAP_MEMORY_DMA + UVM_CREATE_EXTERNAL_RANGE + UVM_MAP_EXTERNAL_ALLOCATION (nvrm.c:504-531). The CPU and GPU see
  one address per buffer.
- GPFIFO ring and USERD share one GPU-uncached 0x10000 allocation; USERD sits after the ring (nvrm.c:712-723).
- Teardown: munmap and RM_FREE per buffer (`nvrm_free`, nvrm.c:624-680), then UVM unregister and close (nvrm.c:850-881).

### 2.7 Submit, doorbell, completion

- Submit (no system call): write the GP entry into the ring, `dsb sy`, store GPPut into USERD, `dsb sy`
  (`nvrm_enqueue`, nvrm.c:818-834; entry format `nvrm_gp_entry`, nvrm.c:808-812).
- Doorbell: `*rm->doorbell = rm->token` (nvrm.c:841), where `doorbell` = USERMODE page + NVC361_NOTIFY_CHANNEL_PENDING
  (0x90) (nvrm.c:366) and `token` comes from GET_WORK_SUBMIT_TOKEN (nvrm.c:743-745).
- Ring consumption: Blackwell USERD has no GPGet, so the caller retires entries after it sees completion
  (nvrm.c:824-826, `nvrm_retire` 836).
- Completion: polling only. A semaphore release writes a word in GPU-uncached memory; the CPU polls it with a memory
  barrier, a hard deadline and a stall deadline (docs/DRIVER.md:67). Implementation `m16_native_wait_marker`, a 50 us
  sleep-poll (src/omega_gpu_platform_linux.c:11, 310); the platform declares no interrupt wait (`.irq_wait = NULL`,
  src/omega_gpu_platform_linux.c:404; `OMEGA_GPU_SVC_IRQ` "polling never counts", src/omega_gpu_platform.h:102-103).
  Why uncached: GB10B keeps polled lines sticky in L2 (nvrm.c:552-553; docs/DRIVER.md:42).

### 2.8 Mapped onto the minimum driver surface

| Category | What the client uses today |
|---|---|
| Client | open `/dev/nvidiactl`; NV01_ROOT_CLIENT; UVM_INITIALIZE / UVM_MM_INITIALIZE; CARD_INFO; REGISTER_FD |
| Device | NV01_DEVICE_0, NV20_SUBDEVICE_0; controls GET_ID_INFO_V2, GET_CLASSLIST, GR_GET_INFO, GET_GID_INFO; UVM_REGISTER_GPU |
| Address space | FERMI_VASPACE_A, NV01_MEMORY_VIRTUAL; UVM_REGISTER_GPU_VASPACE |
| Memory | NV01_MEMORY_SYSTEM (cached or GPU-uncached); RM_MAP_MEMORY + mmap; RM_MAP_MEMORY_DMA / RM_UNMAP_MEMORY_DMA; UVM_CREATE_EXTERNAL_RANGE, UVM_MAP_EXTERNAL_ALLOCATION, UVM_FREE; RM_FREE |
| Command ring | KEPLER_CHANNEL_GROUP_A, FERMI_CONTEXT_SHARE_A, BLACKWELL_CHANNEL_GPFIFO_A with ring + USERD in one buffer; UVM_REGISTER_CHANNEL; GPFIFO_SCHEDULE |
| Compute object | BLACKWELL_COMPUTE_B on the channel |
| Doorbell | HOPPER_USERMODE_A page mmap; GET_WORK_SUBMIT_TOKEN; store at offset 0x90 |
| Completion | CPU poll of a GPU-uncached semaphore word; no interrupt |

On GB10 nvidia.ko forwards these RM_ALLOC/RM_CONTROL calls to GSP-RM as RPCs
(~/handoffs/2026-10-24-nvidia-driver-out-omega.md:13); aien-gpu has to send the same RPCs over the GSP message queue.

### 2.9 The Omega continuation

`native/nvrm/context.om` (1104 lines) is the Omega version of the same client: "Native continuation of canonical
nvrm/nvrm.c, source349913f1, NVIDIA ABI580.173.02. One explicitly owned context, bounded buffers, channel,
submission and completion" (context.om:2-3). It opens the same three files (`/dev/nvidiactl`, `/dev/nvidia0`,
`/dev/nvidia-uvm`, context.om:4-6). Its licence line is `AGPL-3.0-only` (context.om:1).

## 3. VFIO in user space versus a first-party kernel module

The 2026-10-24 plan (~/handoffs/2026-10-24-nvidia-driver-out-omega.md:17-28) proposed an Omega user-space driver over
Linux's own vfio-pci, "no NVIDIA kernel code, no C kernel module of ours": vfio container, group and device fds, BAR0
mmap, IOMMU_MAP_DMA, GSP boot ported to .om, RM operations as GSP RPCs, our page tables, channel and doorbell. Facts it
rested on: the GPU 000f:01:00.0 [10de:2e12] is alone in IOMMU group 20, has one 64 MiB BAR0 and no VRAM BAR, and the
kernel has CONFIG_VFIO_PCI=m (lines 9-10). Its own open risks: SMMU/ATS coherence under vfio, interrupts, reset between
sessions, and whether GSP-RM needs a WPR carve-out vfio cannot provide (line 20). Status: PROPOSED, never built.

**Design decision of this lane:** the direction is a first-party kernel module, aien-gpu.ko. The module gives
per-process isolation, cleanup on process death and a doorbell page per client; vfio gives one process the whole BAR0.
CUDA++ today relies on all three properties from nvidia.ko (one RM client per process, per-channel work-submit token on
a USERMODE page, section 2). This decision is reviewed in docs/DESIGN.md, written by OwnDriver.

## 4. Contradictions found between sources

1. nvrm.c size: the 2026-10-24 handoff says "physics nvrm/nvrm.c (1,017 lines)" (line 12); CUDApp 780a0af1 nvrm.c
   is 908 lines. Different repository and revision; this audit cites CUDApp 780a0af1 only.
2. Package count: the handoff says 143 nvidia/cuda dpkg packages (line 8); `dpkg -l` on 2026-10-26 counts 167
   installed (`ii` + `hi`), 24 of them on hold.
3. Boot path: L332-plan.md:16 names "FSP COT boot"; GPU_DRIVER.md:59-61 and the firmware inventory say GB10 has no FSP
   and SEC2 does the check. The plan text is superseded.
4. Licence: the 2026-10-21 handoff (line 100) says "logic stays AGPL in user space"; Drake's decision for this repo is
   `MIT OR GPL-2.0-only`. context.om is `AGPL-3.0-only` while nvrm.c is Apache-2.0 WITH LLVM-exception.
5. Module identity: the window-0 pre-check hashed `aien_gpu.ko` `da858c00...` (commit d5891389); the window-1 build
   receipt has `aee06949...` (89360c06 plus fixes). The window-0 checks-only PASS is not about the current module.
6. DMA width: /proc/driver/nvidia says 41 bits, dmesg said 40; the old module uses 40
   (~/handoffs/2026-10-21-own-gpu-driver.md:112).
7. Repository name: L332-plan.md:6 says the work is in aien-dev/aienos; the branch is now in aien-dev/aienux and the
   worktree path still says aienos.
8. The 2026-10-23 rung-0 attempt exists only as an untracked worktree directory, not on the branch or in PR #441.
9. VFIO plan "no C kernel module of ours" versus this lane's kernel module (section 3; a decision, not an error).
