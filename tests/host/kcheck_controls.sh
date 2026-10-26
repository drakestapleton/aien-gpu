#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# kcheck_controls.sh KCHECK -- negative and positive controls for tools/kcheck.om.
# Each instruction is assembled by the system assembler (a referee, not hand-written hex), appended
# to a copy of a unit that passes (build/ident.bin), and kcheck's verdict is compared with the
# expectation. Prints KCHECK-CONTROLS PASS n=N or the first wrong verdict; exit status 0 only on PASS.
set -u
KCHECK=${1:?usage: kcheck_controls.sh build/kcheck}
B=$(dirname "$KCHECK")
BASE=$B/ident.bin
T=$(mktemp -d /tmp/kcheck-ctl.XXXXXX)
trap 'rm -rf "$T"' EXIT
n=0
check() { # expect asm-lines...
	want=$1; shift
	printf '%s\n' "$@" > "$T/i.s"
	if ! ${CC:-gcc} -c -o "$T/i.o" -x assembler "$T/i.s" 2> "$T/as.err"; then
		echo "KCHECK-CONTROLS FAIL cannot assemble: $*"; cat "$T/as.err"; exit 1
	fi
	objcopy -O binary -j .text "$T/i.o" "$T/i.bin" || exit 1
	cat "$BASE" "$T/i.bin" > "$T/u.bin"
	if "$KCHECK" < "$T/u.bin" > "$T/out"; then got=PASS; else got=FAIL; fi
	if [ "$got" != "$want" ]; then
		echo "KCHECK-CONTROLS FAIL want $want got $got for: $* ($(cat "$T/out"))"; exit 1
	fi
	n=$((n + 1))
}
check PASS 'add x0, x0, #1'
check PASS 'sub sp, sp, #4096' 'add sp, sp, #4096'
check PASS 'ldr x0, [x1, x2, lsl #3]' 'strb w3, [x4, x5]' 'ldur x6, [x7, #-8]'
check PASS 'udiv x0, x1, x2' 'msub x3, x4, x5, x6' 'csinc x7, x8, x9, ne' 'lsl x0, x1, #3'
check FAIL 'mov x19, #1'
check FAIL 'mov x18, x0'
check FAIL 'add x0, x28, #1'
check FAIL 'mov x29, sp'
check FAIL 'svc #0'
check FAIL 'brk #0'
check FAIL 'fadd d0, d1, d2'
check FAIL 'ldr q0, [x0]'
check FAIL 'str d1, [x2, #8]'
check FAIL 'br x17'
check FAIL 'blr x16'
check FAIL 'ret x1'
check FAIL 'br x16'
check FAIL 'mrs x0, tpidr_el1'
check FAIL 'msr daifset, #2'
check FAIL 'b .+0x400000'
check FAIL 'cbz x0, .-0x100000'
check FAIL 'sub sp, sp, #4096' 'sub sp, sp, #4096'
check FAIL 'mov sp, x0'
check FAIL 'sub sp, sp, x1'
check FAIL 'stp x0, x1, [sp, #-16]!'
check FAIL 'ldr x0, [x1], #8'
check FAIL 'ldr x0, [x1, #8]!'
check FAIL 'adr x0, .'
check FAIL 'adrp x0, .'
check FAIL 'ldr x0, .'
check FAIL 'ldxr x0, [x1]'
check FAIL '.arch armv8.4-a' 'ldadd x0, x1, [x2]'
check FAIL '.arch armv8.4-a' 'paciasp'
check FAIL 'dmb ish'
check FAIL 'orr sp, x0, #1'
check FAIL '.inst 0x00000000'
echo "KCHECK-CONTROLS PASS n=$n"
