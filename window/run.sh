#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# window/run.sh -- the hardware bring-up of aien-gpu.ko on the GB10, as one ordered script.
# READ window/README.md FIRST. It needs Drake's explicit authorization for an exclusive GPU window:
# it stops the live 35B on 18091, unloads NVIDIA's four kernel modules, loads ours, starts GSP-RM
# with our code (stage B), brings up RM and runs the m16 gate through our driver (stage C), then
# unloads ours, reloads NVIDIA's, runs CUDA++'s m16 gate on NVIDIA's path and restores 18091.
# Any failed stage stops the stages and goes straight to the rollback (steps R1..R6).
#
#   window/run.sh check  --pkg DIR [options]   read-only preflight, no root, touches nothing (default)
#   window/run.sh go     --pkg DIR --authorized "who/when/where Drake said yes" [options]
# options: --ev DIR (evidence, default ~/handoffs/L332/window-<UTC>), --max-stage A|B|C (default C),
#          --cudapp-m16 PATH (CUDA++ m16 requalification binary for the NVIDIA path; required for go),
#          --arena-mib N (default 96), --heap-mib N (default 512)
# 'go' refuses unless it runs inside `spark-window run` of a window opened with --gb10 exclusive.
# Root commands it runs (each through `sudo -n`, nothing else; see README "sudoers"):
#   systemctl stop|start nvidia-persistenced; rmmod nvidia_drm|nvidia_modeset|nvidia_uvm|nvidia|aien_gpu;
#   modprobe nvidia|nvidia_uvm|nvidia_modeset|nvidia_drm; insmod <repo>/kmod/aien_gpu.ko allow_bind=1
#   arena_mib=N heap_mib=N; sysctl -w kernel.panic=.. kernel.panic_on_oops=..; fuser -v /dev/nvidia*;
#   <repo>/build/aien-gpu-boot, <repo>/build/aien-gpu-m16 (they open the root-only /dev/aien-gpu);
#   reboot (only on the fallback path).
set -uo pipefail

MODE=${1:-check}; shift || true
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PKG= EV= AUTH= MAXSTAGE=C CUDAPP_M16= ARENA_MIB=96 HEAP_MIB=512
while [ $# -gt 0 ]; do
	case $1 in
	--pkg) PKG=$2; shift 2 ;;
	--ev) EV=$2; shift 2 ;;
	--authorized) AUTH=$2; shift 2 ;;
	--max-stage) MAXSTAGE=$2; shift 2 ;;
	--cudapp-m16) CUDAPP_M16=$2; shift 2 ;;
	--arena-mib) ARENA_MIB=$2; shift 2 ;;
	--heap-mib) HEAP_MIB=$2; shift 2 ;;
	*) echo "unknown option $1"; exit 2 ;;
	esac
done
GPU=000f:01:00.0
KO=$ROOT/kmod/aien_gpu.ko
BOOT=$ROOT/build/aien-gpu-boot
M16=$ROOT/build/aien-gpu-m16
NV_MODS_UNLOAD="nvidia_drm nvidia_modeset nvidia_uvm nvidia"
NV_MODS_LOAD="nvidia nvidia_uvm nvidia_modeset nvidia_drm"
MARK=/tmp/aien-q36-live-down-for-tests
T0=$(date +%s)
EV=${EV:-$HOME/handoffs/L332/window-$(date -u +%Y%m%dT%H%M%SZ)}
LOG=

say() { local l; l="$(date -u +%H:%M:%S) +$(( $(date +%s) - T0 ))s $*"; echo "$l"; [ -n "$LOG" ] && echo "$l" >> "$LOG"; }
die() { say "REFUSED: $*"; exit 2; }
S() { say "root: $*"; sudo -n "$@"; }

