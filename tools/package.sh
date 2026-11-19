#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# package.sh ORACLE OUT -- build the GSP boot package aien-gpu-boot reads, for GB20B (the chip in the
# DGX Spark: PMC_BOOT_42 0x1bba1000). Uses the extractors of the pinned aienux clone (ORACLE =
# build/oracle, c9678d7b); they are chip-neutral. Reads NVIDIA's signed 580.173.02 files on this machine
# (on loan, docs/BOUNDARY.md) and checks every piece against a pin below: the firmware container and
# its two sections, the boot loader with its descriptor, the FMC with hash, key and signature, and
# the system information as text and as encoded. Writes only into OUT.
# Touches no device. Exit 0 only on WINDOW1_PACKAGE: PASS.
#
# What makes the package GB20B (open-gpu-kernel-modules 580.173.02):
#   GSP-RM signature   .fwsignature_gb20y: architecture GB2XX with implementation >= GB20B is family
#                      GB20Y (arch/nvalloc/common/inc/nv-firmware-chip-family-select.h:64-68)
#   boot loader        kgspGetBinArchiveGspRmBoot_GB20B           (generated/g_kernel_gsp_nvoc.c:792-794)
#   FMC, hash, key, signature
#                      kgspGetBinArchiveGspRmFmcGfwProdSigned_GB20B (:928-930)
# The GB10B pieces (gb10y, _GB10B archives) are for another chip: SEC2 verifies the FMC against a key
# of its own chip. Pins measured 2026-11-18 on spark-b87b (~/work/gsp-firmware-contents/SHA256SUMS).
set -eu
ORACLE=${1:?usage: package.sh ORACLE OUT}
OUT=${2:?usage: package.sh ORACLE OUT}
FAMILY=gb20y
CHIP=GB20B
FW=/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin
GEN=${OGKM_DIR:-$HOME/workspace/_ext/open-gpu-kernel-modules-580.173.02}/src/nvidia/generated
SI_TXT=${SYSINFO_TXT:-$ORACLE/evidence/own-gpu-driver/rung2/system_info.txt}
PIN_FW=32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963
PIN_RM_ELF=931add19204a5f0240442272f58fb2bc5db6cec4dd6f6341c1d888754146aa07
PIN_RM_SIG=6d38023b5b15a9e0d093be9dd443ebafb2aba3e0a3f1f260c387afc903a1f9a1
PIN_BOOTLOADER=6da8510c912ccd5a8b73f9a5cce9e0f4ea657aa430431d2f8e4874834aeadb94
PIN_FMC=b92f83c1250019af11162d9ee471b8fb74081ea598cf6added0bee7a3663e2be
PIN_FMC_HASH=4a5854c785fa7a9ea4aaabb52f6c9838242f81506659ff413da0f4b8ac1f40bf
PIN_FMC_PKEY=54076b108ad3da65e09f675d88efd553340a0c0653cf13372f4e30dd6ab17e7c
PIN_FMC_SIG=6c5fbbf096efff9d0f47d4cc70096898f059579154464e6c3b972d2e606f1334
# the boot loader's RM_RISCV_UCODE_DESC as ck_gsp_bindata_extract prints it (tools/gb20b_gsp_boot.h)
# GspSystemInfo of this Spark (evidence/own-gpu-driver/rung2 of the oracle): the text and its encoding
PIN_SYSINFO_TXT=fd384ca727b74c6c29feda3b7a8c20491099b1a2dac6966707d4264e5ae7086f
PIN_SYSINFO_BIN=bd4b23c08ecf78e074132b58a6968ee52262d801e8092ed4e63cc12d8c82ee5d
PIN_DESC='version=5 manifest=0x0+0xa00 monitor_data=0xa00+0xe600 monitor_code=0xf000+0x24d00 monitor_enabled=1 fb_reserved=0x0 signed_as_code=0 app_version=0 image=UCODE_IMAGE_PROD/212992'

