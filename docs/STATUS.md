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
| 2026-11-18T23:09 | after `rmmod nvidia` (hot) | stage A PASS (self-test 7 units, bound, firmware pin matched); stage B `MMIO-STOPPED stage=7 polls=0,0,0,0 gsp_mb0=0x80000000`, 230 us | kernel log of that boot; board lines 2026-11-18T23:17:45Z |
| 2026-11-18T23:42 | cold boot, nvidia held back | stage A PASS; stage B `MMIO-STOPPED stage=7 polls=0,0,0,1 gsp_mb0=0xbadf4100`, 336 us; HWCFG2 also 0xbadf4100 | `~/handoffs/L332/window0-20261118T234217Z` |

Reading: 0x80000000 was the suspended marker nvidia.ko leaves at unload; 0xbadf41xx is the PRI
target-locked pattern (`privErrTargetLocked = 0xBADF4100U`, mask `0xFFFFFF00U`,
`kernel_sec2_gb10b.c:271-282` and `kern_fsp_gh100.c:1103-1113`), not an FMC code. Both runs finished the register steps without waiting: the
GB10B step 6 (SEC2 MAILBOX0 != 0xc001cafe) is true at once on this chip. What the chip does with a
correct GB20B start-up is NOT_RUN.

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

Decimal constants in `om/boot.om` and `om/rmarg.om`, each from `aien-calc eval` on spark-b87b
2026-11-18: 0x5e0 = 1504 (calc:fa011cc8f9b64e1068e838193ecf6657d0161c3a5aa262d7f4c027aa3cdd3b3b),
0x840c80 = 8653952 (calc:7475c332e5d22899f2c...), 0x840c84 = 8653956 (calc:e10da9821b34fb7d4f1...),
0x840ac0 = 8653504 (calc:1ac64d232498e1a23fd...), 0x840ac4 = 8653508 (calc:8a7592ee71dfe649b0d...),
2^25 = 33554432 (calc:6e5fcbff12200e9d5ef...), 2^30 = 1073741824 (calc:22d14a6beea9f3662a8...),
0x10de = 4318 (calc:c6bbdecb8bfb9558314...), 2^32 = 4294967296 (calc:73099441f520f1f7df4...).
Receipt ids after the first are prefixes; `aien-calc eval` of the same expression reprints each in full.

Hardware rules learned the same day: the chip must be cold (booted with nvidia held back) for our
start-up; a sysfs PCIe FLR on 000f:01:00.0 resets the whole Spark (2026-11-18T23:15:09Z); the way back
to nvidia.ko after our start-up is a clean restart.