# ---- preflight (both modes): read-only ----
preflight() {
	local ok=1
	[ -n "$PKG" ] && [ -f "$PKG/MANIFEST.txt" ] || { echo "no boot package (--pkg; build with tools/package.sh)"; ok=0; }
	[ -f "$KO" ] || { echo "no $KO (make kmod)"; ok=0; }
	[ -x "$BOOT" ] || { echo "no $BOOT (make tools)"; ok=0; }
	[ -x "$M16" ] || { echo "no $M16 (make tools)"; ok=0; }
	if [ -f "$KO" ]; then
		local vm; vm=$(/sbin/modinfo -F vermagic "$KO" | cut -d' ' -f1)
		[ "$vm" = "$(uname -r)" ] || { echo "vermagic $vm != $(uname -r)"; ok=0; }
	fi
	[ -e /sys/bus/pci/devices/$GPU ] || { echo "GPU $GPU absent"; ok=0; }
	[ "$(cat /sys/bus/pci/devices/$GPU/vendor 2>/dev/null)" = 0x10de ] || { echo "GPU vendor not NVIDIA"; ok=0; }
	[ "$(cat /sys/bus/pci/devices/$GPU/device 2>/dev/null)" = 0x2e12 ] || { echo "GPU device not 0x2e12"; ok=0; }
	grep -q '^nvidia ' /proc/modules || echo "note: nvidia not loaded now"
	echo "live 18091: $(curl -s -m 2 http://127.0.0.1:18091/health || echo unreachable)"
	echo "nvidia-persistenced: $(systemctl is-active nvidia-persistenced)"
	echo "kernel.panic=$(cat /proc/sys/kernel/panic) panic_on_oops=$(cat /proc/sys/kernel/panic_on_oops)"
	echo "secure boot: $(mokutil --sb-state 2>/dev/null || echo unknown); sig_enforce=$(cat /sys/module/module/parameters/sig_enforce)"
	for c in "systemctl is-active nvidia-persistenced" "rmmod --version" "modprobe --version"; do :; done
	if sudo -n true 2>/dev/null; then echo "sudo -n: available"; else echo "sudo -n: NOT available (go would fail)"; ok=0; fi
	[ $ok = 1 ]
}

if [ "$MODE" = check ]; then
	if preflight; then echo "WINDOW-CHECK PASS (nothing was changed)"; exit 0; fi
	echo "WINDOW-CHECK FAIL"; exit 1
fi
[ "$MODE" = go ] || die "mode must be check or go"
[ -n "$AUTH" ] || die "--authorized is required: Drake's explicit authorization for this window"
[ -n "${SPARK_WINDOW:-}" ] && [ "${AIEN_GB10_LOCK_HELD:-}" = 1 ] || die "run inside: spark-window open l332-aien-gpu --ledger L332 --gb10 exclusive --quiet none --lease 20 --wait; spark-window run window/run.sh go ..."
[ -x "$CUDAPP_M16" ] || die "--cudapp-m16 PATH: the CUDA++ m16 gate for the NVIDIA path must be ready before NVIDIA is unloaded"
mkdir -p "$EV" || die "evidence dir"
LOG=$EV/window.log
preflight >> "$LOG" 2>&1 || die "preflight failed (see $LOG)"
say "WINDOW START authorized: $AUTH; window $SPARK_WINDOW; repo $(git -C "$ROOT" rev-parse HEAD); ko sha256 $(sha256sum "$KO" | cut -d' ' -f1)"

STAGE_A=NOT_RUN STAGE_B=NOT_RUN STAGE_C=NOT_RUN M16_AIEN=NOT_RUN RESTORE=NOT_RUN M16_NV=NOT_RUN LIVE=NOT_RUN
REBOOT_NEEDED=0 AIEN_LOADED=0 NV_UNLOADED=0

# ---- W1 snapshot and the 18091 rollback recipe ----
say "W1 snapshot"
{ lsmod; nvidia-smi -L; nvidia-smi -q -d PERFORMANCE,MEMORY | head -60; } > "$EV/before.txt" 2>&1
S fuser -v /dev/nvidia* > "$EV/holders-before.txt" 2>&1
LP=$(pgrep -f 'aien-serve-http.*--port 18091' | head -1)
if [ -n "$LP" ]; then
	readlink "/proc/$LP/cwd" > "$EV/18091.cwd"
	tr '\0' '\n' < "/proc/$LP/cmdline" > "$EV/18091.cmdline"
	tr '\0' '\n' < "/proc/$LP/environ" | grep -E '^(AIEN|Q36|OMEGA)' | grep -v -E 'KEY|TOKEN|SECRET' > "$EV/18091.env"
	say "W1 18091 pid $LP cwd $(cat "$EV/18091.cwd")"
else
	say "W1 18091 not running: it will not be restarted by this script"
fi

