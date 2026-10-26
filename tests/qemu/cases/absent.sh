# SPDX-License-Identifier: MIT OR GPL-2.0-only
# qemu-args:
#
# No GB10 in the guest. The module must load, run its Omega self-test, register
# the PCI driver without binding anything or creating /dev/aien-gpu, and unload
# cleanly. Ten load/unload cycles.
. /cases/lib.sh

N=10
[ ! -e "$DEVNODE" ] || fail "$DEVNODE exists before first load"
i=1
while [ "$i" -le "$N" ]; do
	do_insmod
	n=$(dmesg | grep -c "$SELFTEST_PASS")
	[ "$n" -eq "$i" ] || fail "cycle $i: '$SELFTEST_PASS' seen $n times, want $i"
	[ -d "$DRV" ] || fail "cycle $i: $DRV missing after insmod"
	b=$(bound_devices)
	[ -z "$b" ] || fail "cycle $i: aien-gpu bound $b with no GB10 present"
	[ ! -e "$DEVNODE" ] || fail "cycle $i: $DEVNODE exists with no GB10 present"
	check_clean "cycle $i after insmod"
	do_rmmod
	[ ! -e "$DEVNODE" ] || fail "cycle $i: $DEVNODE exists after rmmod"
	check_clean "cycle $i after rmmod"
	i=$((i + 1))
done
pass "$N/$N load+unload cycles, self-test PASS x$N, nothing bound, no /dev/aien-gpu, no splat"
