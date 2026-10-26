# What aien-gpu replaces, what stays NVIDIA's, and what we may say

Written 2026-10-26. Every claim names its source. Status words follow omega-communication: PROPOSED, IMPLEMENTED,
EXERCISED, QUALIFIED, MERGED, DEPLOYED. NOT_RUN is never a pass. "DOCS SILENT on X" means no source we hold says.

## 1. What becomes ours

aien-gpu.ko replaces nvidia.ko, nvidia_uvm, nvidia_modeset and nvidia_drm on the compute path. The CPU-side pieces:

| Piece | What it covers | Prior L332 work it builds on (aienux@c9678d7b) |
|---|---|---|
| The kernel module | PCI probe, BAR0, firmware request, per-process device file, mmap of buffers and the doorbell page, cleanup when a process dies | `native/linux/aien-gpu/` (built, never loaded; evidence/own-gpu-driver/rung2/build-receipts-20261022T0108Z.txt) |
| Memory management | GPU page tables and DMA mappings of system memory (GB10 has no VRAM BAR; ~/handoffs/2026-10-24-nvidia-driver-out-omega.md:9) | `native/kernel/gpu/gmmu.c`, `gpu_mem.c`; L193 MMU simulator |
| Channel setup | channel group, GPFIFO ring, USERD, compute object, work-submit token | `native/kernel/gpu/gpu_chan.c`, contract `gb10_chan.devc` |
| Interrupt path | polling first; CUDA++ already completes by polling (docs/AUDIT.md 2.7) | none on chip |
| RPC transport to GSP | the message queue nvidia.ko uses to forward RM_ALLOC and RM_CONTROL to GSP-RM | `native/kernel/gpu/gsp_mmio.c`, contract `gb10_gsp.devc`; window-1 tool |
| CPU sequencer | NVIDIA's `kgspExecuteSequencerBuffer` chores, rewritten | `native/kernel/gpu/gsp_seq.c` (docs/GPU_DRIVER.md:97-106) |

In this repository the decision logic is Omega (`om/`), compiled by the pinned seeded compiler and entered from the
module through `kmod/agpu_om.S`. Status today: PROPOSED to IMPLEMENTED depending on the piece; nothing has run on the
chip (docs/AUDIT.md 1.1).

## 2. What cannot become ours, and why

**Signed firmware is a hardware root of trust we cannot replace.** The GPU runs only programs NVIDIA signed, a security
block on the chip checks the signature before it starts them, and we do not have NVIDIA's private key.

| Item | Fact | Source |
|---|---|---|
| GSP-RM firmware `gsp_ga10x.bin` | `/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin`, 75,012,080 bytes, sha256 `32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963`. The chip's operating program; on GB10 it does the work nvidia.ko forwards as RPCs | GB10_FIRMWARE_INVENTORY.md:78 (aienux@c9678d7b); `ls -l` and `sha256sum` run 2026-10-26 give the same size and hash |
| Which file GB10 loads | GB10X/GB10Y families `return NV_FIRMWARE_FOR_NAME("gsp_ga10x")`; nvidia.ko declares `firmware: nvidia/580.173.02/gsp_ga10x.bin` | inventory:67,96 (nv-firmware.h:95-102, modinfo) |
| GSP boot loader and GSP-FMC | Compiled into nvidia.ko's source as C byte arrays ("bindata") under an MIT header: `g_bindata_kgspGetBinArchiveGspRmBoot_GB10B.c` and `g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_GB10B.c`. The source licence is MIT; the bytes are NVIDIA-signed binaries we can copy but not change | inventory:103-109,141-143 |
| FMC image, hash, signature, public key | FMC image 195,912 bytes (sha256 `b13ead4b...1f2e`); SHA-384 hash 48 bytes (`f0389f3a...500c`); signature 384 bytes (`9db42925...7272`); public key 384 bytes (`e9bbc669...7816`). Boot image PROD 196,608 bytes (`427a691d...00a2`) | inventory:111-120 (digests measured 2026-10-12 by `ck_gsp_bindata_extract`) |
| SEC2 checks the signature | The driver copies the FMC to system memory and puts the hash, signature and public key into an `NVDM_PAYLOAD_COT` sent to SEC2 (868 bytes in one message per GPU_DRIVER.md:65-67); GB10 has no FSP, SEC2 does the check | inventory:192-196,221-227 (kernel_sec2_gb10b.c:438-535, kernel_gsp_gh100.c:803-872); GPU_DRIVER.md:59-60 |
| A SEC2 refusal is final | "A SEC2 refusal is final and is reported, never bypassed" | inventory:290 |
| Chip root of trust | Hot Chips 2025 GB10 deck, p8: "Dual Secure Root support. SROOT Processor: NVIDIA Root for Secure Boot and NVIDIA secrets management. OSROOT Processor: Authentication and measurement of UEFI & OS & other System SW components" | `doc-page hc2025-nvidia-gb10-skende 8` |
| No private key | We hold only the public key the driver ships; the matching private key is NVIDIA's | GPU_DRIVER.md:60-61 ("we do not have NVIDIA's signing key") |
| SEC2's own firmware and boot ROM | Not loaded by the driver on GB10B; who loads SEC2 firmware before Linux (SoC boot chain, UEFI) is UNKNOWN | inventory:173-176 |
| SoC firmware | UEFI and the rest of the SoC boot chain run before Linux and stay NVIDIA's; the state UEFI leaves SEC2 and the GPU in is UNKNOWN | inventory:54-56, 338-340 |

