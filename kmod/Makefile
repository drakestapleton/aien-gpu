# SPDX-License-Identifier: MIT OR GPL-2.0-only
# Out-of-tree build of aien_gpu.ko. Building never loads it. Use the top-level `make kmod`, which
# first builds and checks the Omega units; OMBUILD names the directory holding them.
KDIR ?= /lib/modules/$(shell uname -r)/build
# The compiler the running kernel was built with (CONFIG_CC_VERSION_TEXT), when present.
KCC ?= $(shell command -v aarch64-linux-gnu-gcc-13 || echo gcc)
OMBUILD ?= $(error OMBUILD unset: run `make kmod` from the repository root)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) OMBUILD=$(OMBUILD) CC=$(KCC) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
