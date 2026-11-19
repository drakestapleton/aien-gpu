# aien-gpu design

Ledger L332. Status words: PROPOSED, IMPLEMENTED, EXERCISED, QUALIFIED, MERGED, DEPLOYED (skill
`omega-communication`). Nothing in this repository has run on the GB10; every hardware result is
NOT_RUN until the window in `window/` runs. Current evidence per part: `docs/STATUS.md`.

## 1. What it replaces, and what it does not

aien-gpu.ko replaces `nvidia.ko` and `nvidia_uvm` on the compute path that CUDA++ and the Omega engine
use. It does not replace `nvidia_modeset` or `nvidia_drm`: the Spark runs headless for this work, and
when aien-gpu is bound the display modules are simply not loaded. The GPU still runs NVIDIA's signed
GSP-RM firmware; why that part cannot become ours is in `docs/BOUNDARY.md`.

The minimum surface comes from what the CUDA++ client exercises (`docs/AUDIT.md` section 2): a client,
a device, an address space, memory, a command ring, a compute object, a doorbell and completion.

```
CUDA++ / Omega engine  (src/omega_gpu_platform.h: typed handles, buffers, queues)
        |  second implementation of the same seam: omega_gpu_platform_aien.c (CUDApp PR)
/dev/aien-gpu  ABI 1 (kmod/aien_gpu_uapi.h): INFO, BOOT, ARENA_READ, RM_INIT, MEM_ALLOC/FREE,
        |                                       CHAN_CREATE/DESTROY, mmap of memory and doorbell
aien-gpu.ko   C glue: PCI, DMA, mmap, locks, barriers, BAR0 access, one register executor
        |     Omega units (om/*.om, run in the kernel): every decision, layout and check
BAR0 (64 MiB) + system memory through the SMMU
        |
GB10: SEC2 checks NVIDIA's signature, GSP-RM (NVIDIA firmware, on loan) runs the chip
```

## 2. Kernel module or VFIO

The 2026-10-24 plan proposed an Omega user-space driver over Linux's `vfio-pci`. This lane chose a
first-party kernel module:

- vfio gives one process the whole BAR0. A compute driver must give each client its own memory and
  address space, a doorbell page but nothing else of BAR0, and clean up a client's channels when the
  process dies. A kernel module does that with file descriptors and `release`; vfio would need a
  privileged daemon holding all of BAR0 plus a second protocol to the clients.
- The doorbell path stays a single user-space store, as with NVIDIA's driver: the module maps the
  64 KiB usermode page (BAR0 + 0xBB0000) into the client. No system call per launch.
- GSP-RM must be told when a client's objects go away (RM `FREE` in order). The module does that in
  `release`, even after a crash of the client.

## 3. ABI: new and small, not NVIDIA's ioctls

Two options were open: keep `NV_ESC_*` / `UVM_*` compatibility so the CUDA++ client runs unchanged, or
define a new ABI and update the client. Chosen: a new ABI plus a client update.

- NVIDIA's ioctl ABI is RM's object model (resource server, eleven classes, six controls, UVM's eleven
  ioctls) with parameter layouts tied to one driver version (580.173.02). Compatibility means
  re-implementing RM's resource server and UVM, two large subsystems, only to stay bound to NVIDIA's
  shapes.
- The client already has the seam for this: `src/omega_gpu_platform.h` in CUDApp is written so the
  runtime above it never sees a file descriptor or an NVIDIA handle, and `docs/DRIVER.md` there names
  L332 as its second implementation. The update is one new file implementing that seam
  (`omega_gpu_platform_aien.c`, CUDApp draft PR), not a change to CUDA++ itself.
- A small ABI is a small checked surface: every argument block has a fixed size, is copied into the
  kernel once, and is checked by an Omega unit before the kernel acts on it.
- A/B parity stays possible: the same CUDA++ build runs on either platform, and the m16 gate
  (`tools/aien-gpu-m16.c` over aien-gpu, CUDApp `m16/` over nvidia.ko) checks the same behaviour.

Inside the module, the messages to GSP-RM are NVIDIA's 580.173.02 RPC structures: GSP-RM is NVIDIA's
firmware and only speaks those. They are built and checked by Omega units from the MIT headers'
layouts (`docs/STAGE-C.md`).

## 4. Omega inside the kernel

Question asked: can the pinned Omega compiler emit a relocatable AArch64 object that links into a
kernel module? Answer, with evidence from this repository:

- `omc` (omega-mv main 3364a1df, sha256 befb8199..., and since the requalification omega-mv 91b3c932,
  sha256 189ada82...) has two outputs: a whole static Linux program
  (program mode) and, for a unit without `fn main`, bare function words (function mode, OMEGESE 9.1).
  It does not write an ELF relocatable object. It does not need to: function-mode words are
  position independent (only PC-relative branches; `const`, `global`, `text` and host words are refused
  in function mode, so there is no data and no system call), so they can be placed anywhere.
- `kmod/agpu_om.S` places each unit's words, byte for byte, in the module's `.text` with `.incbin`
  behind a `bti c` landing pad. The assembler produces the relocatable object; the kernel's module
  loader relocates only our own trampoline's two absolute addresses (the runtime table).
- Calling convention (OSC-1, omega-mv `docs/osc/OSC-1-DESIGN.md` section 7, checked by
  `seed/check/runner.c`): arguments in x0..x5 (an array argument is its address), result in x0, the
  runtime table in x7, a trap branches to the table entry at offset 16 with the code in x1 and never
  returns. This is AAPCS64 for six integer arguments plus one extra register; C reaches it through
  `agpu_om_enter` (`kmod/agpu_om.S`), which saves x19..x28, keeps the frame in x19, and on a trap
  restores the stack and returns the code. No setjmp, nothing crosses a C frame.
