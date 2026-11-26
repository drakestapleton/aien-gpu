#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# plan_test.sh ORACLE BUILD -- stage B up to the device boundary, on the host: build the real GB20B
# boot package from NVIDIA's 580.173.02 files on this machine (tools/package.sh), lay it out with
# aien-gpu-boot --plan-only and require om/abi.om (the module's own check, linked into the tool) to
# accept the block; then a negative control (poll budget 0) must be refused by the same check.
# Then what the chip would be handed: GspFwWprMeta must describe the GB20B boot loader and name the
# CrashCat queue, the COT packet must drive om/boot.om to stage 0 on the simulated GB20B SEC2
# (build/cot_boot, with its own two controls), and a package with a GB10B-sized boot loader or FMC
# must be refused by the tool.
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
# control: a changed system information text must be refused by its pin, before anything is encoded
{ cat "$ORACLE/evidence/own-gpu-driver/rung2/system_info.txt"; echo "# changed"; } > "$T/si.txt"
if SYSINFO_TXT="$T/si.txt" OGKM_DIR=$OGKM sh "$ROOT/tools/package.sh" "$ORACLE" "$T/pkg2" > "$T/pkg2.log" 2>&1 ||
	! grep -q 'si.txt does not match its pin' "$T/pkg2.log" || [ -e "$T/pkg2/system_info.bin" ]; then
	tail -3 "$T/pkg2.log"; echo "PLAN-TEST FAIL control: a changed system information text was not refused"; exit 1
fi
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
# GspFwWprMeta as built (little endian): sizeOfBootloader 212992 at 40, bootloaderCodeOffset 0xf000 at
# 48, bootloaderDataOffset 0xa00 at 56, sysmemAddrOfCrashReportQueue at 224 (non-zero, 4 KiB aligned),
# sizeOfCrashReportQueue 4096 at 232
hex() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }
M=$T/out/wpr_meta.bin
[ "$(hex "$M" 40 8)" = 0040030000000000 ] && [ "$(hex "$M" 48 8)" = 00f0000000000000 ] &&
	[ "$(hex "$M" 56 8)" = 000a000000000000 ] && [ "$(hex "$M" 232 4)" = 00100000 ] || {
	echo "PLAN-TEST FAIL WprMeta is not GB20B's: $(hex "$M" 40 24) crash size $(hex "$M" 232 4)"; exit 1; }
case "$(hex "$M" 224 8)" in
0000000000000000) echo "PLAN-TEST FAIL WprMeta has no CrashCat queue"; exit 1 ;;
00?0*) ;;
*) echo "PLAN-TEST FAIL CrashCat queue address not 4 KiB aligned: $(hex "$M" 224 8)"; exit 1 ;;
esac
cot=$("$B/cot_boot" "$T/out/cot_packet.bin" 2>&1) || { echo "$cot"; echo "PLAN-TEST FAIL COT packet"; exit 1; }
# controls: the two GB10B image sizes (196608, 195912) must be refused before anything is built
for c in bootloader.bin:196608 fmc.bin:195912; do
	f=${c%:*}; n=${c#*:}
	rm -rf "$T/bad"; cp -r "$T/pkg" "$T/bad"
	head -c "$n" "$T/pkg/$f" > "$T/bad/$f"
	"$B/aien-gpu-boot" --plan-only --pkg "$T/bad" --out "$T/badout" > "$T/bad.log" 2>&1
	brc=$?
	if [ "$brc" -ne 2 ] || ! grep -q '^AIEN_GPU_BOOT: PACKAGE-SIZE' "$T/bad.log"; then
		cat "$T/bad.log"; echo "PLAN-TEST FAIL control $f of $n bytes not refused ($brc)"; exit 1
	fi
done
echo "PLAN-TEST PASS ${line#AIEN_GPU_BOOT: }; $cot; controls: poll budget 0 refused by om/abi.om (code 6), GB10B-sized boot loader and FMC refused, changed system information refused"
