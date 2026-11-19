#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# package-gb20b.sh ORACLE OUT [SYSINFO_TXT]
# Build the GB20B (Spark 0x1b/0xb) GSP boot package from NVIDIA 580.173.02 files on this machine
# (on loan, docs/BOUNDARY.md). Extracts .fwimage + .fwsignature_gb20y, GspRmBoot_GB20B (212992 B,
# monCodeOff=0xf000), GspRmFmcGfwProdSigned_GB20B (212296 B, sha384-checked), and system_info.bin,
# pinning every SHA-256. Writes only into OUT. Touches no device.
set -euo pipefail

ORACLE=${1:?usage: package-gb20b.sh ORACLE OUT [SYSINFO_TXT]}
OUT=${2:?usage: package-gb20b.sh ORACLE OUT [SYSINFO_TXT]}
SI_TXT=${3:-$ORACLE/evidence/own-gpu-driver/rung2/system_info.txt}
FW=/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin
GEN=${OGKM_DIR:-$HOME/workspace/_ext/open-gpu-kernel-modules-580.173.02}/src/nvidia/generated

# Exact pinned SHA-256 digests (NVIDIA 580.173.02, GB20B / gb20y)
PIN_FW_CONTAINER=32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963
PIN_FWIMAGE=931add19204a5f0240442272f58fb2bc5db6cec4dd6f6341c1d888754146aa07
PIN_SIG_GB20Y=6d38023b5b15a9e0d093be9dd443ebafb2aba3e0a3f1f260c387afc903a1f9a1
PIN_BOOT_PROD_GB20B=6da8510c912ccd5a8b73f9a5cce9e0f4ea657aa430431d2f8e4874834aeadb94
PIN_FMC_IMG_GB20B=b92f83c1250019af11162d9ee471b8fb74081ea598cf6added0bee7a3663e2be
PIN_FMC_HASH_GB20B=4a5854c785fa7a9ea4aaabb52f6c9838242f81506659ff413da0f4b8ac1f40bf
PIN_FMC_PKEY_GB20B=54076b108ad3da65e09f675d88efd553340a0c0653cf13372f4e30dd6ab17e7c
PIN_FMC_SIG_GB20B=6c5fbbf096efff9d0f47d4cc70096898f059579154464e6c3b972d2e606f1334