# ---- W2 drain and stop 18091 ----
if [ -n "$LP" ]; then
	touch "$MARK"
	for _ in $(seq 60); do
		inflight=$(curl -s -m 2 http://127.0.0.1:18091/health | sed -n 's/.*"in_flight":\([0-9]*\).*/\1/p')
		[ "${inflight:-0}" = 0 ] && break; sleep 1
	done
	say "W2 SIGTERM 18091 (in_flight ${inflight:-?})"
	kill -TERM "$LP"
	for _ in $(seq 60); do kill -0 "$LP" 2>/dev/null || break; sleep 1; done
	kill -0 "$LP" 2>/dev/null && die "18091 did not exit within 60 s (nothing else changed; it is still running)"
fi

# ---- W3 no other GPU holder ----
S fuser -v /dev/nvidia* > "$EV/holders-w3.txt" 2>&1
if grep -v -E 'nvidia-persiste|USER|^/dev|kernel' "$EV/holders-w3.txt" | grep -q '[0-9]'; then
	say "W3 other GPU holders remain (see holders-w3.txt): stopping here, restoring 18091"
	RESTORE_ONLY=1
fi

restore_live() {
	[ -s "$EV/18091.cmdline" ] || { LIVE=NOT_RUNNING_BEFORE; return; }
	local t=$(date +%s)
	say "R6 restart 18091 from the recipe"
	( cd "$(cat "$EV/18091.cwd")" && env $(tr '\n' ' ' < "$EV/18091.env") nohup $(tr '\n' ' ' < "$EV/18091.cmdline") \
		>> "$HOME/.local/state/aien-q36/serve.log" 2>&1 & )
	for _ in $(seq 900); do
		curl -s -m 2 http://127.0.0.1:18091/health | grep -q '"ok"\|ready' && { LIVE=PASS; break; }
		sleep 1
	done
	[ "$LIVE" = PASS ] || LIVE=FAIL
	rm -f "$MARK"
	say "R6 18091 $LIVE in $(( $(date +%s) - t )) s"
}

if [ "${RESTORE_ONLY:-0}" = 1 ]; then restore_live; say "WINDOW END stopped at W3"; exit 1; fi

# ---- W4 persistenced, W5 panic on oops, W6 unload NVIDIA ----
OLD_PANIC=$(cat /proc/sys/kernel/panic) OLD_OOPS=$(cat /proc/sys/kernel/panic_on_oops)
S systemctl stop nvidia-persistenced
S sysctl -w kernel.panic=10 kernel.panic_on_oops=1 >> "$LOG"
t=$(date +%s)
for m in $NV_MODS_UNLOAD; do
	grep -q "^$m " /proc/modules || continue
	if ! timeout 60 sudo -n rmmod "$m"; then
		say "W6 rmmod $m failed or hung (L158: nvidia_modeset's kthread can block it): fallback reboot"
		REBOOT_NEEDED=1; break
	fi
done
if [ $REBOOT_NEEDED = 1 ]; then
	echo "after the reboot run: window/after-reboot.sh $EV" > "$EV/NEXT"
	say "WINDOW END W6 reboot; then window/after-reboot.sh $EV"; S reboot; exit 1
fi
NV_UNLOADED=1
say "W6 NVIDIA modules unloaded in $(( $(date +%s) - t )) s; driver link: $(readlink /sys/bus/pci/devices/$GPU/driver || echo none)"
[ -e /sys/bus/pci/devices/$GPU/driver ] && { say "W6 GPU still bound to a driver"; REBOOT_NEEDED=1; }

# ---- W7 stage A: our module binds and reads the identity ----
stage_a() {
	S insmod "$KO" allow_bind=1 arena_mib="$ARENA_MIB" heap_mib="$HEAP_MIB" || return 1
	AIEN_LOADED=1
	dmesg | grep -E 'aien-gpu|aien_gpu' | tail -20 > "$EV/stageA-dmesg.txt"
	grep -q 'omega self-test PASS' "$EV/stageA-dmesg.txt" || return 1
	grep -q ' bound: BOOT_0' "$EV/stageA-dmesg.txt" || return 1
	S "$BOOT" --info > "$EV/stageA-info.txt" 2>&1 || return 1
	grep -q '^AIEN_GPU_INFO: abi=1 state=1 .*fw_ok=1' "$EV/stageA-info.txt"
}
# ---- W8 stage B: GSP-RM started by our code, INIT_DONE ----
stage_b() {
	S "$BOOT" --pkg "$PKG" --out "$EV/stageB" > "$EV/stageB.txt" 2>&1
	local rc=$?
	grep -q '^AIEN_GPU_BOOT: INIT-DONE' "$EV/stageB.txt" && [ $rc = 0 ]
}
# ---- W9 stage C: RM bring-up, then the m16 gate through our driver ----
stage_c() {
	S "$M16" --rm-init --out "$EV/stageC" > "$EV/stageC-rminit.txt" 2>&1 || return 1
	grep -q '^AIEN_GPU_RMINIT: OK' "$EV/stageC-rminit.txt" || return 1
	S "$M16" --out "$EV/m16" > "$EV/stageC-m16.txt" 2>&1
	grep -q '^AIEN_GPU_M16: PASS' "$EV/stageC-m16.txt" && M16_AIEN=PASS || { M16_AIEN=FAIL; return 1; }
}
if [ $REBOOT_NEEDED = 0 ]; then
	if stage_a; then STAGE_A=PASS; else STAGE_A=FAIL; fi
	say "W7 stage A $STAGE_A"
	if [ $STAGE_A = PASS ] && [ "$MAXSTAGE" != A ]; then
		if stage_b; then STAGE_B=PASS; else STAGE_B=FAIL; fi
		say "W8 stage B $STAGE_B: $(grep '^AIEN_GPU_BOOT:' "$EV/stageB.txt" | head -1)"
	fi
	if [ $STAGE_B = PASS ] && [ "$MAXSTAGE" = C ]; then
		if stage_c; then STAGE_C=PASS; else STAGE_C=FAIL; fi
		say "W9 stage C $STAGE_C: $(grep -h -E '^AIEN_GPU_(RMINIT|M16):' "$EV"/stageC-*.txt | tr '\n' ' ')"
	fi
fi

# ---- rollback / restore, always ----
dmesg > "$EV/dmesg-before-restore.txt"
if [ $AIEN_LOADED = 1 ]; then
	say "R1 rmmod aien_gpu (sends UNLOADING_GUEST_DRIVER when GSP-RM is up)"
	timeout 60 sudo -n rmmod aien_gpu || { say "R1 rmmod aien_gpu failed"; REBOOT_NEEDED=1; }
fi
if [ $REBOOT_NEEDED = 0 ] && [ $NV_UNLOADED = 1 ]; then
	t=$(date +%s)
	for m in $NV_MODS_LOAD; do timeout 180 sudo -n modprobe "$m" || { REBOOT_NEEDED=1; break; }; done
	for _ in $(seq 60); do nvidia-smi -L > "$EV/after-nvidia-smi.txt" 2>&1 && break; sleep 1; done
	grep -q 'GPU 0' "$EV/after-nvidia-smi.txt" || REBOOT_NEEDED=1
	say "R2 NVIDIA reload $([ $REBOOT_NEEDED = 0 ] && echo PASS || echo FAIL) in $(( $(date +%s) - t )) s"
fi
if [ $REBOOT_NEEDED = 1 ]; then
	RESTORE=REBOOT
	echo "after the reboot run: window/after-reboot.sh $EV" > "$EV/NEXT"
	say "WINDOW STAGES A=$STAGE_A B=$STAGE_B C=$STAGE_C m16_aien=$M16_AIEN; GPU needs a reset: rebooting; then window/after-reboot.sh $EV"
	S sysctl -w kernel.panic="$OLD_PANIC" kernel.panic_on_oops="$OLD_OOPS" >> "$LOG"
	S reboot; exit 1
fi
S systemctl start nvidia-persistenced
say "R3 nvidia-persistenced $(systemctl is-active nvidia-persistenced)"
"$CUDAPP_M16" > "$EV/m16-nvidia.txt" 2>&1 && M16_NV=PASS || M16_NV=FAIL
say "R4 CUDA++ m16 on NVIDIA's path $M16_NV"
S sysctl -w kernel.panic="$OLD_PANIC" kernel.panic_on_oops="$OLD_OOPS" >> "$LOG"
say "R5 sysctl restored"
RESTORE=PASS
[ $M16_NV = PASS ] || RESTORE=FAIL
restore_live
{ lsmod; nvidia-smi -L; } > "$EV/after.txt" 2>&1
say "AIEN-GPU-WINDOW stageA=$STAGE_A stageB=$STAGE_B stageC=$STAGE_C m16_aien=$M16_AIEN restore=$RESTORE m16_nvidia=$M16_NV live18091=$LIVE total_s=$(( $(date +%s) - T0 ))"
(cd "$EV" && sha256sum -- * > SHA256SUMS 2>/dev/null)
[ $STAGE_C = PASS ] && [ $RESTORE = PASS ] && [ $LIVE != FAIL ]
