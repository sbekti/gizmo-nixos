# SPDX-License-Identifier: GPL-2.0-only
obj-m += gizmo-pn544.o

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
