# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# Shared helpers for guest case scripts (busybox sh). Sourced by each case.
# Environment from /aien/env: CASE, MOD_KO (guest path), MOD_NAME.
# A case reports through /tmp/detail and its exit status (0 = PASS).

DRV=/sys/bus/pci/drivers/aien-gpu
DEVNODE=/dev/aien-gpu
SELFTEST_PASS='aien-gpu: omega self-test PASS'
TRAP_LINE='aien-gpu: omega trap contained code='
# Kernel splat patterns. Matched only in dmesg after the case start marker.
SPLAT='Oops|BUG:|WARNING:|Call trace|Unable to handle|Kernel panic|general protection'
# Taint bits that mean something went wrong: D (died, 128), W (warned, 512),
# B (bad page, 32), L (soft lockup, 16384). Unsigned/out-of-tree taints are expected.
BAD_TAINT=$((32 | 128 | 512 | 16384))
TAINT0=$(cat /proc/sys/kernel/tainted)

fail() {
	echo "$*" > /tmp/detail
	echo "case $CASE: FAIL: $*"
	exit 1
}

pass() {
	echo "$*" > /tmp/detail
	exit 0
}

# dmesg since the case start marker written by /init.
case_dmesg() {
	dmesg | sed -n '/aien-qemu: case .* start/,$p'
}

check_clean() {
	splat=$(case_dmesg | grep -E "$SPLAT" | head -n 1)
	[ -z "$splat" ] || fail "$1: kernel splat: $splat"
	t=$(cat /proc/sys/kernel/tainted)
	new=$(( (t & ~TAINT0) & BAD_TAINT ))
	[ "$new" -eq 0 ] || fail "$1: new bad taint bits $new (tainted=$t, at start $TAINT0)"
}

do_insmod() {
	insmod "$MOD_KO" "$@" || fail "insmod $MOD_KO $* exited $?"
	grep -q "^$MOD_NAME " /proc/modules || fail "insmod $*: $MOD_NAME not in /proc/modules"
}

do_rmmod() {
	rmmod "$MOD_NAME" || fail "rmmod $MOD_NAME exited $?"
	if lsmod | grep -q "^$MOD_NAME "; then
		fail "lsmod still lists $MOD_NAME after rmmod"
	fi
	[ ! -e "$DRV" ] || fail "$DRV still present after rmmod"
}

# Print PCI functions bound to the aien-gpu driver (BDF symlinks in $DRV).
bound_devices() {
	for e in "$DRV"/*:*:*.*; do
		[ -e "$e" ] && basename "$e"
	done
}

# find_pci VENDOR DEVICE -> sysfs path of the first match, empty if none.
find_pci() {
	for d in /sys/bus/pci/devices/*; do
		[ -r "$d/vendor" ] || continue
		if [ "$(cat "$d/vendor")" = "$1" ] && [ "$(cat "$d/device")" = "$2" ]; then
			echo "$d"
			return 0
		fi
	done
	return 0
}

# refusal_case [insmod params...]
# Requires -device pci-testdev (1b36:0005). Loads the module, offers the
# device via new_id and checks that the probe refused it.
refusal_case() {
	dev=$(find_pci 0x1b36 0x0005)
	[ -n "$dev" ] || fail "precondition: pci-testdev 1b36:0005 not in guest (need -device pci-testdev)"
	bdf=$(basename "$dev")
	pre=none
	[ -e "$dev/driver" ] && pre=$(basename "$(readlink "$dev/driver")")
	[ "$pre" = none ] || fail "precondition: $bdf already bound to $pre, new_id cannot reach aien-gpu"
	[ ! -e "$DEVNODE" ] || fail "$DEVNODE exists before load"

	do_insmod "$@"
	[ -d "$DRV" ] || fail "$DRV missing after insmod"
	wrc=0
	echo "1b36 0005" > "$DRV/new_id" || wrc=$?
	sleep 1

	drv=none
	[ -e "$dev/driver" ] && drv=$(basename "$(readlink "$dev/driver")")
	[ "$drv" != aien-gpu ] || fail "$bdf bound to aien-gpu (driver symlink)"
	[ ! -e "$DRV/$bdf" ] || fail "$bdf listed under $DRV"
	[ -z "$(bound_devices)" ] || fail "aien-gpu has bound devices: $(bound_devices)"
	refusal=$(case_dmesg | grep 'aien-gpu' | grep -i 'refus' | head -n 1)
	[ -n "$refusal" ] || fail "no dmesg line with 'aien-gpu' and 'refus' after new_id (new_id write rc=$wrc)"
	[ ! -e "$DEVNODE" ] || fail "$DEVNODE exists after refused probe"
	enable=$(cat "$dev/enable")
	check_clean "after probe"

	do_rmmod
	[ ! -e "$DEVNODE" ] || fail "$DEVNODE exists after rmmod"
	check_clean "after rmmod"
	# Strip the "[  t.tttttt] " timestamp for the detail line.
	REFUSAL=$(echo "$refusal" | sed 's/^\[[^]]*\] *//')
	BDF=$bdf
	ENABLE=$enable
	NEWID_RC=$wrc
}
