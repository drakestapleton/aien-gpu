#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# static-checks.sh KDIR -- the kernel's own static checkers on kmod/:
#   1. checkpatch.pl --strict --no-tree on every C/header file of the module (warnings and errors fail);
#   2. sparse (C=2 rebuild) when a sparse binary is on PATH or in SPARSE; otherwise prints NOT_RUN.
# Prints STATIC PASS or STATIC FAIL, or STATIC PASS-PARTIAL with the NOT_RUN reason.
set -u
KDIR=${1:?usage: static-checks.sh KDIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OMBUILD=${OMBUILD:-$ROOT/build}
rc=0
cp=$KDIR/scripts/checkpatch.pl
if [ ! -x "$cp" ] && [ ! -f "$cp" ]; then echo "STATIC FAIL no checkpatch.pl in $KDIR"; exit 1; fi
for f in "$ROOT"/kmod/*.c "$ROOT"/kmod/*.h; do
	out=$(perl "$cp" --strict --no-tree --terse --show-types --ignore SPDX_LICENSE_TAG,FILE_PATH_CHANGES -f "$f" 2>&1)
	if printf '%s\n' "$out" | grep -q -E ': (ERROR|WARNING|CHECK):'; then
		printf '%s\n' "$out" | grep -E ': (ERROR|WARNING|CHECK):'
		rc=1
	fi
done
[ $rc -eq 0 ] && echo "checkpatch: clean ($(ls "$ROOT"/kmod/*.c "$ROOT"/kmod/*.h | wc -l) files)"
SP=${SPARSE:-$(command -v sparse || true)}
if [ -z "$SP" ]; then
	echo "sparse: NOT_RUN (no sparse binary; set SPARSE=/path/to/sparse)"
	[ $rc -eq 0 ] && { echo "STATIC PASS-PARTIAL sparse NOT_RUN"; exit 0; }
	echo "STATIC FAIL"; exit 1
fi
log=$(mktemp)
make -C "$KDIR" M="$ROOT/kmod" OMBUILD="$OMBUILD" CC="${KCC:-$(command -v aarch64-linux-gnu-gcc-13 || echo gcc)}" \
	C=2 CHECK="$SP" modules > "$log" 2>&1 || { cat "$log"; echo "STATIC FAIL build"; exit 1; }
if grep -E '^[^ ]*aien_gpu[^:]*\.[ch]:[0-9]+:[0-9]+: (warning|error)' "$log"; then
	rc=1
else
	echo "sparse: clean ($SP)"
fi
rm -f "$log"
[ $rc -eq 0 ] && echo "STATIC PASS" && exit 0
echo "STATIC FAIL"
exit 1
