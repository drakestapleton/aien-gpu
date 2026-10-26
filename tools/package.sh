#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# package.sh ORACLE OUT -- build the GSP boot package aien-gpu-boot reads, with the extractors and the
# package script of the pinned aienux clone (ORACLE = build/oracle, c9678d7b). Reads NVIDIA's signed
# 580.173.02 images on this machine (on loan, docs/BOUNDARY.md) and checks each against its pin;
# writes only into OUT. Touches no device. Exit 0 only on WINDOW1_PACKAGE: PASS.
set -eu
ORACLE=${1:?usage: package.sh ORACLE OUT}
OUT=${2:?usage: package.sh ORACLE OUT}
T=$(mktemp -d /tmp/agpu-pkgtools.XXXXXX)
trap 'rm -rf "$T"' EXIT
make -s -C "$ORACLE/native/kernel" OUT="$T" gsp-fw-inspect gsp-bindata-extract "$T/host/ck_gsp_sysinfo_encode" >&2
bash "$ORACLE/evidence/own-gpu-driver/rung2/window1-package.sh" "$OUT" "$T/host"
