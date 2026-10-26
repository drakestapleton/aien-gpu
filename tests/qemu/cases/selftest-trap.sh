# SPDX-License-Identifier: MIT OR GPL-2.0-only
# qemu-args:
#
# selftest_trap=1 makes the module's init self-test also feed an Omega unit an
# input that traps. The trap must be contained (reported, no splat) and the
# self-test must still report PASS after it.
. /cases/lib.sh

[ ! -e "$DEVNODE" ] || fail "$DEVNODE exists before load"
do_insmod selftest_trap=1
log=$(case_dmesg)
t=$(echo "$log" | grep -n "$TRAP_LINE" | head -n 1 | cut -d: -f1)
p=$(echo "$log" | grep -n "$SELFTEST_PASS" | tail -n 1 | cut -d: -f1)
[ -n "$t" ] || fail "no '$TRAP_LINE' line with selftest_trap=1"
[ -n "$p" ] || fail "no '$SELFTEST_PASS' line with selftest_trap=1"
[ "$t" -lt "$p" ] || fail "'$SELFTEST_PASS' (line $p) not after trap line (line $t)"
code=$(echo "$log" | grep -o "$TRAP_LINE[^ ]*" | head -n 1 | sed "s/^$TRAP_LINE//")
[ -z "$(bound_devices)" ] || fail "aien-gpu bound $(bound_devices) with no GB10 present"
[ ! -e "$DEVNODE" ] || fail "$DEVNODE exists with no GB10 present"
check_clean "after insmod selftest_trap=1"
do_rmmod
check_clean "after rmmod"
pass "trap contained code=$code then self-test PASS; rmmod clean, no splat"
