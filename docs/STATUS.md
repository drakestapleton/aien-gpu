# Status (2026-10-26)

Hardware: **NOT_RUN** for every part. The verdicts below are host or emulator results; none of them
qualifies anything on the GB10. Labels per skill `omega-communication`.

| Part | Level | Evidence (command, verdict) |
|---|---|---|
| Omega units in the kernel (15 units, `om/`) | IMPLEMENTED, EXERCISED host + QEMU | `make om`: every unit `KCHECK PASS`; module self-test at load in QEMU |
| `tools/kcheck.om` (kernel-safety allowlist) | EXERCISED host | `tests/host/kcheck_controls.sh`: `KCHECK-CONTROLS PASS n=36` |
| Trampoline (`kmod/agpu_om.S`): traps, x19..x28 kept | EXERCISED host + QEMU | `UNIT PASS checks=82`; QEMU `selftest-trap PASS` (trap code 3 contained, call after it works) |
| Stage A: identity, bind refusal | EXERCISED QEMU (no GB10 in the guest) | QEMU `absent PASS` (10 load/unload cycles), `wrongdev PASS` (pci-testdev refused by om/ident.om), `nobind PASS` |
| Stage B: 11 GSP start-up steps (`om/boot.om`) | EXERCISED against the aienux simulator | `PARITY-BOOT PASS scenarios=1056`, every stage 0..8 reached; identical register traces with the C oracle |
| Stage B: sequencer (`om/seq.om` + `kmod/agpu_exec.h`) | EXERCISED host | `PARITY-SEQ PASS valid=3000 malformed=2000` |
| Stage B: message queue (`om/seal`, `mqput`, `mqget`) | EXERCISED host | `PARITY-MQ PASS sends=586 recvs=40 refusals=374`, byte-identical command queue |
| Stage B: real boot package laid out, block accepted | EXERCISED host (NVIDIA's real 580.173.02 images) | `PLAN-TEST PASS`, COT packet 868 bytes, negative control refused (code 6) |
| Stage C: RM bring-up, objects, channel (`om/rm*.om`, `kmod/aien_gpu_rm.c`) | IMPLEMENTED, payloads EXERCISED host | `PARITY-RM PASS checks=20276` (bytes vs NVIDIA's own structs); procedure and UNKNOWNs in `docs/STAGE-C.md` |
| Stage C: page tables and memory (`om/gmmu.om`, `memchk.om`, `rpdes.om`, `kmod/aien_gpu_mem.c`) | IMPLEMENTED, encoders EXERCISED host | `PARITY-GMMU PASS checks=102242` |
| Mutation testing of all units | EXERCISED host | `MUTANTS PASS killed=57/57` |
| Module build against 7.0.0-1019-nvidia | EXERCISED | `make kmod`, no warnings (pahole/BTF note only) |
| Static checks | EXERCISED | `tools/static-checks.sh`: checkpatch --strict clean (9 files), sparse clean, `STATIC PASS` |
| CUDA++ client over `/dev/aien-gpu` | IMPLEMENTED, host gate | aien-dev/CUDApp draft PR #97: `CUDA++ HOST TESTS PASS (36 targets)` |
| `tools/aien-gpu-m16` (m16 gate over our driver) | IMPLEMENTED | `--dry-run` ok; device runs NOT_RUN |
| Window script (`window/run.sh`) | IMPLEMENTED, `check` mode exercised | needs Drake's authorization; `window/README.md` |
| Omega `const` in function mode | PROPOSED upstream | aien-dev/omega-mv draft #806 in OmegaNext's queue; units here use literals until it lands |

What only the chip can settle is listed in `docs/STAGE-C.md` section 7 and `window/README.md`.
