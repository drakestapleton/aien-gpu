# SPDX-License-Identifier: MIT OR GPL-2.0-only
# qemu-args: -device pci-testdev
#
# A non-GB10 PCI function (QEMU pci-testdev, 1b36:0005) is offered to the
# driver through new_id with allow_bind=1. The probe must refuse it.
. /cases/lib.sh

refusal_case allow_bind=1
pass "allow_bind=1: $BDF 1b36:0005 not bound (new_id rc=$NEWID_RC, enable=$ENABLE); dmesg: $REFUSAL"
