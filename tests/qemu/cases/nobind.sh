# SPDX-License-Identifier: MIT OR GPL-2.0-only
# qemu-args: -device pci-testdev
#
# As wrongdev, but with the default allow_bind=0: the probe must refuse before
# touching the device. Observable proxy for "not touched": the function's
# enable count is 0 after the refused probe (pci_enable_device never left on).
. /cases/lib.sh

refusal_case
[ "$ENABLE" = 0 ] || fail "allow_bind=0: $BDF enable=$ENABLE after refused probe, want 0"
pass "allow_bind=0: $BDF 1b36:0005 not bound, enable=0 (new_id rc=$NEWID_RC); dmesg: $REFUSAL"
