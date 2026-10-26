#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# window/after-reboot.sh EVDIR --cudapp-m16 PATH -- finish the restore when window/run.sh had to reboot
# (an NVIDIA module would not unload, or NVIDIA's driver would not come back on the GPU our driver had
# started). NVIDIA's modules load at boot as usual; this checks them, runs CUDA++'s m16 gate on
# NVIDIA's path and restarts 18091 from the recipe run.sh saved. Root: none (persistenced is enabled).
set -uo pipefail
EV=${1:?usage: after-reboot.sh EVDIR --cudapp-m16 PATH}; shift
[ "${1:-}" = --cudapp-m16 ] && CUDAPP_M16=$2 || { echo "need --cudapp-m16 PATH"; exit 2; }
LOG=$EV/after-reboot.log
say() { echo "$(date -u +%FT%TZ) $*" | tee -a "$LOG"; }
say "boot id $(cat /proc/sys/kernel/random/boot_id); uptime $(cut -d' ' -f1 /proc/uptime) s"
n=$(grep -c '^nvidia' /proc/modules); say "nvidia modules loaded: $n (want 4)"
lsmod | grep -q '^aien_gpu ' && say "aien_gpu is loaded: unexpected"
nvidia-smi -L > "$EV/after-reboot-nvidia-smi.txt" 2>&1; GPU_OK=$?
say "nvidia-smi rc $GPU_OK; persistenced $(systemctl is-active nvidia-persistenced)"
if [ $GPU_OK != 0 ] || [ "$n" != 4 ]; then
	say "RESTORE FAIL: NVIDIA's driver does not see the GPU after a reboot. Next: a full power cycle (Drake, physical). Nothing else is tried on the chip."
	exit 1
fi
"$CUDAPP_M16" > "$EV/after-reboot-m16-nvidia.txt" 2>&1 && M16=PASS || M16=FAIL
say "CUDA++ m16 on NVIDIA's path: $M16"
if [ -s "$EV/18091.cmdline" ]; then
	t=$(date +%s)
	( cd "$(cat "$EV/18091.cwd")" && env $(tr '\n' ' ' < "$EV/18091.env") nohup $(tr '\n' ' ' < "$EV/18091.cmdline") \
		>> "$HOME/.local/state/aien-q36/serve.log" 2>&1 & )
	LIVE=FAIL
	for _ in $(seq 900); do curl -s -m 2 http://127.0.0.1:18091/health | grep -q '"status":"ok"' && { LIVE=PASS; break; }; sleep 1; done
	rm -f /tmp/aien-q36-live-down-for-tests
	say "18091 $LIVE in $(( $(date +%s) - t )) s"
fi
say "AFTER-REBOOT restore=PASS m16_nvidia=$M16 live18091=${LIVE:-NOT_RUNNING_BEFORE}"
