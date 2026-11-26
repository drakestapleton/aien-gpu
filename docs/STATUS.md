# Status (2026-11-18)

Hardware: stage A has run on the chip; stage B has run twice and **stopped**; stage C and compute are
**NOT_RUN**. The GB20B start-up and package of this revision have **not run on the chip**: every verdict
in the table is a host or emulator result and qualifies nothing on the GB10. Labels per skill
`omega-communication`.

## The chip is GB20B, not GB10B (2026-11-18)

The product is the "GB10"; its identity registers read BOOT_0 0x1bb000a1, BOOT_42 0x1bba1000:
architecture 0x1b, implementation 0xb, which open-gpu-kernel-modules 580.173.02 dispatches as GB20B.
This module was written against the GB10B HALs. What happened on the chip (driver of 5112e27, GB10B
start-up and package):

| When (UTC) | State of the chip | Result | Evidence |
|---|---|---|---|
| 2026-11-18T23:09 | after `rmmod nvidia` (hot) | stage A PASS (self-test 7 units, bound, firmware pin matched); stage B `MMIO-STOPPED stage=7 polls=0,0,0,0 gsp_mb0=0x80000000`, 230 us | `docs/evidence/hot-20261118T2309Z-kernel.txt` (kernel log of that boot); `docs/evidence/hot-20261118T2317Z-board.txt` (the result as reported on the board; the tool's own output file was not kept) |
| 2026-11-18T23:42 | cold boot, nvidia held back | stage A PASS; stage B `MMIO-STOPPED stage=7 polls=0,0,0,1 gsp_mb0=0xbadf4100`, 336 us; HWCFG2 also 0xbadf4100 | `~/handoffs/L332/window0-20261118T234217Z` |

Reading: 0x80000000 was the suspended marker nvidia.ko leaves at unload; 0xbadf41xx is the PRI
target-locked pattern (`privErrTargetLocked = 0xBADF4100U`, mask `0xFFFFFF00U`,
`kernel_sec2_gb10b.c:271-282` and `kern_fsp_gh100.c:1103-1113`), not an FMC code. Both runs finished the register steps without waiting: the
GB10B step 6 (SEC2 MAILBOX0 != 0xc001cafe) is true at once on this chip. What the chip does with a
correct GB20B start-up is NOT_RUN.

MEASURED on this chip, from NVIDIA's own driver: when nvidia.ko was loaded again after the hot attempt
it logged what it sends SEC2 (`docs/evidence/hot-20261118T2309Z-kernel.txt`):
`ksec2SendBootCommands_GB20B: version=0x2, size=0x35c` with `frtsSysmemOffset=0x0, frtsSysmemSize=0x0,
frtsVidmemOffset=0x0, frtsVidmemSize=0x0`. So on this chip the driver takes the GB20B functions and its
COT payload is version 2, 860 bytes, FRTS fields zero: what this revision builds, and not what the
attempts of 2026-11-18 sent. The same log shows the hand-back failing: `ksec2ProcessNvdmMessage_GB20B:
Unknown or unsupported NVDM type received: 0x14`, `SEC2 boot cmds failed. RM cannot boot`, and one
minute later `ksec2PollForCanSend_IMPL: Timed out waiting for SEC2 command queue to be empty`. (0x14 is
the type of the COT command, not of a reply. That nvidia.ko read back what our attempt had left in the
channel is a reading of these lines, not something the log states.)

Differences from GB10B that touch this module, all read from source at tag 580.173.02 (HAL audit of
every `generated/g_*_nvoc.c`, 111 differing rows; `~/handoffs/L332/GB20B-PORT-PLAN-20261118.md`):

| What | GB10B | GB20B | Here |
|---|---|---|---|
| Secure-boot wait before the COT | none | PMC scratch 0x5e0 == 0xff (kernel_sec2_gb20b.c:541-575) | `om/boot.om` step 0 |
| After the COT doorbell | poll SEC2 MAILBOX0 | read the SEC2 reply from MSGQ 0x840c80/84 and check it (kernel_sec2_gb20b.c:481-537, 1034; kernel_sec2.c:496-523) | `om/boot.om` step 6 |
| COT payload | version 1, frtsSysmemSize = FMC size | version 2, FRTS fields 0 (g_kernel_sec2_nvoc.c:478-485) | `tools/gb20b_gsp_boot.c` |
| FMC, boot loader | `_GB10B` archives, 195912 / 196608 bytes | `_GB20B` archives, 212296 / 212992 bytes, boot-loader code at 0xf000 (g_kernel_gsp_nvoc.c:792-794, 928-930) | `tools/package.sh` pins |
| GSP-RM signature | `.fwsignature_gb10y` | `.fwsignature_gb20y` (nv-firmware-chip-family-select.h:64-68) | `tools/package.sh` |
| CrashCat queue in GspFwWprMeta | none | 4096 bytes of system memory (g_kernel_gsp_nvoc.c:1859-1866; kernel_gsp_gh100.c:366-372) | `tools/gb20b_gsp_boot.c`; whether GSP-RM needs it: NOT DETERMINED |
| Work-submit token | (runlist << 16) or ChID | also bit 30, RUNLIST_DOORBELL ENABLE (g_kernel_fifo_nvoc.c:647; gb202/dev_vm.h:27-32) | `om/rmarg.om` op 4, `om/rmchk.om` runlist bound 127 |
| Identity | architecture 0x1a | architecture 0x1b | `om/ident.om` accepts 0x1b only |
| DMA address width | 40 bits (`gpuarchGetDmaAddrWidth_GB10B`, `kern_gpu_arch_gb10b.c:32-42`, `41 - 1`, `gb10b/hwproject.h:27`) | 41 bits (`osinit.c:1525-1530`: default `gpuarchGetDmaAddrWidth_4a4dee`, `g_gpu_arch_nvoc.h:181-183` returns 0, falling back to `gpuarchGetSystemPhysAddrWidth_GB10B`, `g_gpu_arch_nvoc.c:146-149`, `kern_gpu_arch_gb10b.c:27-30` = 41) | unchanged at 40: stricter, limits only where our buffers are placed |

Same on both chips, unchanged here: the SEC2 command-queue send, the target-mask and lockdown waits,
the FMC error check, FALCON_OS, RISC-V active, GspFwWprMeta population, the message queues, and
GspSystemInfo (the one differing HAL feeding it, `kbifReadPcieCplCapsFromConfigSpace`, reads the
same PCIe DevCap2 atomic completer bits 7..9, measured clear on this Spark GPU in
`build/oracle/evidence/own-gpu-driver/rung2/system_info.txt:63-64`, GPU DevCap2 `0x00670813`).

The decimal forms of the register offsets, bit masks and sizes that `om/boot.om`, `om/rmarg.om` op 4 and
`tools/gb20b_gsp_boot.h` take from NVIDIA's headers in hexadecimal, and of the constants named in the notes
of `om/ident.om`, `om/rmarg.om` and `om/rmchk.om`, each recomputed with `aien-calc eval`
on spark-b87b 2026-11-19 (small counts and bounds such as 4, 20, 127, 1024 and 4095 are written as they
are in the source and have no receipt). The receipts are in `docs/calc-receipts/` (file name =
receipt id; `aien-calc verify calc:<id>` recomputes one from its own inputs):

| Expression | Value | Used as | Receipt (first 16 hex digits) |
|---|---|---|---|
| `0x5e0` | 1504 | NV_PMC_SCRATCH_RESET_PLUS_2 (boot step 0) | `fa011cc8f9b64e10` |
| `0xff` | 255 | secure boot done | `6cf4d9d16f069531` |
| `0x840ac0` | 8653504 | SEC2 EMEMC(0) | `1ac64d232498e1a2` |
| `0x840ac4` | 8653508 | SEC2 EMEMD(0) | `8a7592ee71dfe649` |
| `0x840c00` | 8653824 | SEC2 QUEUE_HEAD(0) | `13f17922dc1fb78a` |
| `0x840c04` | 8653828 | SEC2 QUEUE_TAIL(0) | `1bbe8e6bad1aad20` |
| `0x840c80` | 8653952 | SEC2 MSGQ_HEAD(0) | `7475c332e5d22899` |
| `0x840c84` | 8653956 | SEC2 MSGQ_TAIL(0) | `e10da9821b34fb7d` |
| `2^24` | 16777216 | EMEMC auto-increment on write | `64d152d1ce0358e4` |
| `2^25` | 33554432 | EMEMC auto-increment on read | `6e5fcbff12200e9d` |
| `2^32` | 4294967296 | 32-bit wrap of the reply size | `73099441f520f1f7` |
| `0x7e` | 126 | MCTP message type, vendor defined PCI | `a9aa42a2e19d0d61` |
| `0x10de` | 4318 | NVIDIA vendor id | `c6bbdecb8bfb9558` |
| `0x15` | 21 | NVDM type SEC2_RESPONSE | `e0f0497170ae08f0` |
| `0x18` | 24 | NVDM type SMBPBI | `38d2bcce14436908` |
| `0xbadf4100` | 3135193344 | PRI target-locked pattern | `9b75f8bde8b6d082` |
| `0xffffff00` | 4294967040 | mask of that pattern | `fbebed9782519fb6` |
| `0x110040` | 1114176 | GSP MAILBOX0 | `21fb2f484ec67edf` |
| `0x110044` | 1114180 | GSP MAILBOX1 | `3afe539e94869002` |
| `0x110080` | 1114240 | GSP FALCON_OS | `aa97c8d7a41d4ed8` |
| `0x1100f4` | 1114356 | GSP HWCFG2 | `20ae331c51997671` |
| `0x111388` | 1119112 | RISC-V CPUCTL | `5000ede2622d0ae4` |
| `2^30` | 1073741824 | doorbell RUNLIST_DOORBELL ENABLE (om/rmarg.om op 4) | `22d14a6beea9f366` |
| `0x34000` | 212992 | GspRmBoot_GB20B size | `34f2075f44dc3087` |
| `0xf000` | 61440 | GspRmBoot_GB20B monitor code offset | `5d52fca0ae63d06b` |
| `133 * 2^20` | 139460608 | GSP firmware heap size | `376b58881c83da9d` |
| `2^13` | 8192 | HWCFG2 RISCV_BR_PRIV_LOCKDOWN (bit 13) | `495ae977dd37c9a5` |
| `0xffff` | 65535 | 16-bit vendor id field | `57e7fe4d9a051797` |
| `0xffffffff` | 4294967295 | 32-bit mask of the reply size and of the low address half | `9411575d6f2fb86e` |
| `0x2e12` | 11794 | PCI device id of the GPU (om/ident.om) | `03f00f82240ea1d6` |
| `64 * 2^20` | 67108864 | 64 MiB, the least BAR0 size (om/ident.om) and a size bound (om/rmchk.om) | `68983794d3185375` |
| `0x1a` | 26 | architecture GB100 (om/ident.om) | `6ec32e28c46c6852` |
| `0x1b` | 27 | architecture GB200 (om/ident.om) | `4a2765f9cf31e125` |
| `0xb` | 11 | implementation B (om/ident.om) | `20a938b7a4c8664d` |
| `0x3f` | 63 | architecture field mask (om/ident.om) | `7af13bfffba51145` |
| `2^48` | 281474976710656 | address bound (om/rmarg.om) | `76b45cc2782f8650` |
| `2^31` | 2147483648 | suspended marker 0x80000000 (om/rmarg.om) | `20583828dd298176` |
| `0xcec0` | 52928 | class BLACKWELL_COMPUTE_B (om/rmchk.om) | `020591fe10598db7` |
| `0xcdc0` | 52672 | class BLACKWELL_COMPUTE_A (om/rmchk.om) | `063e2656f90c6570` |
| `0xca6f` | 51823 | class BLACKWELL_CHANNEL_GPFIFO_B (om/rmchk.om) | `5822a7a80c610b79` |
| `0xc96f` | 51567 | class BLACKWELL_CHANNEL_GPFIFO_A (om/rmchk.om) | `f7e87a0de6e7ac91` |
| `2^21` | 2097152 | 2 MiB (om/rmchk.om) | `6b2b2c7717c378e1` |
| `50000 * 100 / 10^6` | 5 | seconds in aien-gpu-boot's default wait: 50000 polls 100 us apart | `6f24577e0be828c0` |

What that day showed about handing the chip between drivers, each seen once: a start-up attempted
after `rmmod nvidia` (with the GB10B sequence) stopped at once; nvidia.ko then did not re-initialise
the chip (log above); a sysfs PCIe FLR on 000f:01:00.0 reset the whole Spark (2026-11-18T23:15:09Z,
`docs/evidence/hot-20261118T2317Z-board.txt`). Whether a GB20B start-up could begin after `rmmod
nvidia` is NOT_RUN and is not planned: this module is run on a cold chip only (booted with nvidia held
back), and the way back to nvidia.ko is a clean restart.

| Part | Level | Evidence (command, verdict) |
|---|---|---|
| Omega units in the kernel (15 units, `om/`) | IMPLEMENTED, EXERCISED host + QEMU | `make om`: every unit `KCHECK PASS`; module self-test at load in QEMU |
| `tools/kcheck.om` (kernel-safety allowlist) | EXERCISED host | `tests/host/kcheck_controls.sh`: `KCHECK-CONTROLS PASS n=36` |
| Trampoline (`kmod/agpu_om.S`): traps, x19..x28 kept | EXERCISED host + QEMU | `UNIT PASS checks=96`; QEMU `selftest-trap PASS` (trap code 3 contained, call after it works) |
| Stage A: identity (GB20B only since 2026-11-18; GB10B refused, code 6), bind refusal | EXERCISED QEMU (no GB10 in the guest); bind and self-test also seen on the chip, see below | QEMU `absent PASS` (10 load/unload cycles), `wrongdev PASS` (pci-testdev refused by om/ident.om), `nobind PASS` |
| Stage B: 12 GSP start-up steps for GB20B (`om/boot.om`) | EXERCISED against the GB20B simulator in `tests/host` | `PARITY-BOOT PASS scenarios=7745` (44 kinds of fault, 11 packet lengths, 8 sets of poll budgets, each with a refused and an accepted packet), every stage 0..11 and every reply refusal 1..7 reached; identical register traces with the C reference `tests/host/gb20b_mmio.c`; a start-up reported good made no access the driver never makes |
| Stage B: sequencer (`om/seq.om` + `kmod/agpu_exec.h`) | EXERCISED host | `PARITY-SEQ PASS valid=3000 malformed=2000` |
| Stage B: message queue (`om/seal.om`, `om/mqput.om`, `om/mqget.om`) | EXERCISED host | `PARITY-MQ PASS sends=586 recvs=40 refusals=374`, byte-identical command queue |
| Stage B: real GB20B boot package laid out, block accepted | EXERCISED host (NVIDIA's real 580.173.02 GB20B images, the boot loader's descriptor and the system information, as text and as encoded, each pinned in `tools/package.sh`) | `PLAN-TEST PASS`: COT packet 868 bytes, version 2; GspFwWprMeta carries GB20B boot loader + CrashCat queue; `TEST-GB20B-BUILDER PASS` (5 direct builder size/offset controls); a changed system information text is refused by its pin; `COT-BOOT PASS` (version 1 / FRTS controls test simulator acceptance rule, `SIM-CHOICE`, real SEC2 `NOT_RUN`); second builder: `gb20b_boot.c` under `tools/` on the branch `side/gb20b-full-assembly` (`4dd4b3b`, in this repository), written separately by another lane, lays out the same package: the 8 package binaries, the 868-byte `cot_packet.bin` (sha256 `e0cfc64158339def33345b551c4d528586f5d7045b0d028995b4e094f1b3a901`) and `system_info.sent.bin` are byte-identical; the 128-byte boot block differs in two bytes (the block version, 2 here, and the COT offset: the packet sits one page later here, after the CrashCat page); that builder has no CrashCat queue and writes no `wpr_meta.bin`, so GspFwWprMeta is not cross-checked. Reproduced 2026-11-19: `~/handoffs/L332/xcheck-side-4dd4b3b-20261119.txt` |
| Stage C: RM bring-up, objects, channel (`om/rm*.om`, `kmod/aien_gpu_rm.c`) | IMPLEMENTED, payloads EXERCISED host | `PARITY-RM PASS checks=20281` (bytes vs NVIDIA's own structs; work-submit token per the GB202 HAL GB20B takes); procedure and UNKNOWNs in `docs/STAGE-C.md` |
| Stage C: page tables and memory (`om/gmmu.om`, `memchk.om`, `rpdes.om`, `kmod/aien_gpu_mem.c`) | IMPLEMENTED, encoders EXERCISED host | `PARITY-GMMU PASS checks=102242` |
| Mutation testing of all units | EXERCISED host | `MUTANTS PASS killed=86/86` |
| Off-by-one mutants, generated (`tests/host/boundary.sh`) | EXERCISED host | `BOUNDARY PASS units= boot ident mutants=114 killed=110 equivalent=4`: every literal on every comparison line of `om/boot.om` and `om/ident.om`, plus one and minus one; the 4 no test can see are in `tests/host/boundary_equivalent.txt` with their reasons. Not yet in this gate: `om/abi.om`, `om/rmarg.om`, `om/rmchk.om` (59 survivors, on lines this change did not write; `REVIEW-LESSONS.md`) |
| NVIDIA source the gates read (outside the repository) | pinned | `NVSRC PASS files=58 sha256=f0ba8567...0389` (`tools/nvsrc-pin.sh`): the 56 headers `tests/host/parity_rm.c` opens and the two GB20B bindata sources |
| Claims a machine can check (`tools/claims-lint.sh`) | EXERCISED host | `CLAIMS PASS`: quoted paths exist, named receipts and evidence files are there and intact, and a new line that names an outside calculator as the source of a number fails (the 11 older ones are listed in `tools/claims-baseline.txt`); `CLAIMS-SELFTEST PASS controls=4 caught=4` |
| Module build against 7.0.0-1019-nvidia | EXERCISED | `make kmod`, no warnings (pahole/BTF note only); `KO-UNITS PASS units=15 control=caught`: the module holds the units of this build (`tools/ko-units.sh`) |
| Static checks | EXERCISED | `tools/static-checks.sh`: checkpatch --strict clean (9 files); on spark-b87b 2026-11-18 no sparse binary, so `STATIC PASS-PARTIAL sparse NOT_RUN` |
| CUDA++ client over `/dev/aien-gpu` | IMPLEMENTED, host gate | aien-dev/CUDApp draft PR #97: `CUDA++ HOST TESTS PASS (36 targets)` |
| `tools/aien-gpu-m16.c` (m16 gate over our driver) | IMPLEMENTED | `--dry-run` ok; device runs NOT_RUN |
| Window script (`window/run.sh`) | `go` DISABLED 2026-11-18 (cold-boot rewrite pending); `check` mode exercised | `window/run.sh go` refuses before touching any device; `window/README.md` |
| Omega `const` in function mode | PROPOSED upstream | aien-dev/omega-mv draft #806 in OmegaNext's queue; units here use literals until it lands |

## Assumptions and `NOT_RUN` conditions (GB20B start-up)

- **Confidential Computing assumed OFF (`NOT_RUN` on chip):** `_ksec2GetGspUcodeArchive` (`kernel_sec2_gb20b.c:578-614`) selects `GspRmCcFmcGfwProdSigned` when CC is enabled; `gpuIsCCEnabledInHw` has not been measured on this Spark.
- **HFRP power-on (`gpuPowerOn_GB20B`, `kern_gpu_gb20b.c:197`):** not part of start-up (no non-generated caller found in 580.173.02; unmeasured on chip).
- **Step 0 poll budget:** `om/boot.om` shares poll budget `st[4]` between Step 0 (FWSEC scratch `0x5e0 == 0xff`) and Step 1 (SEC2 command queue empty); default `aien-gpu-boot` `50000 * 100 us = 5 s` (receipt `50000 * 100 / 10^6` in the table above) exceeds NVIDIA's 4 s `ksec2WaitForSecureBoot_GB20B` timeout, but the two waits do not have independent budgets.
- **MSGQ head/tail read count (`NOT_RUN` on chip):** `om/boot.om` reads `MSGQ_HEAD`/`MSGQ_TAIL` fewer times than NVIDIA's `ksec2PollForResponse` + `ksec2ReadPacket_GB20B` sequence; assumes those reads have no hardware side effects.
- **One reading of the driver:** `om/boot.om`, the C reference `tests/host/gb20b_mmio.c` and the simulator `tests/host/gb20b_sim.c` were written by one lane from one reading of NVIDIA's source. `PARITY-BOOT` shows the Omega equals that C; it does not show the C equals the driver. Two reviewers read the driver again and found no difference (`~/handoffs/L332/review-4955ab8-r1.md`, `review-3a45c36-r3.md`), and the driver's own log on this chip agrees on the COT payload (above). The order of the register accesses of steps 0 and 6 has no second source: a trace of NVIDIA's own `ksec2*` functions against a register stub would be one, and is not built.
- **How long the ioctl may spin (unchanged by this revision):** `kmod/aien_gpu_boot.c` waits between polls with `udelay` and calls `cond_resched`; `om/abi.om` admits budgets up to 10000000 polls and pauses up to 1000 us, so a caller can ask for hours. `aien-gpu-boot` asks for 5 s per wait.
- **Simulator acceptance rule (`SIM-CHOICE`):** `tests/host/gb20b_sim.c` and `tests/host/cot_boot.c` reject COT version 1 and non-zero FRTS size at stage 11 per the GB20B HAL source reading; real SEC2 behavior on malformed or valid packets is `NOT_RUN`.
- **Arithmetic and model checks (model only, hardware `NOT_RUN`):** the decimal constants are recomputed with `aien-calc`, receipts in `docs/calc-receipts/` (table above). `docs/proofs/AienGpuGb20b.lean` holds 22 Lean 4 theorems (3 negative controls) about a hand-written model of the reply size, the EMEM pointer, the work-submit token and the order of the steps; `lean docs/proofs/AienGpuGb20b.lean` (Lean 4.35.0, no imports) checks them, see `docs/proofs/README.md`. The model is not derived from the code, so these proofs do not fail when the code changes. The lane that wrote `39a2f15` also reported a Wolfram check of 32 constants; its inputs and results were not kept, so it is not claimed here.
- **Interface versions:** `AGPU_ABI_VERSION` stays 1 (no request of a compute client changed; the CUDA++ client refuses any other value). The changed `struct agpu_boot_out` is versioned by the AGPU_BOOT block itself: `AGPU_BOOT_VERSION` 2, checked by `om/abi.om`; a version 1 block (the GB10B layout) is refused with code 2 (`UNIT` test, two mutants, module self-test). `39a2f15` had raised `AGPU_ABI_VERSION` to 2; this revision takes that back.
- **The C oracles:** aien-dev/aienux `feat/own-gpu-driver` at `c9678d7b` is kept in this repository as the branch `oracle/aienux-c9678d7b`. `~/git/aienux.git` on spark-b87b does not hold that commit (checked 2026-11-19), which is why `39a2f15` found it only in another lane's work folder.

What only the chip can settle is listed in `docs/STAGE-C.md` section 7 and `window/README.md`.
