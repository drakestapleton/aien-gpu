#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# ko-units.sh BUILD KO UNIT... -- prove that the built module holds the Omega units of this build.
# kmod/agpu_om.S places every unit with .incbin; the assembler does not report that file to the
# kernel build as a dependency, so a module could keep a unit from an earlier build (seen 2026-11-18:
# aien_gpu.ko with an om/ident.om two edits old). For every UNIT this takes the bytes of the module
# symbol agpu_om_UNIT (after its 4-byte landing pad) out of KO and requires them to equal
# BUILD/UNIT.bin, then repeats one comparison against a copy with one bit flipped, which must differ.
# Prints KO-UNITS PASS units=N control=caught, or KO-UNITS FAIL and what differs. Reads only.
set -u
B=${1:?usage: ko-units.sh BUILD KO UNIT...}
KO=${2:?usage: ko-units.sh BUILD KO UNIT...}
shift 2
[ $# -gt 0 ] || { echo "KO-UNITS FAIL no units named"; exit 1; }
T=$(mktemp -d /tmp/agpu-kounits.XXXXXX)
trap "rm -rf \"$T\"" EXIT
readelf -S -W "$KO" > "$T/sec" 2>/dev/null && readelf -s -W "$KO" > "$T/sym" 2>/dev/null ||
	{ echo "KO-UNITS FAIL cannot read $KO"; exit 1; }
# the unit of a symbol: section index, then that section's file offset (hex)
unit_bytes() { # unit out
	set -- "$1" "$2" $(awk -v s="agpu_om_$1" "\$8==s { print \$2, \$3, \$7 }" "$T/sym")
	[ $# -eq 5 ] || { echo "KO-UNITS FAIL no symbol agpu_om_$1 in $KO"; return 1; }
	off=$(awk -v n="$5" "{ gsub(/\\[ */, \"[\") } \$1==\"[\" n \"]\" { print \$5 }" "$T/sec")
	[ -n "$off" ] || { echo "KO-UNITS FAIL section $5 of agpu_om_$1 not found"; return 1; }
	[ "$4" -gt 4 ] || { echo "KO-UNITS FAIL agpu_om_$1 is $4 bytes"; return 1; }
	tail -c +$((0x$off + 0x$3 + 4 + 1)) "$KO" | head -c $(($4 - 4)) > "$2"
}
n=0
for u in "$@"; do
	[ -s "$B/$u.bin" ] || { echo "KO-UNITS FAIL $B/$u.bin missing"; exit 1; }
	unit_bytes "$u" "$T/$u.ko.bin" || exit 1
	if ! cmp -s "$T/$u.ko.bin" "$B/$u.bin"; then
		echo "KO-UNITS FAIL $u: the module holds $(sha256sum < "$T/$u.ko.bin" | cut -c1-16) ($(wc -c < "$T/$u.ko.bin") bytes), $B/$u.bin is $(sha256sum < "$B/$u.bin" | cut -c1-16) ($(wc -c < "$B/$u.bin") bytes): rebuild the module"
		exit 1
	fi
	n=$((n + 1))
done
# control: the first unit with its first byte changed must not compare equal
u=$1
{ printf "\\377"; tail -c +2 "$B/$u.bin"; } > "$T/flip.bin"
if cmp -s "$B/$u.bin" "$T/flip.bin" || cmp -s "$T/$u.ko.bin" "$T/flip.bin"; then
	echo "KO-UNITS FAIL control: a changed $u.bin compared equal"; exit 1
fi
echo "KO-UNITS PASS units=$n control=caught"
