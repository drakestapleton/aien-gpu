# SPDX-License-Identifier: MIT OR GPL-2.0-only
# aien-gpu: build the Omega units, check them, run the host tests, build the module.
#   make om          compile om/*.om with the pinned omc, run tools/kcheck on every unit
#   make host-test   unit tests and parity against the C oracles (aienux c9678d7b, fetched by pin)
#   make kmod        build kmod/aien_gpu.ko against the running kernel's headers (never loads it) and
#                    check that it holds the units of this build (tools/ko-units.sh)
#   make check       om + host-test + kmod + static checks
# OMC must be the seeded Omega compiler with the pinned sha256 below; nothing else is accepted.

OMC ?= $(error set OMC=/path/to/omega-mv/seed/out/omc (sha256 $(OMC_SHA256)))
OMC_SHA256 := 189ada82e00bb440cb41351bd252cc68c54a1b1050051cc42a1df700081c3b54
ORACLE_REPO := https://github.com/aien-dev/aienux.git
ORACLE_REV := c9678d7b5286b15cfdb92a648264de6d3e9e72ac
KDIR ?= /lib/modules/$(shell uname -r)/build
B := build
UNITS := ident boot seq seal mqput mqget abi gmmu memchk rpdes rm rmchan rmctl rmchk rmarg
CC ?= gcc
HOSTCFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror -I tests/host

.PHONY: om host-test kmod check static clean pin
all: check

$(B):
	mkdir -p $(B)

pin: | $(B)
	@got=$$(sha256sum $(OMC) | cut -d' ' -f1); \
	if [ "$$got" != "$(OMC_SHA256)" ]; then echo "OMC sha256 $$got, pinned $(OMC_SHA256): refused"; exit 1; fi; \
	echo "omc pinned $$got" > $(B)/omc.pin

$(B)/kcheck: tools/kcheck.om pin
	$(OMC) < $< > $@.tmp && chmod +x $@.tmp && mv $@.tmp $@

$(B)/%.bin: om/%.om $(B)/kcheck
	$(OMC) < $< > $@.tmp || { rc=$$?; rm -f $@.tmp; echo "omc refused $< (status $$rc)"; exit 1; }
	$(B)/kcheck < $@.tmp > $@.kcheck || { cat $@.kcheck; rm -f $@.tmp; exit 1; }
	cat $@.kcheck
	mv $@.tmp $@

om: $(UNITS:%=$(B)/%.bin)

# The trampoline object, assembled for the host from the very file the module uses.
$(B)/agpu_om.o: kmod/agpu_om.S om
	$(CC) -c -Wa,-I$(B) -o $@ $<

ORACLE := $(B)/oracle
$(ORACLE)/.rev:
	rm -rf $(ORACLE)
	git clone -q --filter=blob:none $(ORACLE_REPO) $(ORACLE)
	git -C $(ORACLE) checkout -q $(ORACLE_REV)
	git -C $(ORACLE) rev-parse HEAD > $@

OGPU := $(ORACLE)/native/kernel/gpu
ODEV := $(ORACLE)/native/kernel/dev
OCFLAGS := -I $(OGPU) -I $(ODEV) -Wno-unused-parameter -Wno-sign-compare

# om/boot.om against the GB20B reference and simulator in tests/host (written from NVIDIA's
# 580.173.02 GB20B HALs); the oracle only lends struct ck_gpu_regs (gmmu.h).
G2 := tests/host/gb20b_mmio.c tests/host/gb20b_sim.c
$(B)/parity_boot: tests/host/parity_boot.c $(G2) tests/host/gb20b_mmio.h tests/host/gb20b_sim.h $(B)/agpu_om.o $(ORACLE)/.rev
	$(CC) $(HOSTCFLAGS) $(OCFLAGS) -o $@ $< $(G2) $(B)/agpu_om.o

$(B)/parity_seq: tests/host/parity_seq.c $(B)/agpu_om.o $(ORACLE)/.rev
	$(CC) $(HOSTCFLAGS) $(OCFLAGS) -o $@ $< $(OGPU)/gsp_seq.c $(B)/agpu_om.o

$(B)/parity_mq: tests/host/parity_mq.c $(B)/agpu_om.o $(ORACLE)/.rev
	$(CC) $(HOSTCFLAGS) $(OCFLAGS) -o $@ $< $(ODEV)/gb10_gsp_boot.c $(B)/agpu_om.o