- Kernel ABI constraints, enforced per build by `tools/kcheck.om` (an Omega program, fail-closed
  allowlist of A64 classes from the Arm ARM DDI0487 M.d): registers x0..x17 and x30 only (x18 is the
  shadow call stack of this kernel, `CONFIG_SHADOW_CALL_STACK=y`; x19..x28 belong to the caller;
  x29 is the frame chain), no SIMD/FP instruction (no `kernel_neon_begin` needed), no system, `svc`,
  `brk`, pointer-authentication, pair, literal, `adr` or exclusive/atomic instruction, branches inside
  the unit, `br` only to the trap entry, SP moved only by immediate frame reservations, and the sum of
  all frames at most 4096 bytes (omc refuses call cycles in function mode, so no chain exceeds the
  sum). `tests/host/kcheck_controls.sh` assembles 36 control instructions with the system assembler
  and requires each verdict.
- Run, not just built: the module runs every unit at load (`agpu_selftest`); the QEMU cases boot the
  very kernel the Spark runs (7.0.0-1019-nvidia, shadow call stack and kernel pointer authentication
  on) and require `omega self-test PASS`, a contained trap (`selftest_trap=1`: code 3, then a normal
  call) and ten clean load/unload cycles. The host tests call the same object through the same
  trampoline.
- Not provided by Omega today, so in C, each with its successor:
  memory barriers (`dma_wmb`/`dma_rmb`/`dma_mb`) and MMIO (`ioread32`/`iowrite32`): function mode has
  no barrier or device-access word; proposal: barrier words and a bounded MMIO view for function mode,
  gated by kcheck-style tests (coordinated with agent OmegaNext; not silently added to the ABI);
  `const` in function mode is refused (error 9) although OMEGESE 9.12 says it composes everywhere: an
  omega-mv PR adds it (`docs/STATUS.md` names the PR); return-address signing: Omega frames store x30
  unsigned on the kernel stack where PAC-instrumented C would sign it; proposal: a kernel profile in
  omc that emits `paciasp`/`autiasp` (an extension, not used until it exists).

## 5. What is Omega and what is C

| Decision or layout | Omega unit | C glue does |
|---|---|---|
| Is this PCI function our GB10? (vendor, device, BAR0 size; BOOT_0, BOOT_42 arch 0x1a impl 0xb) | `om/ident.om` | read the PCI fields and two registers |
| Is an ioctl block acceptable? | `om/abi.om` (+ stage-C units) | copy in/out |
| The 12 GSP start-up register steps, their order, values and budgets | `om/boot.om` (a step machine) | perform one read, write or pause per call |
| A GSP "run CPU sequencer" request: check it whole, normalize it | `om/seq.om` | `kmod/agpu_exec.h`: perform the checked list |
| A command-queue message: headers, checksum, padding | `om/seal.om` | put the payload at byte 80 |
| Ring placement, wrap and full check | `om/mqput.om` | fence, publish the write pointer, notify |
| A status-queue message: shape, pointers, length, checksum, sequence, signature | `om/mqget.om` | read the write pointer with a fence, publish ours |
| Stage C payloads, reply checks, tokens | `om/rm*.om` (`docs/STAGE-C.md`) | RPC wait loop, DMA buffers |
| GPU page-table entries and walks | `om/gmmu*.om` | page-table pages, TLB invalidate registers |
| Kernel safety of the units themselves | `tools/kcheck.om` (build time) | none |

Recorded host dependencies (not Omega yet, each with its successor):

- The GSP boot package builder: aienux `native/kernel/dev/gb10_gsp_boot.c` at c9678d7b (C, L192),
  extended for GB20B by `tools/gb20b_gsp_boot.[ch]` (GB20B boot-loader geometry, COT version 2 with
  zero FRTS fields, and CrashCat queue at WprMeta 224/232; `_Static_assert`s pin the inherited
  133 MiB FW heap and 7 log buffers shared by GB10B|GB20B), linked into `tools/aien-gpu-boot` from
  the pinned clone; extractors from the same clone (`tools/package.sh`). Successor: a program-mode
  `.om` port (`omega-gpu-driver gsp/arena.om`) checked byte for byte against the C on the real
  package (`tests/host/plan_test.sh` already builds that package on the host).
- The test oracles (aienux C) are test-only and fetched by pin; nothing of them is in the module.

## 6. Stages and states

| Stage | ioctl | State after | What runs |
|---|---|---|---|
| A identity | probe (`allow_bind=1`) | BOUND | `om/ident.om`, BAR0 map, firmware size and sha256 pin, DMA arena and heap |
| B GSP start-up | `AGPU_BOOT` (root) | GSP_UP | `om/boot.om` on BAR0, then status queue until INIT_DONE with sequencer requests |
| C RM | `AGPU_RM_INIT` (root), then per client `MEM_ALLOC`, `CHAN_CREATE` | RM_READY | `docs/STAGE-C.md` |

One GSP start-up per module load; a failed stage leaves the state FAILED and the window script rolls
back. After the first register write the module never frees the arena or heap (GSP may still DMA into
them); the window reboots before any later load.

## 7. Security notes

- Binding needs `allow_bind=1`; the probe touches nothing before `om/ident.om` accepts the PCI facts.
- `AGPU_BOOT`, `AGPU_ARENA_READ`, `AGPU_RM_INIT` and the arena mapping need `CAP_SYS_ADMIN`; the
  arena mapping is refused after the start-up begins and every existing user mapping of it is revoked
  (`unmap_mapping_range`), because GSP-RM's boot structures and queues live there.
- Each open file is its own RM client with its own address space; handles are never shared.
- The device lock is never held across a user-memory fault (blocks are copied before and after).
- The doorbell page is shared by all clients, as with NVIDIA's driver; a token only rings a channel.
