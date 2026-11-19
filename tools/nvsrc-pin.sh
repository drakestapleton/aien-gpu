#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# nvsrc-pin.sh NVSRC PIN CC ARGS... -- pin the files of NVIDIA's source tree that the gates read.
# NVSRC is the src/ directory of open-gpu-kernel-modules 580.173.02; it is not part of this
# repository. The files read are: every header the compiler opens for tests/host/parity_rm.c (the
# command CC ARGS... must print its dependency list, i.e. contain -M), and the two GB20B bindata
# sources tools/package.sh extracts. Each file's sha256 with its path below NVSRC goes into one
# sorted list; the sha256 of that list must equal PIN. A changed, missing or extra file fails.
# PIN = print: show the value instead of comparing.
# Prints NVSRC PASS files=N sha256=..., exit 0; else NVSRC FAIL with the reason.
set -u
NVSRC=${1:?usage: nvsrc-pin.sh NVSRC PIN CC ARGS...}
PIN=${2:?usage: nvsrc-pin.sh NVSRC PIN CC ARGS...}
shift 2
T=$(mktemp -d /tmp/agpu-nvsrc.XXXXXX)
trap 'rm -rf "$T"' EXIT
root=$(cd "$NVSRC" 2>/dev/null && pwd -P) || { echo "NVSRC FAIL $NVSRC is not a directory"; exit 1; }
"$@" > "$T/deps" 2> "$T/err" || { cat "$T/err"; echo "NVSRC FAIL the dependency list could not be made"; exit 1; }
{
	tr ' \\' '\n\n' < "$T/deps" | grep '/' | grep -v ':$'
	echo "$root/nvidia/generated/g_bindata_kgspGetBinArchiveGspRmBoot_GB20B.c"
	echo "$root/nvidia/generated/g_bindata_kgspGetBinArchiveGspRmFmcGfwProdSigned_GB20B.c"
} | while read -r f; do
	[ -n "$f" ] || continue
	r=$(realpath -q "$f") || { echo "MISSING $f"; continue; }
	case $r in "$root"/*) echo "${r#"$root"/}" ;; esac
done | sort -u > "$T/files"
if grep -q '^MISSING ' "$T/files"; then grep '^MISSING ' "$T/files" | head -3; echo "NVSRC FAIL a file is missing"; exit 1; fi
n=$(wc -l < "$T/files")
[ "$n" -ge 10 ] || { echo "NVSRC FAIL only $n files of $NVSRC in the dependency list"; exit 1; }
(cd "$root" && xargs sha256sum < "$T/files") > "$T/sums" || { echo "NVSRC FAIL a file could not be read"; exit 1; }
got=$(sha256sum < "$T/sums" | cut -d' ' -f1)
if [ "$PIN" = print ]; then echo "NVSRC files=$n sha256=$got"; exit 0; fi
if [ "$got" != "$PIN" ]; then
	echo "NVSRC FAIL the $n files the gates read from $NVSRC hash to $got, pinned $PIN"; exit 1
fi
echo "NVSRC PASS files=$n sha256=$got"