| Part | Level | Evidence (command, verdict) |
|---|---|---|
| Omega units in the kernel (15 units, `om/`) | IMPLEMENTED, EXERCISED host + QEMU | `make om`: every unit `KCHECK PASS`; module self-test at load in QEMU |
| `tools/kcheck.om` (kernel-safety allowlist) | EXERCISED host | `tests/host/kcheck_controls.sh`: `KCHECK-CONTROLS PASS n=36` |
| Trampoline (`kmod/agpu_om.S`): traps, x19..x28 kept | EXERCISED host + QEMU | `UNIT PASS checks=92`; QEMU `selftest-trap PASS` (trap code 3 contained, call after it works) |
| Stage A: identity (GB20B only since 2026-11-18; GB10B refused, code 6), bind refusal | EXERCISED QEMU (no GB10 in the guest); bind and self-test also seen on the chip, see below | QEMU `absent PASS` (10 load/unload cycles), `wrongdev PASS` (pci-testdev refused by om/ident.om), `nobind PASS` |
| Stage B: 12 GSP start-up steps for GB20B (`om/boot.om`) | EXERCISED against the GB20B simulator in `tests/host` | `PARITY-BOOT PASS scenarios=3345`, every stage 0..11 and every reply refusal 1..7 reached; identical register traces with the C reference `tests/host/gb20b_mmio.c`; a start-up reported good made no access the driver never makes |
| Stage B: sequencer (`om/seq.om` + `kmod/agpu_exec.h`) | EXERCISED host | `PARITY-SEQ PASS valid=3000 malformed=2000` |
| Stage B: message queue (`om/seal`, `mqput`, `mqget`) | EXERCISED host | `PARITY-MQ PASS sends=586 recvs=40 refusals=374`, byte-identical command queue |
| Stage B: real GB20B boot package laid out, block accepted | EXERCISED host (NVIDIA's real 580.173.02 GB20B binary images & boot descriptor pinned in `tools/package.sh`; `system_info.txt` and `system_info.bin` sha256 recorded in `MANIFEST.txt`) | `PLAN-TEST PASS`: COT packet 868 bytes, version 2; GspFwWprMeta carries GB20B boot loader + CrashCat queue; `TEST-GB20B-BUILDER PASS` (5 direct builder size/offset controls); `COT-BOOT PASS` (version 1 / FRTS controls test simulator acceptance rule, `SIM-CHOICE`, real SEC2 `NOT_RUN`); independent cross-check vs `side/gb20b-full-assembly` `4dd4b3b`: all 9 package binaries, 868 B `cot_packet.bin` (`e0cfc641...`), and `system_info.sent.bin` byte-for-byte identical |
| Stage C: RM bring-up, objects, channel (`om/rm*.om`, `kmod/aien_gpu_rm.c`) | IMPLEMENTED, payloads EXERCISED host | `PARITY-RM PASS checks=20281` (bytes vs NVIDIA's own structs; work-submit token per the GB202 HAL GB20B takes); procedure and UNKNOWNs in `docs/STAGE-C.md` |
| Stage C: page tables and memory (`om/gmmu.om`, `memchk.om`, `rpdes.om`, `kmod/aien_gpu_mem.c`) | IMPLEMENTED, encoders EXERCISED host | `PARITY-GMMU PASS checks=102242` |
| Mutation testing of all units | EXERCISED host | `MUTANTS PASS killed=83/83` |
| Module build against 7.0.0-1019-nvidia | EXERCISED | `make kmod`, no warnings (pahole/BTF note only); `KO-UNITS PASS units=15 control=caught`: the module holds the units of this build (`tools/ko-units.sh`) |
| Static checks | EXERCISED | `tools/static-checks.sh`: checkpatch --strict clean (9 files); on spark-b87b 2026-11-18 no sparse binary, so `STATIC PASS-PARTIAL sparse NOT_RUN` |
| CUDA++ client over `/dev/aien-gpu` | IMPLEMENTED, host gate | aien-dev/CUDApp draft PR #97: `CUDA++ HOST TESTS PASS (36 targets)` |
| `tools/aien-gpu-m16` (m16 gate over our driver) | IMPLEMENTED | `--dry-run` ok; device runs NOT_RUN |
| Window script (`window/run.sh`) | `go` DISABLED 2026-11-18 (cold-boot rewrite pending); `check` mode exercised | `window/run.sh go` refuses before touching any device; `window/README.md` |
| Omega `const` in function mode | PROPOSED upstream | aien-dev/omega-mv draft #806 in OmegaNext's queue; units here use literals until it lands |

## Assumptions and `NOT_RUN` conditions (GB20B start-up)

- **Confidential Computing assumed OFF (`NOT_RUN` on chip):** `_ksec2GetGspUcodeArchive` (`kernel_sec2_gb20b.c:588-614`) selects `GspRmCcFmcGfwProdSigned` when CC is enabled; `gpuIsCCEnabledInHw` has not been measured on this Spark.
- **HFRP power-on (`gpuPowerOn_GB20B`, `kern_gpu_gb20b.c:197`):** not part of start-up (no non-generated caller found in 580.173.02; unmeasured on chip).
- **Step 0 poll budget:** `om/boot.om` shares poll budget `st[4]` between Step 0 (FWSEC scratch `0x5e0 == 0xff`) and Step 1 (SEC2 command queue empty); default `aien-gpu-boot` `50000 * 100 us = 5 s` (Wolfram verified) exceeds NVIDIA's 4 s `ksec2WaitForSecureBoot_GB20B` timeout, but the two waits do not have independent budgets.
- **MSGQ head/tail read count (`NOT_RUN` on chip):** `om/boot.om` reads `MSGQ_HEAD`/`MSGQ_TAIL` fewer times than NVIDIA's `ksec2PollForResponse` + `ksec2ReadPacket_GB20B` sequence; assumes those reads have no hardware side effects.
- **Simulator acceptance rule (`SIM-CHOICE`):** `tests/host/gb20b_sim.c` and `tests/host/cot_boot.c` reject COT version 1 and non-zero FRTS size at stage 11 per the GB20B HAL source reading; real SEC2 behavior on malformed or valid packets is `NOT_RUN`.
- **Math & formal checks (`HOST_TESTED` model only, hardware `NOT_RUN`):** all 32 hex-to-decimal register/mask constants, COT/WprMeta geometry, 32-bit MSGQ wraparound boundaries (`3, 4, 5, 19, 20, 1024`), and 5 s poll timeout verified via `WolframLanguageEvaluator` (2026-11-18); universal arithmetic/bitfield/protocol-ordering invariants (including 3 negative controls) machine-checked in Lean 4 `v4.35.0` (`~/workspace/lean-verify/LeanVerify/AienGpuGb20b.lean`, 19 theorems, zero `sorryAx`, zero `native_decide`).

What only the chip can settle is listed in `docs/STAGE-C.md` section 7 and `window/README.md`.