# om/gmmu.om, om/memchk.om, om/rpdes.om; the oracle gmmu.c is #included (its index math is static)
$(B)/parity_gmmu: tests/host/parity_gmmu.c $(B)/agpu_om.o $(ORACLE)/.rev
	$(CC) $(HOSTCFLAGS) $(OCFLAGS) -o $@ $< $(B)/agpu_om.o

# om/rm.om, om/rmchan.om, om/rmctl.om, om/rmchk.om, om/rmarg.om: every payload byte against the real
# NVIDIA 580.173.02 structs (MIT SDK headers, NVSRC) and the framing against the aienux oracle.
NVSRC ?= $(HOME)/workspace/_ext/open-gpu-kernel-modules-580.173.02/src
NVSDK := -I $(NVSRC)/common/sdk/nvidia/inc -I $(NVSRC)/common/sdk/nvidia/inc/ctrl -I $(NVSRC)/common/inc
$(B)/parity_rm: tests/host/parity_rm.c $(B)/agpu_om.o $(ORACLE)/.rev
	@test -f $(NVSRC)/common/sdk/nvidia/inc/alloc/alloc_channel.h || \
		{ echo "NVSRC=$(NVSRC): open-gpu-kernel-modules 580.173.02 src/ not found"; exit 1; }
	$(CC) $(HOSTCFLAGS) $(OCFLAGS) $(NVSDK) -o $@ $< $(ODEV)/gb10_gsp_boot.c $(B)/agpu_om.o

$(B)/unit_test: tests/host/unit_test.c $(B)/agpu_om.o
	$(CC) $(HOSTCFLAGS) -o $@ $< $(B)/agpu_om.o

# stage B user-space tool: the aienux L192 package builder (recorded host dependency), compiled with
# GB20B's geometry by tools/gb20b_gsp_boot.c, + om/abi.om
$(B)/aien-gpu-boot: tools/aien-gpu-boot.c tools/gb20b_gsp_boot.c tools/gb20b_gsp_boot.h kmod/aien_gpu_uapi.h $(B)/agpu_om.o $(ORACLE)/.rev
	$(CC) $(HOSTCFLAGS) -I kmod -I tools -I $(ODEV) -Wno-unused-parameter -o $@ $< tools/gb20b_gsp_boot.c $(B)/agpu_om.o

# the real COT packet (from plan_test.sh) through om/boot.om on the simulated GB20B SEC2
$(B)/cot_boot: tests/host/cot_boot.c tests/host/gb20b_sim.c tests/host/gb20b_sim.h tests/host/gb20b_mmio.h $(B)/agpu_om.o $(ORACLE)/.rev
	$(CC) $(HOSTCFLAGS) $(OCFLAGS) -o $@ $< tests/host/gb20b_sim.c $(B)/agpu_om.o

# CUDA++ m16 gate over /dev/aien-gpu (stage C); --dry-run needs no device
$(B)/aien-gpu-m16: tools/aien-gpu-m16.c kmod/aien_gpu_uapi.h | $(B)
	$(CC) $(HOSTCFLAGS) -I kmod -o $@ $<

TOOLS := aien-gpu-boot aien-gpu-m16
tools: $(TOOLS:%=$(B)/%)

HOST_TESTS := unit_test parity_boot parity_seq parity_mq parity_gmmu parity_rm
host-test: $(HOST_TESTS:%=$(B)/%) tools $(B)/cot_boot
	@set -e; for t in $(HOST_TESTS); do ./$(B)/$$t; done
	sh tests/host/kcheck_controls.sh $(B)/kcheck
	sh tests/host/mutants.sh $(OMC) $(B)
	sh tests/host/plan_test.sh $(ORACLE) $(B)

kmod: om
	$(MAKE) -C kmod KDIR=$(KDIR) OMBUILD=$(abspath $(B))
	sh tools/ko-units.sh $(B) kmod/aien_gpu.ko $(UNITS)

static: kmod
	OMBUILD=$(abspath $(B)) sh tools/static-checks.sh $(KDIR)

QEMU_CASES := absent wrongdev nobind selftest-trap
qemu-test: kmod
	@set -e; for c in $(QEMU_CASES); do sh tests/qemu/run.sh kmod/aien_gpu.ko $$c; done

check: om host-test kmod static qemu-test

clean:
	rm -rf $(B)
	$(MAKE) -C kmod clean KDIR=$(KDIR) || true
