#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# ko-units.sh BUILD KO UNIT... -- prove that the built module holds the Omega units of this build.
# kmod/agpu_om.S places every unit with .incbin; the assembler does not report that file to the
# kernel build as a dependency, so a module could keep a unit from an earlier build (seen 2026-11-18:
# aien_gpu.ko with an om/ident.om two edits old). For every UNIT this takes the bytes of the module
# symbol agpu_om_UNIT (after its 4-byte landing pad) out of KO and requires them to equal
# BUILD/UNIT.bin, then flips one bit (XOR 0x01) of the first unit byte inside a copy of KO,
# re-extracts it through unit_bytes(), and requires the comparison to catch the mismatch.
# Prints KO-UNITS PASS units=N control=caught, or KO-UNITS FAIL and what differs. Reads only.
set -u
B=${1:?usage: ko-units.sh BUILD KO UNIT...}
KO=${2:?usage: ko-units.sh BUILD KO UNIT...}
shift 2
[ $# -gt 0 ] || { echo "KO-UNITS FAIL no units named"; exit 1; }
T=$(mktemp -d /tmp/agpu-kounits.XXXXXX)
trap "rm -rf \"$T\"" EXIT
readelf -S -W "$KO" > "$T/sec" 2>/dev/null && readelf -s -W --sym-base=10 "$KO" > "$T/sym" 2>/dev/null ||
	{ echo "KO-UNITS FAIL cannot read $KO"; exit 1; }
# the unit of a symbol: section index, then that section's file offset (hex), symbol size (decimal)
unit_pos() { # unit -> prints "<file_byte_offset> <payload_len>"
	set -- "$1" $(awk -v s="agpu_om_$1" "\$8==s { print \$2, \$3, \$7 }" "$T/sym")
	[ $# -eq 4 ] || { echo "KO-UNITS FAIL no symbol agpu_om_$1 in $KO" >&2; return 1; }
	off=$(awk -v n="$4" "{ gsub(/\\[ */, \"[\") } \$1==\"[\" n \"]\" { print \$5 }" "$T/sec")
	[ -n "$off" ] || { echo "KO-UNITS FAIL section $4 of agpu_om_$1 not found" >&2; return 1; }
	[ "$3" -gt 4 ] || { echo "KO-UNITS FAIL agpu_om_$1 is $3 bytes" >&2; return 1; }
	echo "$((0x$off + 0x$2 + 4)) $(($3 - 4))"
}
unit_bytes() { # ko unit out
	pos=$(unit_pos "$2") || return 1
	set -- "$1" "$3" $pos
	tail -c +$(($3 + 1)) "$1" | head -c "$4" > "$2"
}
n=0
for u in "$@"; do
	[ -s "$B/$u.bin" ] || { echo "KO-UNITS FAIL $B/$u.bin missing"; exit 1; }
	unit_bytes "$KO" "$u" "$T/$u.ko.bin" || exit 1
	if ! cmp -s "$T/$u.ko.bin" "$B/$u.bin"; then
		echo "KO-UNITS FAIL $u: the module holds $(sha256sum < "$T/$u.ko.bin" | cut -c1-16) ($(wc -c < "$T/$u.ko.bin") bytes), $B/$u.bin is $(sha256sum < "$B/$u.bin" | cut -c1-16) ($(wc -c < "$B/$u.bin") bytes): rebuild the module"
		exit 1
	fi
	n=$((n + 1))
done
# control: flip one bit (XOR 0x01) of the first unit's first byte inside a copy of KO and re-extract
u=$1
pos=$(unit_pos "$u") || exit 1
set -- $pos
foff=$1
cp "$KO" "$T/flip.ko" || exit 1
orig=$(od -An -tu1 -j "$foff" -N 1 "$T/flip.ko" | tr -d " \n")
flipped=$((orig ^ 1))
printf "\$(printf "%03o" "$flipped")" | dd of="$T/flip.ko" bs=1 seek="$foff" count=1 conv=notrunc 2>/dev/null || exit 1
unit_bytes "$T/flip.ko" "$u" "$T/flip.ko.bin" || exit 1
if cmp -s "$T/flip.ko.bin" "$B/$u.bin"; then
	echo "KO-UNITS FAIL control: a 1-bit flipped $u inside copy of $KO compared equal"; exit 1
fi
echo "KO-UNITS PASS units=$n control=caught"
