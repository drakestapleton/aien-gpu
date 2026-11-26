#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# boundary.sh OMC BUILD UNIT TEST [UNIT TEST ...] -- off-by-one mutants, generated, not hand-picked.
# For every code line of om/UNIT.om that holds a comparison (lt gt ge le eq ne) other than one on the
# step counter `pc`, every decimal literal on that line is changed by +1 and by -1 (not below 0), one
# change per mutant. Each mutant is built with the pinned omc and linked into TEST, which must FAIL.
# A mutant that still passes is a survivor: either a test is missing (add it) or the change cannot be
# observed (list it in tests/host/boundary_equivalent.txt with the reason). The gate fails on a
# survivor that is not listed, and on a listed one that is killed (a stale entry).
# List format: UNIT|N|TOKEN|DELTA|LINE|REASON, N = which line with that text (1 = first), TOKEN = the
# position of the literal on the line, DELTA = +1 or -1.
# Prints BOUNDARY PASS units=.. mutants=.. killed=.. equivalent=.., exit 0; else BOUNDARY FAIL.
set -u
OMC=${1:?usage: boundary.sh OMC BUILD UNIT TEST ...}
B=${2:?usage: boundary.sh OMC BUILD UNIT TEST ...}
shift 2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-gcc}
EQ=$ROOT/tests/host/boundary_equivalent.txt
T=$(mktemp -d /tmp/agpu-bnd.XXXXXX)
trap 'rm -rf "$T"' EXIT
ORACLE=$B/oracle/native/kernel
NVSRC=${NVSRC:-$HOME/workspace/_ext/open-gpu-kernel-modules-580.173.02/src}
CF="-std=gnu11 -O2 -I $ROOT/tests/host -I $ORACLE/gpu -I $ORACLE/dev -Wno-unused-parameter"
CF="$CF -I $NVSRC/common/sdk/nvidia/inc -I $NVSRC/common/sdk/nvidia/inc/ctrl -I $NVSRC/common/inc"
srcs() { # test -> extra C sources (as tests/host/mutants.sh)
	case $1 in
	parity_boot) echo "$ROOT/tests/host/gb20b_mmio.c $ROOT/tests/host/gb20b_sim.c" ;;
	parity_seq) echo "$ORACLE/gpu/gsp_seq.c" ;;
	parity_mq | parity_rm) echo "$ORACLE/dev/gb10_gsp_boot.c" ;;
	*) echo "" ;;
	esac
}
grep -v '^#' "$EQ" 2>/dev/null | grep . > "$T/eq" || : > "$T/eq"
: > "$T/used"
# keys = the first five fields; a line without a reason is refused
while IFS='|' read -r u n t d l reason; do
	[ -n "$reason" ] || { echo "BOUNDARY FAIL $EQ: no reason on: $u|$n|$t|$d|$l"; exit 1; }
	echo "$u|$n|$t|$d|$l"
done < "$T/eq" > "$T/eqkeys" || exit 1
units='' total=0 killed=0 equiv=0 bad=0
while [ $# -ge 2 ]; do
	unit=$1 test=$2
	shift 2
	units="$units $unit"
	# lineno|occurrence|token|delta|original line|mutated line
	awk '
	/^note/ { next }
	{
		n = split($0, tk, " "); cmp = 0; pc = 0
		for (i = 1; i <= n; i++)
			if (tk[i] ~ /^(lt|gt|ge|le|eq|ne)$/) { cmp = 1; if (tk[i + 1] == "pc") pc = 1 }
		seen[$0]++
		if (!cmp || pc) next
		for (i = 1; i <= n; i++) {
			if (tk[i] !~ /^[0-9]+$/) continue
			for (d = -1; d <= 1; d += 2) {
				v = tk[i] + d
				if (v < 0) continue
				line = ""
				for (k = 1; k <= n; k++) line = line (k > 1 ? " " : "") (k == i ? sprintf("%.0f", v) : tk[k])
				printf "%d|%d|%d|%s|%s|%s\n", NR, seen[$0], i, (d > 0 ? "+1" : "-1"), $0, line
			}
		}
	}' "$ROOT/om/$unit.om" > "$T/list"
	[ -s "$T/list" ] || { echo "BOUNDARY FAIL $unit: no comparison with a literal found"; exit 1; }
	while IFS='|' read -r ln occ tok delta orig mut; do
		total=$((total + 1))
		rm -rf "$T/u"; mkdir -p "$T/u"
		cp "$B"/*.bin "$T/u/"
		awk -v ln="$ln" -v new="$mut" '{ if (NR == ln) print new; else print }' "$ROOT/om/$unit.om" > "$T/m.om"
		key="$unit|$occ|$tok|$delta|$orig"
		listed=0
		grep -q -F -x -e "$key" "$T/eqkeys" 2>/dev/null && listed=1
		if ! "$OMC" < "$T/m.om" > "$T/u/$unit.bin" 2> "$T/omc.err"; then
			killed=$((killed + 1)); alive=0
		else
			$CC -c -Wa,-I"$T/u" -o "$T/om.o" "$ROOT/kmod/agpu_om.S" || exit 1
			# shellcheck disable=SC2046
			$CC $CF -o "$T/t" "$ROOT/tests/host/$test.c" $(srcs "$test") "$T/om.o" || exit 1
			if "$T/t" > "$T/out" 2>&1; then alive=1; else alive=0; killed=$((killed + 1)); fi
		fi
		if [ $alive = 1 ] && [ $listed = 1 ]; then
			equiv=$((equiv + 1)); echo "$key" >> "$T/used"
		elif [ $alive = 1 ]; then
			bad=$((bad + 1)); echo "BOUNDARY survivor $unit:$ln token $tok $delta: '$orig' -> '$mut'"
			echo "  list line: $key|REASON"
		elif [ $listed = 1 ]; then
			bad=$((bad + 1)); echo "BOUNDARY stale entry (the mutant is killed): $key"
		fi
	done < "$T/list"
done
sort "$T/eqkeys" > "$T/a"; sort "$T/used" > "$T/b"
if ! cmp -s "$T/a" "$T/b"; then
	comm -23 "$T/a" "$T/b" | sed "s/^/BOUNDARY list entry matches no mutant of the units run: /"
	bad=$((bad + 1))
fi
if [ "$bad" -ne 0 ] || [ $((killed + equiv)) -ne "$total" ]; then
	echo "BOUNDARY FAIL units=$units mutants=$total killed=$killed equivalent=$equiv problems=$bad"; exit 1
fi
echo "BOUNDARY PASS units=$units mutants=$total killed=$killed equivalent=$equiv"