die() { echo "GB20B_PACKAGE: FAIL ($*)"; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

T=$(mktemp -d /tmp/agpu-gb20b-tools.XXXXXX)
trap 'rm -rf "$T"' EXIT
make -s -C "$ORACLE/native/kernel" OUT="$T" gsp-fw-inspect gsp-bindata-extract "$T/host/ck_gsp_sysinfo_encode" >&2
TOOLS="$T/host"

mkdir -p "$OUT"
[[ -z $(ls -A "$OUT") ]] || die "$OUT is not empty"
W="$OUT/work"
mkdir -p "$W"

# 1. GSP-RM: .fwimage + .fwsignature_gb20y (NV_FIRMWARE_CHIP_FAMILY_GB20Y)
[[ $(sha "$FW") == "$PIN_FW_CONTAINER" ]] || die "$FW container sha256 mismatch"
"$TOOLS/ck_gsp_fw_inspect" inspect "$FW" --family gb20y --expect-version 580.173.02 > "$W/inspect.txt" || die "fw inspect refused"
grep -q '^RESULT OK' "$W/inspect.txt" || die "fw inspect not OK"
cutsec() {
	local off size
	read -r off size < <(awk -v n="$1" '$1=="section" && $3==n { sub("offset=","",$5); sub("size=","",$6); print $5, $6 }' "$W/inspect.txt")
	[[ -n ${off:-} && -n ${size:-} ]] || die "section $1 missing"
	tail -c +$((off + 1)) "$FW" | head -c "$size" > "$2"
	[[ $(stat -c %s "$2") -eq $size ]] || die "short cut of $1"
}
cutsec .fwimage "$OUT/rm_elf.bin"
cutsec .fwsignature_gb20y "$OUT/rm_sig.bin"
[[ $(sha "$OUT/rm_elf.bin") == "$PIN_FWIMAGE" ]] || die ".fwimage sha256 mismatch"
[[ $(sha "$OUT/rm_sig.bin") == "$PIN_SIG_GB20Y" ]] || die ".fwsignature_gb20y sha256 mismatch"

# 2. GspRmBoot_GB20B + GspRmFmcGfwProdSigned_GB20B
"$TOOLS/ck_gsp_bindata_extract" extract "$GEN/g_bindata_kgspGetBinArchiveGspRmBoot_GB20B.c" --out "$W" > "$W/boot.txt" || die "boot bindata refused"
"$TOOLS/ck_gsp_bindata_extract" extract "$GEN/g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_GB20B.c" --out "$W" > "$W/fmc.txt" || die "fmc bindata refused"
B="$W/kgspGetBinArchiveGspRmBoot_GB20B"
M="$W/kgspGetBinArchiveGspRmFmcGfwProdSigned_GB20B"
cp "$B.UCODE_IMAGE_PROD.bin" "$OUT/bootloader.bin"
cp "$M.UCODE_IMAGE.bin" "$OUT/fmc.bin"
cp "$M.UCODE_HASH.bin" "$OUT/fmc_hash.bin"
cp "$M.UCODE_PKEY.bin" "$OUT/fmc_pkey.bin"
cp "$M.UCODE_SIG.bin" "$OUT/fmc_sig.bin"

[[ $(sha "$OUT/bootloader.bin") == "$PIN_BOOT_PROD_GB20B" ]] || die "GB20B boot PROD sha256 mismatch"
[[ $(sha "$OUT/fmc.bin") == "$PIN_FMC_IMG_GB20B" ]] || die "GB20B FMC image sha256 mismatch"
[[ $(sha "$OUT/fmc_hash.bin") == "$PIN_FMC_HASH_GB20B" ]] || die "GB20B FMC hash sha256 mismatch"
[[ $(sha "$OUT/fmc_pkey.bin") == "$PIN_FMC_PKEY_GB20B" ]] || die "GB20B FMC pkey sha256 mismatch"
[[ $(sha "$OUT/fmc_sig.bin") == "$PIN_FMC_SIG_GB20B" ]] || die "GB20B FMC sig sha256 mismatch"
grep -q '^hash384 UCODE_HASH equals sha384(UCODE_IMAGE)' "$W/fmc.txt" || die "FMC sha384 check missing"

app=$(sed -n 's/^desc UCODE_DESC_PROD .* app_version=\([0-9]*\) .*/\1/p' "$W/boot.txt")
bl_man=$(sed -n 's/^desc UCODE_DESC_PROD .* manifest=\(0x[0-9a-f]*\)+.*/\1/p' "$W/boot.txt")
bl_dat=$(sed -n 's/^desc UCODE_DESC_PROD .* monitor_data=\(0x[0-9a-f]*\)+.*/\1/p' "$W/boot.txt")
bl_cod=$(sed -n 's/^desc UCODE_DESC_PROD .* monitor_code=\(0x[0-9a-f]*\)+.*/\1/p' "$W/boot.txt")
[[ -n $app && -n $bl_man && -n $bl_dat && -n $bl_cod ]] || die "UCODE_DESC_PROD fields missing"
echo "$app" > "$OUT/app_version.txt"
echo "gb20b" > "$OUT/profile.txt"
cat > "$OUT/bl_desc.txt" <<DESC
code=$bl_cod
data=$bl_dat
manifest=$bl_man
size=$(stat -c %s "$OUT/bootloader.bin")
app_version=$app
DESC

# 3. GspSystemInfo
"$TOOLS/ck_gsp_sysinfo_encode" encode "$SI_TXT" "$OUT/system_info.bin" > "$W/sysinfo.txt" || die "system info refused"

# 4. Manifest
{
	echo "# GB20B boot package, built $(date -u +%FT%TZ) on $(hostname)"
	echo "# profile gb20b, signature .fwsignature_gb20y, boot GspRmBoot_GB20B, fmc GspRmFmcGfwProdSigned_GB20B"
	echo "app_version $app"
	echo "bl_desc code=$bl_cod data=$bl_dat manifest=$bl_man"
	for f in rm_elf.bin rm_sig.bin bootloader.bin fmc.bin fmc_hash.bin fmc_pkey.bin fmc_sig.bin system_info.bin; do
		printf "%-18s %8s %s\n" "$f" "$(stat -c %s "$OUT/$f")" "$(sha "$OUT/$f")"
	done
} > "$OUT/MANIFEST.txt"
cat "$OUT/MANIFEST.txt"
echo "GB20B_PACKAGE: PASS ($OUT)"
