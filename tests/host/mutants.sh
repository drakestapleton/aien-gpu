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
# parity_rm compiles NVIDIA's 580.173.02 SDK headers (Makefile NVSRC)
NVSRC=${NVSRC:-$HOME/workspace/_ext/open-gpu-kernel-modules-580.173.02/src}
CF="$CF -I $NVSRC/common/sdk/nvidia/inc -I $NVSRC/common/sdk/nvidia/inc/ctrl -I $NVSRC/common/inc"
srcs() { # test -> extra C sources
	case $1 in
	parity_boot) echo "$ORACLE/gpu/gsp_mmio.c $ORACLE/gpu/gsp_sim.c" ;;
	parity_boot_gb20b) echo "$ROOT/tools/gb20b_boot.c $ORACLE/dev/gb10_gsp_boot.c" ;;
	parity_seq) echo "$ORACLE/gpu/gsp_seq.c" ;;
	parity_mq) echo "$ORACLE/dev/gb10_gsp_boot.c" ;;
	parity_rm) echo "$ORACLE/dev/gb10_gsp_boot.c" ;;
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
ident|unit_test|if and ne arch 26 ne arch 27|if ne arch 26
ident|unit_test|if and ne arch 26 ne arch 27|if ne arch 27
ident|unit_test|if and ne arch 26 ne arch 27|if and ne arch 26 ne arch 28
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
rm|parity_rm|p32 m 84 19|p32 m 84 20
rm|parity_rm|p32 m 116 h|p32 m 116 0
rm|parity_rm|ahdr m get a 0 get a 1 get a 2 41068 20|ahdr m get a 0 get a 1 get a 2 41069 20
rm|parity_rm|p32 m 116 1|p32 m 116 3
rm|parity_rm|ch4 m 360 114 101 108 47|ch4 m 360 114 101 108 46
rmchan|parity_rm|shl div c 8 12 2097152|shl div c 8 12 1048576
rmchan|parity_rm|mdesc m 304 get a 12 512 0|mdesc m 304 get a 12 4096 0
rmchan|parity_rm|p32 m 356 or get a 21 28|p32 m 356 or get a 21 12
rmchan|parity_rm|if gt get a 7 4095|if gt get a 7 4096
rmctl|parity_rm|p32 m add o 24 add 5 get a add i 3|p32 m add o 24 add 4 get a add i 3
rmctl|parity_rm|p32 m 200 2|p32 m 200 1
rmctl|parity_rm|p32 m 88 2431713542|p32 m 88 2416967942
rmctl|parity_rm|put m 120 1|put m 120 0
rmctl|parity_rm|o = add 168 mul k 32|o = add 160 mul k 32
rmchk|parity_rm|if or ne get a 0 43 ne get a 1 19|if or ne get a 0 43 ne get a 1 20
rmchk|parity_rm|c = r32 m 1808|c = r32 m 1804
rmchk|parity_rm|if and eq ed m i 2 1 ne ed m i 12 0|if and eq ed m i 2 1 ne ed m i 12 1
rmchk|parity_rm|mul 4096 ms|mul 2048 ms
rmchk|parity_rm|if or eq c 52928 and eq c 52672 eq get a 0 0|if eq c 52928
rmchk|parity_rm|s = r32 m 96|s = r32 m 92
rmchk|parity_rm|if gt get a 1 63|if gt get a 1 64
rmarg|parity_rm|tok = or shl get a 0 16 get a 1|tok = or shl get a 0 15 get a 1
rmarg|parity_rm|ret or tok 1073741824|ret tok
rmarg|parity_rm|if or or gt get a 0 63 gt get a 1 4095 gt get a 2 1|if or or gt get a 0 63 gt get a 1 4095 gt get a 2 2
rmarg|parity_rm|while lt c n bound 4096|while le c n bound 4096
rmarg|parity_rm|if eq get a 0 2147483648|if eq get a 0 2147483649
rmarg|parity_rm|ret and shr get a 0 4 1|ret and shr get a 0 3 1
boot|parity_boot_gb20b|if ne pword pkt 8 n 56360962|if ne pword pkt 8 n 56360961
boot|parity_boot_gb20b|if ne pword pkt add 20 mul k 4 n 0|if ne pword pkt add 20 mul k 4 n 1
boot|parity_boot_gb20b|ret bact act 1 1504 0|ret bact act 1 1508 0
boot|parity_boot_gb20b|if eq obs 255|if eq obs 254
boot|parity_boot_gb20b|ret bact act 1 8653952 0|ret bact act 1 8653824 0
boot|parity_boot_gb20b|ret bact act 2 8653504 33554432|ret bact act 2 8653504 16777216
boot|parity_boot_gb20b|ret bact act 2 8653956 get st 11|ret bact act 2 8653952 get st 11
boot|parity_boot_gb20b|if ne and get st 29 4294967167 353427070|if ne and get st 29 4294967167 336649854
boot|parity_boot_gb20b|if ne get st 30 20|if ne get st 30 21
boot|parity_boot_gb20b|if ne get st 31 0|if ne get st 31 1
boot|parity_boot_gb20b|if eq and obs 4294967040 3135193344|if eq and obs 4294967040 0
boot|parity_boot_gb20b|if or eq obs 0 eq and obs 4294967040 3135193344|if eq obs 0
EOF
if [ "$killed" -ne "$total" ]; then echo "MUTANTS FAIL killed=$killed/$total"; exit 1; fi
echo "MUTANTS PASS killed=$killed/$total"
