#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# run.sh MODULE.ko CASE [extra qemu args...]
#
# Boots the host kernel image under qemu-system-aarch64 (-M virt, KVM with
# -cpu host; TCG -cpu max only if KVM cannot start) with a generated busybox
# initramfs, runs cases/CASE.sh in the guest against MODULE.ko and records the
# guest console. Prints one host verdict line
#   AIEN-QEMU <case> PASS|FAIL|TIMEOUT <detail> [accel=... boot_s=... ...]
# and exits 0 only on PASS. Never loads anything on the host.
#
# Environment:
#   AIEN_QEMU_KREL     kernel release (default 7.0.0-1019-nvidia)
#   AIEN_QEMU_KERNEL   kernel image (default /boot/vmlinuz-KREL if readable,
#                      else fetch-kernel.sh's dpkg-verified copy)
#   AIEN_QEMU_TIMEOUT  qemu wall-clock limit in seconds (default 180)
#   AIEN_QEMU_ACCEL    kvm (default, falls back to tcg) or tcg
#   AIEN_QEMU_KEEP     1 keeps the initramfs staging tree
set -eu

usage() {
	echo "usage: $0 MODULE.ko CASE [extra qemu args...]" >&2
	echo "cases: $(cd "$HERE/cases" && ls *.sh | grep -v '^lib.sh$' | sed 's/\.sh$//' | tr '\n' ' ')" >&2
	exit 2
}
die() { echo "run.sh: $*" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 2 ] || usage
MOD=$1
CASE=$2
shift 2
KREL=${AIEN_QEMU_KREL:-7.0.0-1019-nvidia}
TIMEOUT_S=${AIEN_QEMU_TIMEOUT:-180}
ACCEL=${AIEN_QEMU_ACCEL:-kvm}
BUSYBOX=/usr/bin/busybox
CMDLINE="console=ttyAMA0 panic=-1 oops=panic rdinit=/init"
SPLAT='Oops|BUG:|WARNING:|Call trace|Unable to handle|Kernel panic|general protection'

[ -f "$HERE/cases/$CASE.sh" ] && [ "$CASE" != lib ] || usage
[ -r "$MOD" ] || die "module $MOD not readable"
command -v qemu-system-aarch64 >/dev/null || die "qemu-system-aarch64 not found"
file -L "$BUSYBOX" | grep -q 'statically linked' || die "$BUSYBOX is not static"

# Host-side sanity on the module file (read only, nothing is loaded here).
vermagic=$(modinfo -F vermagic "$MOD")
case "$vermagic" in
"$KREL "*) ;;
*) die "vermagic '$vermagic' does not match kernel $KREL" ;;
esac
MOD_NAME=$(modinfo -F name "$MOD")
[ -n "$MOD_NAME" ] || MOD_NAME=$(basename "$MOD" .ko | tr - _)
MOD_FILE=$(basename "$MOD")

KERNEL=${AIEN_QEMU_KERNEL:-}
if [ -z "$KERNEL" ]; then
	if [ -r "/boot/vmlinuz-$KREL" ]; then
		KERNEL=/boot/vmlinuz-$KREL
	else
		KERNEL=$("$HERE/fetch-kernel.sh" "$KREL") || die "no readable kernel image for $KREL"
	fi
fi
[ -r "$KERNEL" ] || die "kernel image $KERNEL not readable"

# Case-declared qemu arguments: "# qemu-args: ..." header line.
CASE_ARGS=$(sed -n 's/^# qemu-args:[[:space:]]*//p' "$HERE/cases/$CASE.sh" | head -n 1)

