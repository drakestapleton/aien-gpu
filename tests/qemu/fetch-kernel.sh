#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
#
# fetch-kernel.sh KREL
#
# /boot/vmlinuz-* is mode 0600 root on the Spark, and this harness never uses
# sudo. Instead, download the installed linux-image-KREL package version with
# apt-get (no root needed), extract boot/vmlinuz-KREL, and accept it only if
# its md5 equals the digest dpkg recorded for the installed /boot file in
# /var/lib/dpkg/info/linux-image-KREL.md5sums. Prints the path on stdout.
# Cache: /tmp/aien-qemu-kernel/KREL/vmlinuz-KREL.
set -eu

KREL=${1:?usage: fetch-kernel.sh KREL}
PKG=linux-image-$KREL
SUMS=/var/lib/dpkg/info/$PKG.md5sums
CACHE=/tmp/aien-qemu-kernel/$KREL
IMG=$CACHE/vmlinuz-$KREL

die() { echo "fetch-kernel: $*" >&2; exit 1; }

[ -r "$SUMS" ] || die "$SUMS missing: $PKG is not installed, nothing to match against"
want=$(awk -v f="boot/vmlinuz-$KREL" '$2 == f { print $1 }' "$SUMS")
[ -n "$want" ] || die "no boot/vmlinuz-$KREL entry in $SUMS"

if [ -r "$IMG" ] && [ "$(md5sum "$IMG" | cut -d' ' -f1)" = "$want" ]; then
	echo "$IMG"
	exit 0
fi

ver=$(dpkg-query -W -f='${Version}' "$PKG") || die "dpkg-query $PKG failed"
mkdir -p "$CACHE"
tmp=$(mktemp -d "$CACHE/dl.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
(cd "$tmp" && apt-get download "$PKG=$ver" >&2) || die "apt-get download $PKG=$ver failed"
deb=$(ls "$tmp"/*.deb)
dpkg-deb -x "$deb" "$tmp/x"
got=$(md5sum "$tmp/x/boot/vmlinuz-$KREL" | cut -d' ' -f1)
[ "$got" = "$want" ] || die "md5 $got of downloaded vmlinuz != dpkg record $want"
mv "$tmp/x/boot/vmlinuz-$KREL" "$IMG"
echo "fetch-kernel: $IMG md5 $got matches $SUMS ($PKG $ver)" >&2
echo "$IMG"
