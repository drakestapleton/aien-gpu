#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# claims-lint.sh [--selftest] [ROOT] -- the claims of this repository's text that a machine can check.
#   1. A line that names Wolfram as the source of a result must be one of the lines of
#      tools/claims-baseline.txt (written before this check existed, results not kept) or say
#      "not kept". A new one fails: cite an aien-calc receipt in docs/calc-receipts/ instead.
#   2. Every path of this repository that a document quotes in backquotes exists (docs/, tools/,
#      tests/, om/, kmod/, window/; build products excepted; a quoted path that is a file of another
#      repository is listed in the baseline as PATH|DOCUMENT|PATH).
#   3. Every receipt named in the table of docs/STATUS.md is a file of docs/calc-receipts/, and every
#      file there is an aien-calc receipt.
#   4. docs/evidence/SHA256SUMS holds for every file of docs/evidence/.
# --selftest: a copy of the tree must pass, and four copies with one planted fault each must fail.
# Prints CLAIMS PASS ..., exit 0; else each failure and CLAIMS FAIL.
set -u
SELF=0
[ "${1:-}" = --selftest ] && { SELF=1; shift; }
ROOT=${1:-$(cd "$(dirname "$0")/.." && pwd)}
T=$(mktemp -d /tmp/agpu-claims.XXXXXX)
trap 'rm -rf "$T"' EXIT
DIRS="docs om kmod tools tests window"
TOP="README.md REVIEW-LESSONS.md Makefile"

lint() { # root -> prints failures, last line CLAIMS PASS/FAIL; status 0/1
	(
	cd "$1" || exit 1
	bad=0
	# 1
	grep -r -n -i -I "wolfram" $DIRS $TOP 2>/dev/null |
		grep -v -e '^tools/claims-lint.sh:' -e '^tools/claims-baseline.txt:' -e '^REVIEW-LESSONS.md:' \
			-e '/build/' > "$T/w" || :
	nb=0
	while IFS= read -r l; do
		f=${l%%:*}; rest=${l#*:}; text=${rest#*:}
		if grep -q -F -x -e "$f|$text" tools/claims-baseline.txt 2>/dev/null; then nb=$((nb + 1)); continue; fi
		case $text in *"not kept"*) continue ;; esac
		echo "claims: $f:${rest%%:*}: a Wolfram result with nothing kept; cite an aien-calc receipt"; bad=1
	done < "$T/w"
	# 2
	np=0
	for d in docs/*.md docs/proofs/README.md README.md REVIEW-LESSONS.md window/README.md; do
		[ -f "$d" ] || continue
		grep -o '`[^`]*`' "$d" | tr -d '`' | grep -E '^(docs|tools|tests|om|kmod|window)/[A-Za-z0-9_.*/-]+(\[[a-z]+\])?$' |
			grep -v -E '\.(ko|o|bin)$' | sort -u > "$T/p"
		while IFS= read -r p; do
			np=$((np + 1))
			# a file of another repository, listed as PATH|DOCUMENT|PATH in the baseline
			grep -q -F -x -e "PATH|$d|$p" tools/claims-baseline.txt 2>/dev/null && continue
			# shellcheck disable=SC2086
			ls -d $p > /dev/null 2>&1 || { echo "claims: $d names \`$p\`, which does not exist"; bad=1; }
		done < "$T/p"
	done
	# 3
	nr=0
	grep -o '| `[0-9a-f]\{16\}` |$' docs/STATUS.md | tr -d '|` ' > "$T/r"
	while IFS= read -r id; do
		nr=$((nr + 1))
		ls docs/calc-receipts/"$id"*.json > /dev/null 2>&1 ||
			{ echo "claims: docs/STATUS.md names receipt $id, not in docs/calc-receipts/"; bad=1; }
	done < "$T/r"
	[ "$nr" -gt 0 ] || { echo "claims: no receipt table found in docs/STATUS.md"; bad=1; }
	for j in docs/calc-receipts/*.json; do
		grep -q '"schema":"aien-calc-receipt"' "$j" || { echo "claims: $j is not an aien-calc receipt"; bad=1; }
	done
	# 4
	ne=$(find docs/evidence -type f ! -name SHA256SUMS | wc -l)
	(cd docs/evidence && sha256sum -c --quiet SHA256SUMS > /dev/null 2>&1) ||
		{ echo "claims: docs/evidence/SHA256SUMS does not hold"; bad=1; }
	[ "$ne" = "$(grep -c . docs/evidence/SHA256SUMS)" ] ||
		{ echo "claims: docs/evidence has $ne files, SHA256SUMS lists $(grep -c . docs/evidence/SHA256SUMS)"; bad=1; }
	if [ $bad = 0 ]; then
		echo "CLAIMS PASS wolfram-baseline=$nb paths=$np receipts=$nr evidence=$ne"
	else
		echo "CLAIMS FAIL"
	fi
	exit $bad
	)
}

if [ $SELF = 0 ]; then lint "$ROOT"; exit $?; fi
copy() { rm -rf "$T/c"; mkdir "$T/c"; (cd "$ROOT" && tar -cf - --exclude=build --exclude='*.o' --exclude='*.ko' $DIRS $TOP) | tar -xf - -C "$T/c"; }
copy; lint "$T/c" > "$T/out" || { cat "$T/out"; echo "CLAIMS-SELFTEST FAIL the untouched copy does not pass"; exit 1; }
n=0
for fault in wolfram path receipt evidence; do
	copy
	case $fault in
	wolfram) echo "The value 12 was checked (Wolfram verified)." >> "$T/c/docs/STATUS.md" ;;
	path) echo 'See `tools/no-such-file.sh`.' >> "$T/c/docs/STATUS.md" ;;
	receipt) rm -f "$(ls "$T/c"/docs/calc-receipts/*.json | head -1)"
		id=$(grep -o '| `[0-9a-f]\{16\}` |$' "$T/c/docs/STATUS.md" | head -1 | tr -d '|` ')
		rm -f "$T/c"/docs/calc-receipts/"$id"*.json ;;
	evidence) f=$(find "$T/c/docs/evidence" -type f ! -name SHA256SUMS | head -1); echo x >> "$f" ;;
	esac
	if lint "$T/c" > "$T/out"; then echo "CLAIMS-SELFTEST FAIL the planted fault '$fault' passes"; exit 1; fi
	n=$((n + 1))
done
echo "CLAIMS-SELFTEST PASS controls=$n caught=$n"
