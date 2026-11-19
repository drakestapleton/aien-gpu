#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# plan_test.sh ORACLE BUILD -- stage B up to the device boundary, on the host: build BOTH the GB10B
# package (tools/package.sh) and the GB20B package (tools/package-gb20b.sh) from NVIDIA 580.173.02
# files on this machine, lay each out with aien-gpu-boot --plan-only and require om/abi.om and
# om/boot.om to accept the block and COTv2 packet; then negative controls (poll budget 0, bad GB20B
# bl_desc.txt code=0xb000) must be refused. No device is touched.
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

# GB10B package + plan-only + negative control
OGKM_DIR=$OGKM sh "$ROOT/tools/package.sh" "$ORACLE" "$T/pkg10" > "$T/pkg10.log" 2>&1 || {
	tail -5 "$T/pkg10.log"; echo "PLAN-TEST FAIL package gb10b"; exit 1; }
"$B/aien-gpu-boot" --plan-only --pkg "$T/pkg10" --out "$T/out10" > "$T/run10.log" 2>&1
rc=$?
line10=$(grep '^AIEN_GPU_BOOT:' "$T/run10.log")
case "$rc:$line10" in
"0:AIEN_GPU_BOOT: PLAN-ONLY profile=gb10b "*" cot 868 bytes "*) ;;
*) cat "$T/run10.log"; echo "PLAN-TEST FAIL plan gb10b ($rc)"; exit 1 ;;
esac
"$B/aien-gpu-boot" --plan-only --polls 0 --pkg "$T/pkg10" --out "$T/neg10" > "$T/neg10.log" 2>&1
nrc=$?
if [ "$nrc" -ne 2 ] || ! grep -q '^AIEN_GPU_BOOT: BLOCK-REFUSED' "$T/neg10.log" || ! grep -q 'verdict 6' "$T/neg10.log"; then
	cat "$T/neg10.log"; echo "PLAN-TEST FAIL negative control gb10b ($nrc)"; exit 1
fi

# GB20B package + plan-only + negative controls
OGKM_DIR=$OGKM bash "$ROOT/tools/package-gb20b.sh" "$ORACLE" "$T/pkg20" > "$T/pkg20.log" 2>&1 || {
	tail -10 "$T/pkg20.log"; echo "PLAN-TEST FAIL package gb20b"; exit 1; }
"$B/aien-gpu-boot" --plan-only --pkg "$T/pkg20" --out "$T/out20" > "$T/run20.log" 2>&1
rc20=$?
line20=$(grep '^AIEN_GPU_BOOT:' "$T/run20.log")
case "$rc20:$line20" in
"0:AIEN_GPU_BOOT: PLAN-ONLY profile=gb20b "*" cot 868 bytes "*) ;;
*) cat "$T/run20.log"; echo "PLAN-TEST FAIL plan gb20b ($rc20)"; exit 1 ;;
esac
"$B/aien-gpu-boot" --plan-only --polls 0 --pkg "$T/pkg20" --out "$T/neg20p" > "$T/neg20p.log" 2>&1
nrc20p=$?
if [ "$nrc20p" -ne 2 ] || ! grep -q '^AIEN_GPU_BOOT: BLOCK-REFUSED' "$T/neg20p.log" || ! grep -q 'verdict 6' "$T/neg20p.log"; then
	cat "$T/neg20p.log"; echo "PLAN-TEST FAIL negative control gb20b polls=0 ($nrc20p)"; exit 1
fi
cp -a "$T/pkg20" "$T/pkg20bad"
sed -i 's/^code=0xf000$/code=0xb000/' "$T/pkg20bad/bl_desc.txt"
"$B/aien-gpu-boot" --plan-only --pkg "$T/pkg20bad" --out "$T/neg20b" > "$T/neg20b.log" 2>&1
nrc20b=$?
if [ "$nrc20b" -ne 2 ] || ! grep -q '^AIEN_GPU_BOOT: BUILD-REFUSED' "$T/neg20b.log"; then
	cat "$T/neg20b.log"; echo "PLAN-TEST FAIL negative control gb20b bad bl_desc ($nrc20b)"; exit 1
fi

echo "PLAN-TEST PASS ${line10#AIEN_GPU_BOOT: } | ${line20#AIEN_GPU_BOOT: }; controls: polls=0 refused (code 6) and gb20b code=0xb000 refused"
