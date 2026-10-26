#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# mutants.sh OMC BUILD -- each line below plants one small mistake in one Omega unit (a literal,
# single-line replacement), rebuilds that unit with the pinned omc, links the host test that must
# notice it, and requires that test to FAIL. A mutant that still passes means the tests are too weak.
# Prints MUTANTS PASS killed=N/N or the surviving mutant; exit status 0 only when all are killed.
set -u
OMC=${1:?usage: mutants.sh OMC BUILD}
B=${2:?usage: mutants.sh OMC BUILD}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-gcc}
T=$(mktemp -d /tmp/agpu-mut.XXXXXX)
trap 'rm -rf "$T"' EXIT
ORACLE=$B/oracle/native/kernel
CF="-std=gnu11 -O2 -I $ROOT/tests/host -I $ORACLE/gpu -I $ORACLE/dev -Wno-unused-parameter"
srcs() { # test -> extra C sources
	case $1 in
	parity_boot) echo "$ORACLE/gpu/gsp_mmio.c $ORACLE/gpu/gsp_sim.c" ;;
	parity_seq) echo "$ORACLE/gpu/gsp_seq.c" ;;
	parity_mq) echo "$ORACLE/dev/gb10_gsp_boot.c" ;;
	*) echo "" ;;
	esac
}
killed=0
total=0
while IFS='|' read -r unit test from to; do
	[ -n "$unit" ] || continue
	total=$((total + 1))
	rm -rf "$T/u"; mkdir -p "$T/u"
	cp "$B"/*.bin "$T/u/"
	awk -v f="$from" -v t="$to" 'BEGIN{done=0} { if (!done) { i = index($0, f); if (i) { $0 = substr($0, 1, i-1) t substr($0, i+length(f)); done=1 } } print } END{ if (!done) exit 3 }' \
		"$ROOT/om/$unit.om" > "$T/m.om" || { echo "MUTANTS FAIL pattern not found in $unit: $from"; exit 1; }
	if ! "$OMC" < "$T/m.om" > "$T/u/$unit.bin" 2> "$T/omc.err"; then
		echo "mutant $total ($unit) refused by omc: $(cat "$T/omc.err")"; killed=$((killed + 1)); continue
	fi
	$CC -c -Wa,-I"$T/u" -o "$T/om.o" "$ROOT/kmod/agpu_om.S" || exit 1
	# shellcheck disable=SC2046
	$CC $CF -o "$T/t" "$ROOT/tests/host/$test.c" $(srcs "$test") "$T/om.o" || exit 1
	if "$T/t" > "$T/out" 2>&1; then
		echo "MUTANTS FAIL survivor $total: $unit '$from' -> '$to' ($(tail -1 "$T/out"))"; exit 1
	fi
	echo "mutant $total ($unit -> $test) killed: $(head -1 "$T/out")"
	killed=$((killed + 1))
done <<'EOF'
boot|parity_boot|ret bact act 2 8653828 sub mul dw 4 4|ret bact act 2 8653828 sub mul dw 4 0
boot|parity_boot|if ge n get st 4|if gt n get st 4
boot|parity_boot|ne and obs 4294967040 3135193344|ne and obs 4294967040 0
boot|parity_boot|if eq and obs 128 0|if eq and obs 64 0
boot|parity_boot|if eq obs shr pa 32|if eq obs shr pa 31
seal|parity_mq|put32 m 32 xor shr x 32 and x 4294967295|put32 m 32 and x 4294967295
seq|parity_seq|ret 5|ret 4
seq|parity_seq|ge get ops add base 2 8|ge get ops add base 2 9
seq|parity_seq|ret le add a 4 barlen|ret le a barlen
seq|parity_seq|if eq op 2|if eq op 9
seal|parity_mq|put32 m 68 4294967295|put32 m 68 0
mqput|parity_mq|room = rem sub add gsprd 63 add wr 1 63|room = rem sub add gsprd 63 wr 63
mqget|parity_mq|if ne ro32 out 36 rxseq|if ne ro32 out 36 ro32 out 36
mqget|parity_mq|if lt rd32 s 24 32|if lt rd32 s 24 0
ident|unit_test|if ne and shr boot42 20 15 11|if ne and shr boot42 20 15 12
abi|unit_test|for k ulong 52|for k ulong 51
abi|unit_test|if or eq p 0 gt p 1000|if or eq p 0 gt p 1001
gmmu|parity_gmmu|ret or or pa 16 shl ap 1|ret or or pa 8 shl ap 1
gmmu|parity_gmmu|pcf = add 16 and fl 1|pcf = add 0 and fl 1
gmmu|parity_gmmu|ap = add 2 shr fl 1|ap = add 3 shr fl 1
gmmu|parity_gmmu|ret 2147483649|ret 2147483648
gmmu|parity_gmmu|ret or shl and fr 268435455 4 2|ret or shl and fr 268435455 4 0
gmmu|parity_gmmu|ret and shr fr 28 1048575|ret and shr fr 27 1048575
gmmu|parity_gmmu|ret 47|ret 48
gmmu|parity_gmmu|if and ge lv 4 and ge va 4294967296 lt va 4831838208|if and ge lv 5 and ge va 4294967296 lt va 4831838208
memchk|parity_gmmu|if and lt va 4831838208 gt add va sz 4294967296|if and lt va 4831834112 gt add va sz 4294967296
memchk|parity_gmmu|if and lt va add ov os lt ov add va sz|if and le va add ov os lt ov add va sz
memchk|parity_gmmu|gt sz 68719476736|ge sz 68719476736
memchk|parity_gmmu|if eq n 256|if eq n 255
rpdes|parity_gmmu|sz = 16|sz = 4096
rpdes|parity_gmmu|ap = add 3 get lv 5|ap = add 2 get lv 5
rpdes|parity_gmmu|put p 32 uchar 5|put p 32 uchar 6
EOF
if [ "$killed" -ne "$total" ]; then echo "MUTANTS FAIL killed=$killed/$total"; exit 1; fi
echo "MUTANTS PASS killed=$killed/$total"
