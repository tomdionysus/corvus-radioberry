# SPDX-License-Identifier: GPL-2.0-or-later
#
# rbpcm_dma, the I2S link measurement module: make on a Raspberry Pi 4
# with the running kernel's headers. Not built by anything else.
obj-m := rbpcm_dma.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