DOCS SILENT on: whether SROOT is the key that signs or verifies the GSP-FMC chain (the deck names SROOT for secure boot
and secrets; the driver source shows only SEC2 receiving the signature); the SEC2 command protocol beyond what the
driver source shows; the GSP-RM message protocol after FMC starts it (inventory:54-55).

If evidence ever shows another way to satisfy the check, it becomes an open question with an experiment; nothing is
tried that could damage the chip (GPU_DRIVER.md:69-71).

## 3. NVIDIA host software DGX OS runs regardless of the GPU driver

Source: `systemctl list-unit-files`, `systemctl is-active` and `systemctl cat` run read-only on 2026-10-26. Listed: every
unit whose name contains "nv" and that NVIDIA ships (nvidia-*, nv-*, nvsm*); nvme*/nvmf* and openvpn* matched the grep
but are not NVIDIA's and are left out. "inactive" on an enabled unit means it is not running now; most are one-shot
configuration scripts by their ExecStart.

| Unit | State | Active now | Purpose (Description / ExecStart) |
|---|---|---|---|
| nvidia-persistenced.service | enabled | active | "NVIDIA Persistence Daemon"; a drop-in `/etc/systemd/system/nvidia-persistenced.service.d/nv-persistence-override.conf` replaces ExecStart with `nvidia-persistenced --persistence-mode --verbose` and WantedBy=basic.target |
| nvidia-cdi-refresh.path | enabled | active | "Trigger CDI refresh on NVIDIA driver or toolkit install / upgrade events" |
| nvidia-cdi-refresh.service | enabled | inactive | "Refresh NVIDIA CDI specification file"; `nvidia-smi -L`, `nvidia-ctk cdi generate` |
| nvidia-nvme-interrupt-coalescing.service | enabled | active | "Configure NVMe Interrupt Coalescing" |
| nv-cpu-governor.service | enabled | inactive | "Set the CPU Frequency Scaling governor"; `cpupower frequency-set -g performance` |
| nv-docker-gpus.service | enabled | inactive | "Configure NVIDIA Docker to only use Compute GPUs" |
| nvidia-conf-xconfig.service | enabled | inactive | "NVIDIA Xconfig service" |
| nvidia-console-settings.service | enabled | inactive | "Configure grub console-settings" |
| nvidia-desktop-default-snaps.service | disabled | inactive | "Seed NVIDIA Desktop Default Snaps" |
| nvidia-dgx-sol.service | disabled | inactive | "NVIDIA DGX Sign of Life Telemetry Service" |
| nvidia-dgx-telemetry.service | disabled | inactive | "NVIDIA DGX Telemetry Service" |
| nvidia-disable-aqc-nic.service | enabled | inactive | "Remove AQC nic before shutdown" |
| nvidia-disable-init-on-alloc.service | enabled | inactive | "NVIDIA Disable Init On Alloc" |
| nvidia-disable-numa-balancing.service | enabled | inactive | "NVIDIA Disable Numa Balancing Config" |
| nvidia-earlycon.service | enabled | inactive | "Configure grub earlycon" |
| nvidia-enable-power-meter-cap.service | enabled | inactive | "NVIDIA Enable Power Meter Capability Config" |
| nvidia-grubserial.service | enabled | inactive | "Configure grubserial" |
| nvidia-pci-realloc.service | enabled | inactive | "Configure grub pci=realloc" |
| nvidia-raid-config.service | enabled | inactive | "NVIDIA RAID Configuration" |
| nvidia-redfish-config.service | enabled | inactive | "NVIDIA Redfish Configuration" |
| nvidia-spark-mlnx-firmware-manager.service | enabled | inactive | "NVIDIA Spark Mellanox Firmware Manager Service"; `/opt/nvidia/spark-mlnx-firmware-manager/launch` |
| nvidia-spark-run-apt-upgrade-once.service | enabled | inactive | "One-time run of apt upgrade after the updated apt pin is in place" |
| nvidia-spark-remove-once.service | disabled | inactive | "One-time remove of nvidia-fs-loader at boot" |
| nvidia-remove-gnome-software-once.service | disabled | inactive | "One-time remove of gnome-software at boot" |
| nvidia-suspend.service | enabled | inactive | "NVIDIA system suspend actions"; `nvidia-sleep.sh "suspend"` |
| nvidia-resume.service | enabled | inactive | "NVIDIA system resume actions"; `nvidia-sleep.sh "resume"` |
| nvidia-hibernate.service | enabled | inactive | "NVIDIA system hibernate actions"; `nvidia-sleep.sh "hibernate"` |
| nvidia-suspend-then-hibernate.service | disabled | inactive | "NVIDIA actions for suspend-then-hibernate" |
| nvsm.service, nvsm-core, nvsm-api-gateway, nvsm-exporter, nvsm-mqtt, nvsm-notifier | masked | inactive | masked; `systemctl cat` printed nothing |

