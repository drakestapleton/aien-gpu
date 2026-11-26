# The hardware window for aien-gpu.ko

Status: **`run.sh go` is DISABLED (2026-11-18).** This procedure unloads NVIDIA's driver and loads
ours on the same boot. That was tried once on the chip (2026-11-18T23:09Z, with the GB10B start-up):
our start-up stopped with the suspended marker 0x80000000 in the GSP's MAILBOX0, and afterwards
nvidia.ko did not re-initialise the chip (`ksec2SendBootCommands_GB20B: SEC2 boot cmds failed`,
then `Timed out waiting for SEC2 command queue to be empty`), so the machine had to restart
(`docs/evidence/hot-20261118T2309Z-kernel.txt`). One run does not show that a hot start can never
work; it is not tried again here. The chip must be cold: booted with nvidia held
back, our module loaded first, and a clean restart as the way back to nvidia.ko. A sysfs PCIe FLR on
000f:01:00.0 is not a way back: it reset the whole Spark (2026-11-18T23:15:09Z). `run.sh check` still
works and changes nothing. The cold-boot procedure is not written in this repository yet; the one
cold run so far is recorded in `~/handoffs/L332/window0-20261118T234217Z` (stage A PASS, stage B
stopped with the GB10B start-up and package). The steps below describe the disabled hot-swap path and
stay as the record of what each stage proves. Any run on the chip needs Drake's explicit
authorization.

## How to run

```sh
# before the window, no root, changes nothing
tools/package.sh build/oracle /tmp/agpu-pkg            # NVIDIA's signed images on loan, pinned
make OMC=<pinned omc> check tools                      # host, QEMU and static gates green
window/run.sh check --pkg /tmp/agpu-pkg                 # WINDOW-CHECK PASS
# the window
spark-window open l332-aien-gpu --ledger L332 --gb10 exclusive --quiet none --lease 20 --wait
spark-window run window/run.sh go --pkg /tmp/agpu-pkg --cudapp-m16 <CUDApp m16 requalify binary> \
    --authorized "Drake, <date>, <where he said yes>"
spark-window close
# only if run.sh rebooted
window/after-reboot.sh <evidence dir> --cudapp-m16 <same binary>
```

## Steps, what each proves, rollback

| Step | Does | Proves | On failure |
|---|---|---|---|
| W1 | snapshot lsmod, nvidia-smi, GPU holders; save 18091's cmdline, cwd, env (keys redacted) | the restore recipe exists before anything stops | stop |
| W2 | wait for in_flight 0 (60 s), SIGTERM 18091, wait for exit (60 s) | nothing is using the 35B | stop: nothing changed |
| W3 | no other process holds /dev/nvidia* | no other lane loses its GPU | restart 18091, stop |
| W4 | `systemctl stop nvidia-persistenced` | it no longer holds nvidia (else "Module nvidia is in use") | rollback |
| W5 | `sysctl kernel.panic=10 panic_on_oops=1` (old values saved) | an oops in our module reboots the box instead of freezing it | rollback |
| W6 | `rmmod nvidia_drm nvidia_modeset nvidia_uvm nvidia`, 60 s each | NVIDIA's kernel code is gone from the GPU; time to unload | reboot path (L158: modeset's kthread may block the unload) |
| W7 stage A | `insmod aien_gpu.ko allow_bind=1 arena_mib=96 heap_mib=512`; dmesg `omega self-test PASS`, `bound: BOOT_0 ..`; `aien-gpu-boot --info` state 1, fw_ok 1 | our module binds the GB10 and reads its identity with Omega deciding; firmware pin | rollback |
| W8 stage B | `aien-gpu-boot --pkg ..` (AGPU_BOOT): 12 register steps by om/boot.om, status queue until INIT_DONE with sequencer requests; GSP logs copied out | GSP-RM started by our code, no NVIDIA kernel code | rollback (GPU may need a reset) |
| W9 stage C | `aien-gpu-m16 --rm-init` (AGPU_RM_INIT, static info saved), then `aien-gpu-m16` (negative control: no doorbell, marker stays 0 for 100 ms; then doorbell, marker 0x16c0ffee) | a CUDA++ m16 gate runs through our driver only | rollback |
| R1 | `rmmod aien_gpu` (UNLOADING_GUEST_DRIVER, halt wait) | clean unload | reboot path |
| R2 | `modprobe nvidia nvidia_uvm nvidia_modeset nvidia_drm`; `nvidia-smi -L` within 60 s | NVIDIA's driver re-boots the GPU after ours | reboot path |
| R3 | `systemctl start nvidia-persistenced` | DGX OS state back | report |
| R4 | CUDA++ m16 requalification on NVIDIA's path | the GPU computes correctly again | report, keep 18091 down until checked |
| R5 | sysctl restored | | |
| R6 | 18091 restarted from the recipe, `/health` ok within 15 min | service back | report |

