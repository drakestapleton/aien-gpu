# aien-gpu

AIEN's own kernel driver for the GB10 GPU in the DGX Spark, replacing NVIDIA's `nvidia.ko` and
`nvidia_uvm` on the compute path of CUDA++ and the Omega engine. Part of Sovereign Intelligence,
built on AienOS (ledger L332).

**Status: IMPLEMENTED. Host and QEMU tests pass. Hardware NOT_RUN.** Nothing here has run on the
GB10 yet. The window that will run it is in `window/`. Per-part evidence is in `docs/STATUS.md`.

## What it is

- A Linux kernel module, `kmod/aien_gpu.ko`. The C in it is Linux glue: PCI, DMA, mmap, locks,
  memory barriers and register access.
- Every decision the driver makes is Omega (`om/*.om`): identifying the chip, checking every ioctl,
  the GSP start-up steps, the sequencer, the message queues, RPC payloads and GPU page tables.
  The pinned Omega compiler turns these into machine words. The module runs those words inside the
  kernel (`docs/DESIGN.md` section 4).
- `tools/kcheck.om` checks every build's words for kernel safety: registers, no FP or SIMD, no
  system instructions and a stack bound.
- A small ABI of our own on `/dev/aien-gpu` (`kmod/aien_gpu_uapi.h`). CUDA++ reaches it through a
  second implementation of its existing platform seam.

## What it does not replace

The GPU still boots NVIDIA's signed GSP firmware. The chip checks that signature, and we do not
hold the key. The machine also still runs NVIDIA's DGX OS. `docs/BOUNDARY.md` covers both, along
with the disclaimer this work can earn:

> No NVIDIA driver or CUDA code between my program and the GPU. The machine still runs NVIDIA's DGX
> OS, and the GPU loads NVIDIA's signed firmware.

That line becomes true only after a hardware receipt. Until then, CUDA++ runs on NVIDIA's open
kernel module 580.173.02.

## Build and test (host and QEMU only; never loads anything on the host)

```sh
make OMC=/path/to/omega-mv/seed/out/omc check   # om, kcheck, host tests, mutants, plan test, module, static, QEMU
```

`OMC` must be the seeded compiler with sha256
`189ada82e00bb440cb41351bd252cc68c54a1b1050051cc42a1df700081c3b54` (omega-mv main `91b3c932`; requalified
2026-10-26: every unit `KCHECK PASS`, all host gates, mutants 57/57, module build, QEMU cases).
The test oracles are the earlier C of this work, aien-dev/aienux `feat/own-gpu-driver` at `c9678d7b`, kept in this repository as the branch
`oracle/aienux-c9678d7b` (the Makefile checks the commit id).

## Licence

`MIT OR GPL-2.0-only` (`LICENSE`). The rest of the project is Apache-2.0 WITH LLVM-exception, and
this repo differs for one reason. Linux only links a module whose licence it accepts, and it gives
the GPL-only kernel functions a driver needs (PCI, DMA, firmware loading) only to GPL-compatible
modules. Apache-2.0 is not compatible with GPL-2.0. So the module, and the Omega units linked into
it, are dual MIT/GPL, the same as NVIDIA's open kernel module.

## Map

| Path | What |
|---|---|
| `docs/AUDIT.md` | earlier L332 work, receipts, and the CUDA++ client's exact NVIDIA surface |
| `docs/BOUNDARY.md` | what can be ours and what cannot; NVIDIA software DGX OS runs regardless; the disclaimer |
| `docs/DESIGN.md` | module or VFIO, the ABI choice, Omega in the kernel, the Omega/C split |
| `docs/STAGE-C.md` | the RM bring-up after GSP-RM is up, with NVIDIA source citations |
| `docs/STATUS.md` | evidence level of every part |
| `docs/calc-receipts/` | `aien-calc` receipts of the decimal constants in the Omega units |
| `docs/proofs/` | Lean 4 proofs about a hand-written model of the GB20B start-up |
| `window/` | the hardware window: ordered script, rollback, timing, root commands |