WORK=$(mktemp -d /tmp/aien-qemu.XXXXXX)
ROOT=$WORK/root
INITRD=$WORK/initramfs.cpio.gz
LOG=$WORK/console.log
mkdir -p "$ROOT/bin" "$ROOT/aien" "$ROOT/cases" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/tmp"
cp "$BUSYBOX" "$ROOT/bin/busybox"
cp "$HERE/init" "$ROOT/init"
cp "$HERE/cases/lib.sh" "$HERE/cases/$CASE.sh" "$ROOT/cases/"
cp "$MOD" "$ROOT/aien/$MOD_FILE"
chmod 0755 "$ROOT/init" "$ROOT/bin/busybox"
cat > "$ROOT/aien/env" <<EOF
CASE='$CASE'
MOD_KO='/aien/$MOD_FILE'
MOD_NAME='$MOD_NAME'
EOF
(cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | gzip -9 > "$INITRD"
[ "${AIEN_QEMU_KEEP:-0}" = 1 ] || rm -rf "$ROOT"

{
	echo "run.sh: case=$CASE module=$MOD name=$MOD_NAME vermagic='$vermagic'"
	echo "run.sh: kernel=$KERNEL sha256=$(sha256sum "$KERNEL" | cut -d' ' -f1)"
	echo "run.sh: case qemu args='$CASE_ARGS' extra='$*' timeout=${TIMEOUT_S}s log=$LOG"
} >&2

# run_qemu ACCEL [extra args] -> qemu exit status in $QRC
run_qemu() {
	acc=$1
	shift
	case $acc in
	kvm) cpu="-enable-kvm -cpu host" ;;
	tcg) cpu="-accel tcg -cpu max" ;;
	*) die "unknown accel $acc" ;;
	esac
	: > "$LOG"
	QRC=0
	# shellcheck disable=SC2086 # cpu and CASE_ARGS are word lists by design
	timeout -k 10 "$TIMEOUT_S" qemu-system-aarch64 \
		-M virt,gic-version=3 $cpu -smp 2 -m 1024 \
		-nodefaults -no-reboot -display none -monitor none \
		-serial "file:$LOG" \
		-kernel "$KERNEL" -initrd "$INITRD" -append "$CMDLINE" \
		$CASE_ARGS "$@" 2> "$WORK/qemu-$acc.stderr" || QRC=$?
}

now_ms() { echo $(($(date +%s%N) / 1000000)); }

t0=$(now_ms)
used=$ACCEL
run_qemu "$ACCEL" "$@"
if [ "$ACCEL" = kvm ] && [ "$QRC" -ne 0 ] && [ "$QRC" -ne 124 ] && [ "$QRC" -ne 137 ] &&
	! grep -q 'Booting Linux' "$LOG"; then
	echo "run.sh: KVM run failed before the kernel booted (qemu rc=$QRC): $(head -n 3 "$WORK/qemu-kvm.stderr" | tr '\n' ' ')" >&2
	echo "run.sh: falling back to TCG -cpu max" >&2
	cp "$LOG" "$WORK/console-kvm-failed.log"
	used=tcg
	t0=$(now_ms)
	run_qemu tcg "$@"
fi
wall_ms=$(($(now_ms) - t0))

tr -d '\r' < "$LOG" > "$WORK/console.txt"
CON=$WORK/console.txt
boot_s=$(sed -n "s/^AIEN-QEMU-BOOT $CASE uptime_s=\([0-9.]*\).*/\1/p" "$CON" | head -n 1)
[ -n "$boot_s" ] || boot_s=none
guest=$(grep -E "^AIEN-QEMU $CASE (PASS|FAIL) " "$CON" | tail -n 1 || true)
host_splat=$(sed -n '/^AIEN-QEMU-DMESG-BEGIN$/,/^AIEN-QEMU-DMESG-END$/p' "$CON" |
	sed -n '/aien-qemu: case .* start/,$p' | grep -E "$SPLAT" | head -n 1 || true)

if [ "$QRC" -eq 124 ] || [ "$QRC" -eq 137 ]; then
	verdict=TIMEOUT
	detail="qemu killed after ${TIMEOUT_S}s; guest said: ${guest:-nothing}"
elif [ -z "$guest" ]; then
	verdict=FAIL
	last=$(grep -E 'Kernel panic|Internal error|Unable to handle|Oops' "$CON" | head -n 1 || true)
	[ -n "$last" ] || last=$(grep -v '^$' "$CON" | tail -n 1 || true)
	detail="no guest verdict (qemu rc=$QRC): ${last:-empty console}"
else
	verdict=$(echo "$guest" | cut -d' ' -f3)
	detail=$(echo "$guest" | cut -d' ' -f4-)
	if [ "$verdict" = PASS ] && [ -n "$host_splat" ]; then
		verdict=FAIL
		detail="host log scan found splat after case start: $host_splat"
	fi
fi

echo "AIEN-QEMU $CASE $verdict $detail [accel=$used boot_s=$boot_s wall_ms=$wall_ms qemu_rc=$QRC log=$LOG]"
[ "$verdict" = PASS ]