Final line: `AIEN-GPU-WINDOW stageA= stageB= stageC= m16_aien= restore= m16_nvidia= live18091= total_s=`.
Evidence goes to `~/handoffs/L332/window-<UTC>/` with SHA256SUMS. A FAIL at any stage is still a result:
the verdict names the step (`docs/STATUS.md` explains each code).

## Time (MODELED, except the 35B load)

| Part | Estimate | Basis |
|---|---|---|
| W1-W5 | 1-2 min | drain bound 60 s + exit bound 60 s |
| W6 unload | under 1 min, or a reboot | never measured on this box (nvidia.ko has not been reloaded since boot) |
| W7-W9 | 1-2 min | INIT_DONE wait bound 30 s; RPC deadlines; m16 under 10 s |
| R1-R4 | 2-5 min | NVIDIA reload time UNKNOWN (never measured here) |
| R6 35B back | about 1.5 min load | MEASURED: `engine ready in 82.5 s` and `86.7 s` (~/.local/state/aien-q36/serve.log lines 13211, 13754) |
| Total | about 10 min of 18091 downtime; 20-30 min if a reboot is needed | reboot-to-ssh time is UNKNOWN |

The exclusive spark-window lease is at most 20 min; W1-R4 fit inside it, R6 runs after the GPU work.

## sudoers: the exact root commands

`window/run.sh` runs only these through `sudo -n` (paths as on DGX OS):

```
/usr/bin/systemctl stop nvidia-persistenced
/usr/bin/systemctl start nvidia-persistenced
/usr/sbin/rmmod nvidia_drm
/usr/sbin/rmmod nvidia_modeset
/usr/sbin/rmmod nvidia_uvm
/usr/sbin/rmmod nvidia
/usr/sbin/rmmod aien_gpu
/usr/sbin/modprobe nvidia
/usr/sbin/modprobe nvidia_uvm
/usr/sbin/modprobe nvidia_modeset
/usr/sbin/modprobe nvidia_drm
/usr/sbin/insmod <repo>/kmod/aien_gpu.ko allow_bind=1 arena_mib=96 heap_mib=512
/usr/sbin/sysctl -w kernel.panic=10 kernel.panic_on_oops=1
/usr/sbin/sysctl -w kernel.panic=<saved> kernel.panic_on_oops=<saved>
/usr/bin/fuser -v /dev/nvidia*
<repo>/build/aien-gpu-boot --info | --pkg <dir> --out <dir>
<repo>/build/aien-gpu-m16 --rm-init --out <dir> | --out <dir>
/usr/sbin/reboot          (fallback only)
```

NVIDIA's modules are reloaded with `modprobe`, not `insmod`, so their dependencies and DGX OS's
modprobe.d options apply exactly as at boot. The two tools need root because /dev/aien-gpu is 0600 and
AGPU_BOOT / AGPU_RM_INIT require CAP_SYS_ADMIN.
