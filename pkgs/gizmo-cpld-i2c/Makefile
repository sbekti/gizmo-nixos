# SPDX-License-Identifier: GPL-2.0-only
obj-m += gizmo-cpld-i2c.o

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