That is 28 nvidia/nv units plus 6 masked nvsm units. Only nvidia-persistenced, nvidia-suspend/resume/hibernate and
nvidia-cdi-refresh call into the GPU driver by their ExecStart; the rest configure GRUB, CPU, NVMe, NICs, RAID,
Redfish, Mellanox firmware, telemetry and apt.

Packages: `dpkg -l` counts 167 installed packages whose line matches nvidia or cuda (`ii` or `hi`), 24 of them on
hold, among them nvidia-driver-580-open, linux-modules-nvidia-580-open-7.0.0-1019-nvidia, libnvidia-compute-580,
nvidia-container-toolkit and nvidia-redfish-config.

Removing these units and packages means replacing the operating system (aienux), not this driver. aien-gpu.ko takes
the GPU compute path; DGX OS keeps its NVIDIA units either way.

## 4. What we may say

Drake's chosen wording, for when it is earned:

> "No NVIDIA driver or CUDA code between my program and the GPU. The machine still runs NVIDIA's DGX OS, and the GPU
> loads NVIDIA's signed firmware."

**It is NOT yet earned.** It becomes true only after a chip receipt shows CUDA++ or the Omega engine running on
aien-gpu.ko with nvidia.ko unloaded. Today: aien-gpu.ko is at most IMPLEMENTED (once OwnDriver's module lands);
hardware NOT_RUN. No window has run (docs/AUDIT.md 1.3), and `lsmod` on 2026-10-26 shows nvidia, nvidia_uvm,
nvidia_modeset and nvidia_drm loaded.

What is true today:

> CUDA++ loads no NVIDIA user-space library, but it uses NVIDIA's open kernel module 580.173.02 through its MIT ioctl
> headers.

Sources: CUDApp 780a0af1 `nvrm/nvrm.c` opens `/dev/nvidiactl`, `/dev/nvidia%u` and `/dev/nvidia-uvm` and calls NVIDIA's
ioctls directly (docs/AUDIT.md section 2), built against `$(NVIDIA_OPEN)` headers (CUDApp Makefile:20-23);
CUDApp docs/DRIVER.md:71 cites a receipt that reads `/proc/self/maps` to show no `libcuda` was loaded;
`/sys/module/nvidia/version` = 580.173.02.
