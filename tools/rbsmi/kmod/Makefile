# SPDX-License-Identifier: GPL-2.0-or-later
#
# rbsmi_dma, the measurement module: make -C tools/rbsmi/kmod on a Pi 4
# with the running kernel's headers. Not built by CMake or the packages.
obj-m := rbsmi_dma.o
ccflags-y := -I$(src)/../../../driver/kernel

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
