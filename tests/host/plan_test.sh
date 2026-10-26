#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# plan_test.sh ORACLE BUILD -- stage B up to the device boundary, on the host: build the real boot
# package from NVIDIA's 580.173.02 files on this machine (tools/package.sh), lay it out with
# aien-gpu-boot --plan-only and require om/abi.om (the module's own check, linked into the tool) to
# accept the block; then a negative control (poll budget 0) must be refused by the same check.
# Prints PLAN-TEST PASS, PLAN-TEST FAIL, or PLAN-TEST NOT_RUN (exit 3) when the NVIDIA source tree or
# firmware is not on this machine. No device is touched.
set -u
ORACLE=${1:?usage: plan_test.sh ORACLE BUILD}
B=${2:?usage: plan_test.sh ORACLE BUILD}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FW=/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin
OGKM=${OGKM_DIR:-$HOME/workspace/_ext/open-gpu-kernel-modules-580.173.02}
if [ ! -r "$FW" ] || [ ! -d "$OGKM/src/nvidia/generated" ]; then
	echo "PLAN-TEST NOT_RUN (needs $FW and $OGKM)"; exit 3
fi
T=$(mktemp -d /tmp/agpu-plan.XXXXXX)
trap 'rm -rf "$T"' EXIT
OGKM_DIR=$OGKM sh "$ROOT/tools/package.sh" "$ORACLE" "$T/pkg" > "$T/pkg.log" 2>&1 || {
	tail -5 "$T/pkg.log"; echo "PLAN-TEST FAIL package"; exit 1; }
"$B/aien-gpu-boot" --plan-only --pkg "$T/pkg" --out "$T/out" > "$T/run.log" 2>&1
rc=$?
line=$(grep '^AIEN_GPU_BOOT:' "$T/run.log")
case "$rc:$line" in
"0:AIEN_GPU_BOOT: PLAN-ONLY "*" cot 868 bytes "*) ;;
*) cat "$T/run.log"; echo "PLAN-TEST FAIL plan ($rc)"; exit 1 ;;
esac
"$B/aien-gpu-boot" --plan-only --polls 0 --pkg "$T/pkg" --out "$T/neg" > "$T/neg.log" 2>&1
nrc=$?
if [ "$nrc" -ne 2 ] || ! grep -q '^AIEN_GPU_BOOT: BLOCK-REFUSED' "$T/neg.log" || ! grep -q 'verdict 6' "$T/neg.log"; then
	cat "$T/neg.log"; echo "PLAN-TEST FAIL negative control ($nrc)"; exit 1
fi
echo "PLAN-TEST PASS ${line#AIEN_GPU_BOOT: }; control: poll budget 0 refused by om/abi.om (code 6)"