die() { echo "WINDOW1_PACKAGE: FAIL ($*)"; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
pinned() { [ "$(sha "$1")" = "$2" ] || die "$1 does not match its pin $2"; }

T=$(mktemp -d /tmp/agpu-pkgtools.XXXXXX)
trap 'rm -rf "$T"' EXIT
make -s -C "$ORACLE/native/kernel" OUT="$T" gsp-fw-inspect gsp-bindata-extract "$T/host/ck_gsp_sysinfo_encode" >&2
TOOLS=$T/host
for t in ck_gsp_fw_inspect ck_gsp_bindata_extract ck_gsp_sysinfo_encode; do
	[ -x "$TOOLS/$t" ] || die "missing tool $TOOLS/$t"
done
mkdir -p "$OUT"
[ -z "$(ls -A "$OUT")" ] || die "$OUT is not empty"
W=$OUT/work
mkdir "$W"

# 1. GSP-RM: the .fwimage section and this chip family's signature section of the container
pinned "$FW" "$PIN_FW"
"$TOOLS/ck_gsp_fw_inspect" inspect "$FW" --family "$FAMILY" --expect-version 580.173.02 > "$W/inspect.txt" ||
	die "fw inspect refused"
grep -q '^RESULT OK' "$W/inspect.txt" || die "fw inspect not OK"
cutsec() { # section-name out
	line=$(awk -v n="$1" '$1=="section" && $3==n { sub("offset=","",$5); sub("size=","",$6); print $5, $6 }' "$W/inspect.txt")
	off=${line% *}
	size=${line#* }
	[ -n "$line" ] && [ -n "$off" ] && [ -n "$size" ] || die "section $1 not in the inspect output"
	tail -c +$((off + 1)) "$FW" | head -c "$size" > "$2"
	[ "$(stat -c %s "$2")" -eq "$size" ] || die "short cut of $1"
}
cutsec .fwimage "$OUT/rm_elf.bin"
cutsec ".fwsignature_$FAMILY" "$OUT/rm_sig.bin"
[ "$(sha "$OUT/rm_elf.bin")" = "$(awk '$1=="sha256_fwimage"{print $2}' "$W/inspect.txt")" ] ||
	die ".fwimage cut differs from the inspected hash"
[ "$(sha "$OUT/rm_sig.bin")" = "$(awk '$1=="sha256_signature"{print $2}' "$W/inspect.txt")" ] ||
	die "signature cut differs from the inspected hash"
pinned "$OUT/rm_elf.bin" "$PIN_RM_ELF"
pinned "$OUT/rm_sig.bin" "$PIN_RM_SIG"

# 2. GspRmBoot (PROD image + its descriptor) and GspRmFmcGfwProdSigned (image, hash, key, signature)
"$TOOLS/ck_gsp_bindata_extract" extract "$GEN/g_bindata_kgspGetBinArchiveGspRmBoot_$CHIP.c" --out "$W" > "$W/boot.txt" ||
	die "boot bindata refused"
"$TOOLS/ck_gsp_bindata_extract" extract "$GEN/g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_$CHIP.c" --out "$W" > "$W/fmc.txt" ||
	die "fmc bindata refused"
B=$W/kgspGetBinArchiveGspRmBoot_$CHIP
M=$W/kgspGetBinArchiveGspRmFmcGfwProdSigned_$CHIP
cp "$B.UCODE_IMAGE_PROD.bin" "$OUT/bootloader.bin"
cp "$M.UCODE_IMAGE.bin" "$OUT/fmc.bin"
cp "$M.UCODE_HASH.bin" "$OUT/fmc_hash.bin"
cp "$M.UCODE_PKEY.bin" "$OUT/fmc_pkey.bin"
cp "$M.UCODE_SIG.bin" "$OUT/fmc_sig.bin"
pinned "$OUT/bootloader.bin" "$PIN_BOOTLOADER"
pinned "$OUT/fmc.bin" "$PIN_FMC"
pinned "$OUT/fmc_hash.bin" "$PIN_FMC_HASH"
pinned "$OUT/fmc_pkey.bin" "$PIN_FMC_PKEY"
pinned "$OUT/fmc_sig.bin" "$PIN_FMC_SIG"
grep -q '^hash384 UCODE_HASH equals sha384(UCODE_IMAGE)' "$W/fmc.txt" || die "FMC hash not checked against the image"
desc=$(sed -n 's/^desc UCODE_DESC_PROD //p' "$W/boot.txt")
[ "$desc" = "$PIN_DESC" ] || die "boot loader descriptor is not the pinned $CHIP one: $desc"
app=$(printf '%s\n' "$desc" | sed -n 's/.* app_version=\([0-9]*\) .*/\1/p')
[ -n "$app" ] || die "no app_version in the PROD descriptor"
echo "$app" > "$OUT/app_version.txt"
echo "$CHIP" > "$OUT/chip.txt"

# 3. GspSystemInfo: SYSINFO_TXT may name another copy of the text, not another text
pinned "$SI_TXT" "$PIN_SYSINFO_TXT"
"$TOOLS/ck_gsp_sysinfo_encode" encode "$SI_TXT" "$OUT/system_info.bin" > "$W/sysinfo.txt" ||
	die "system info refused: $(tail -1 "$W/sysinfo.txt")"
pinned "$OUT/system_info.bin" "$PIN_SYSINFO_BIN"

# 4. manifest
{
	echo "# window-1 boot package for $CHIP (family $FAMILY), built $(date -u +%FT%TZ) on $(hostname), extractors from $(cat "$ORACLE/.rev" 2>/dev/null || echo unknown)"
	echo "# firmware $FW sha256 $(sha "$FW"); bindata from $GEN; system info from $SI_TXT sha256 $(sha "$SI_TXT")"
	echo "app_version $app (RM_RISCV_UCODE_DESC.appVersion of UCODE_DESC_PROD, GspRmBoot_$CHIP)"
	echo "desc $desc"
	for f in rm_elf rm_sig bootloader fmc fmc_hash fmc_pkey fmc_sig system_info; do
		printf '%-16s %10s %s\n' "$f.bin" "$(stat -c %s "$OUT/$f.bin")" "$(sha "$OUT/$f.bin")"
	done
} > "$OUT/MANIFEST.txt"
cat "$OUT/MANIFEST.txt"
echo "WINDOW1_PACKAGE: PASS ($OUT)"
