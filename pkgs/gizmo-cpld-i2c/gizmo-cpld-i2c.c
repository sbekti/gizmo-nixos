// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/module.h>
#include <linux/platform_data/i2c-ocores.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <asm/io.h>

#define GIZMO_CPLD_BASE 0x0280
#define GIZMO_CPLD_SIZE 0x0038
#define GIZMO_CPLD_SIGNATURE 0xFACE

#define GIZMO_CPLD_ENABLE_CONTROL 0x06
#define GIZMO_CPLD_RESET_CONTROL 0x07
#define GIZMO_CPLD_INTERRUPT_POLL 0x0A
#define GIZMO_CPLD_INTERRUPT_MODE 0x0B
#define GIZMO_CPLD_INTERRUPT_POLARITY 0x0C
#define GIZMO_CPLD_INTERRUPT_STATUS 0x0D
#define GIZMO_CPLD_INTERRUPT_ENABLE 0x0E
#define GIZMO_CPLD_INTERRUPT_ACTIVE 0x0F
#define GIZMO_CPLD_GPGPIO_BASE 0x1D
#define GIZMO_CPLD_GPGPIO_DIRECTION_SHIFT 4

#define GIZMO_CPLD_OCORES_BASE 0x02B8
#define GIZMO_CPLD_OCORES_SIZE 0x0008
#define GIZMO_CPLD_OCORES_POLLING_IRQ -1
#define GIZMO_CPLD_IRQ 7

/*
 * Hardware notes identified IRQ 8, but captures showed IRQ 8 colliding with
 * rtc0 and IRQ 7 registering without completing transfers. Default to polling
 * until the CPLD interrupt routing is proven.
 */
static int irq = GIZMO_CPLD_OCORES_POLLING_IRQ;
module_param(irq, int, 0444);
MODULE_PARM_DESC(irq, "OpenCores I2C IRQ number, or -1 to use polling");

static int intc_irq = GIZMO_CPLD_IRQ;
module_param(intc_irq, int, 0444);
MODULE_PARM_DESC(intc_irq, "CPLD parent IRQ for gizmo-intc, or -1 for GPIO-only interrupt lines");

static struct resource gizmo_ocores_resources[2] = {
	{
		.start = GIZMO_CPLD_OCORES_BASE,
		.end = GIZMO_CPLD_OCORES_BASE + GIZMO_CPLD_OCORES_SIZE - 1,
		.flags = IORESOURCE_IO,
	},
};

static struct ocores_i2c_platform_data gizmo_ocores_pdata = {
	/* The CPLD exposes standard byte-wide OpenCores registers at 0x02b8. */
	.reg_shift = 0,
	.reg_io_width = 1,
	.clock_khz = 25000,
	.bus_khz = 100,
	.big_endian = false,
};

static struct i2c_board_info const gizmo_ocores_devices[] = {
	/* These clients make the LED mux buses and IIO sensors appear. */
	{
		I2C_BOARD_INFO("bmc150_accel", 0x10),
	},
	{
		I2C_BOARD_INFO("bmc150_magn", 0x12),
	},
	{
		I2C_BOARD_INFO("pca9543", 0x72),
	},
	{
		I2C_BOARD_INFO("isl29023", 0x44),
	},
};

static struct platform_device *gizmo_ocores_device;
static DEFINE_SPINLOCK(gizmo_cpld_io_lock);

struct gizmo_cpld_bit_gpio {
	struct gpio_chip chip;
	u8 reg;
	bool registered;
};

struct gizmo_cpld_gpgpio {
	struct gpio_chip chip;
	bool registered;
};

struct gizmo_cpld_intc {
	struct gpio_chip chip;
	bool registered;
	bool irq_enabled;
	unsigned int parent_irq;
};

static const char * const gizmo_enable_gpio_names[] = {
	"hdmi_en",
	"nfc_en",
	"backlight_en",
	"bt_ant_hi_pwr_en",
	"usb_a_en",
	"usb_b_en",
	"usb_b_hi_power_en",
	"usb_a_hi_power_en",
};

static const char * const gizmo_reset_gpio_names[] = {
	"lanc_rst",
	"tpm_rst",
	"voice_rst",
	"touchpanel_rst",
	"i2c_switch_rst",
	"dsi_bridge_rst",
	"bluetooth_rst",
	NULL,
};

static const char * const gizmo_intc_gpio_names[] = {
	"touchpanel_intr",
	"nfc_intr",
	"current_sense_intr",
	"ambient_light_sensor_intr",
	"audio_codec_intr",
	"i2c_intr",
	"edp_intr",
	"accelerometer_intr",
};

static u8 gizmo_cpld_read(u8 reg)
{
	return inb(GIZMO_CPLD_BASE + reg);
}

static void gizmo_cpld_write(u8 reg, u8 value)
{
	outb(value, GIZMO_CPLD_BASE + reg);
}

static void gizmo_cpld_update_bits(u8 reg, u8 mask, u8 value)
{
	u8 reg_value;
	unsigned long flags;

	spin_lock_irqsave(&gizmo_cpld_io_lock, flags);
	reg_value = gizmo_cpld_read(reg);
	reg_value &= ~mask;
	reg_value |= value & mask;
	gizmo_cpld_write(reg, reg_value);
	spin_unlock_irqrestore(&gizmo_cpld_io_lock, flags);
}

static int gizmo_cpld_bit_get_direction(struct gpio_chip *gc,
					 unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int gizmo_cpld_bit_direction_input(struct gpio_chip *gc,
					  unsigned int offset)
{
	return 0;
}

static int gizmo_cpld_bit_direction_output(struct gpio_chip *gc,
					   unsigned int offset, int value)
{
	return gc->set(gc, offset, value);
}

static int gizmo_cpld_bit_get(struct gpio_chip *gc, unsigned int offset)
{
	struct gizmo_cpld_bit_gpio *bank = gpiochip_get_data(gc);

	return !!(gizmo_cpld_read(bank->reg) & BIT(offset));
}

static int gizmo_cpld_bit_set(struct gpio_chip *gc, unsigned int offset,
			      int value)
{
	struct gizmo_cpld_bit_gpio *bank = gpiochip_get_data(gc);

	gizmo_cpld_update_bits(bank->reg, BIT(offset),
			       value ? BIT(offset) : 0);
	return 0;
}

static int gizmo_cpld_gpgpio_get_direction(struct gpio_chip *gc,
					   unsigned int offset)
{
	u8 value = gizmo_cpld_read(GIZMO_CPLD_GPGPIO_BASE);

	if (value & BIT(offset + GIZMO_CPLD_GPGPIO_DIRECTION_SHIFT))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

static int gizmo_cpld_gpgpio_direction_input(struct gpio_chip *gc,
					     unsigned int offset)
{
	gizmo_cpld_update_bits(GIZMO_CPLD_GPGPIO_BASE,
			       BIT(offset + GIZMO_CPLD_GPGPIO_DIRECTION_SHIFT),
			       0);
	return 0;
}

static int gizmo_cpld_gpgpio_direction_output(struct gpio_chip *gc,
					      unsigned int offset, int value)
{
	u8 mask = BIT(offset) | BIT(offset + GIZMO_CPLD_GPGPIO_DIRECTION_SHIFT);
	u8 bits = BIT(offset + GIZMO_CPLD_GPGPIO_DIRECTION_SHIFT);

	if (value)
		bits |= BIT(offset);

	gizmo_cpld_update_bits(GIZMO_CPLD_GPGPIO_BASE, mask, bits);
	return 0;
}

static int gizmo_cpld_gpgpio_get(struct gpio_chip *gc, unsigned int offset)
{
	return !!(gizmo_cpld_read(GIZMO_CPLD_GPGPIO_BASE) & BIT(offset));
}

static int gizmo_cpld_gpgpio_set(struct gpio_chip *gc, unsigned int offset,
				 int value)
{
	gizmo_cpld_update_bits(GIZMO_CPLD_GPGPIO_BASE, BIT(offset),
			       value ? BIT(offset) : 0);
	return 0;
}

static int gizmo_cpld_intc_get_direction(struct gpio_chip *gc,
					 unsigned int offset)
{
	return GPIO_LINE_DIRECTION_IN;
}

static int gizmo_cpld_intc_direction_input(struct gpio_chip *gc,
					   unsigned int offset)
{
	return 0;
}

static int gizmo_cpld_intc_get(struct gpio_chip *gc, unsigned int offset)
{
	return !!(gizmo_cpld_read(GIZMO_CPLD_INTERRUPT_POLL) & BIT(offset));
}

static void gizmo_cpld_intc_irq_ack(struct irq_data *data)
{
	gizmo_cpld_write(GIZMO_CPLD_INTERRUPT_STATUS, BIT(irqd_to_hwirq(data)));
}

static void gizmo_cpld_intc_irq_mask(struct irq_data *data)
{
	gizmo_cpld_update_bits(GIZMO_CPLD_INTERRUPT_ENABLE,
			       BIT(irqd_to_hwirq(data)), 0);
	gpiochip_disable_irq(irq_data_get_irq_chip_data(data),
			     irqd_to_hwirq(data));
}

static void gizmo_cpld_intc_irq_unmask(struct irq_data *data)
{
	gpiochip_enable_irq(irq_data_get_irq_chip_data(data),
			    irqd_to_hwirq(data));
	gizmo_cpld_update_bits(GIZMO_CPLD_INTERRUPT_ENABLE,
			       BIT(irqd_to_hwirq(data)),
			       BIT(irqd_to_hwirq(data)));
}

static int gizmo_cpld_intc_irq_set_type(struct irq_data *data,
					unsigned int type)
{
	u8 bit = BIT(irqd_to_hwirq(data));
	u8 mode = 0;
	u8 polarity = 0;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		mode = bit;
		polarity = bit;
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		mode = bit;
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		polarity = bit;
		irq_set_handler_locked(data, handle_level_irq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		irq_set_handler_locked(data, handle_level_irq);
		break;
	default:
		return -EINVAL;
	}

	gizmo_cpld_update_bits(GIZMO_CPLD_INTERRUPT_MODE, bit, mode);
	gizmo_cpld_update_bits(GIZMO_CPLD_INTERRUPT_POLARITY, bit, polarity);
	irqd_set_trigger_type(data, type);

	return 0;
}

static const struct irq_chip gizmo_cpld_intc_irq_chip = {
	.name = "gizmo-intc",
	.irq_ack = gizmo_cpld_intc_irq_ack,
	.irq_mask = gizmo_cpld_intc_irq_mask,
	.irq_unmask = gizmo_cpld_intc_irq_unmask,
	.irq_set_type = gizmo_cpld_intc_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int gizmo_cpld_intc_init_hw(struct gpio_chip *gc)
{
	gizmo_cpld_write(GIZMO_CPLD_INTERRUPT_ENABLE, 0x00);
	gizmo_cpld_write(GIZMO_CPLD_INTERRUPT_STATUS, 0xff);

	return 0;
}

static void gizmo_cpld_intc_handle_one(struct gpio_chip *gc, u8 active,
				       unsigned int offset)
{
	if (active & BIT(offset))
		generic_handle_domain_irq(gc->irq.domain, offset);
}

static void gizmo_cpld_intc_parent_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct irq_chip *parent_chip = irq_desc_get_chip(desc);
	u8 active;
	unsigned int offset;

	chained_irq_enter(parent_chip, desc);

	active = gizmo_cpld_read(GIZMO_CPLD_INTERRUPT_ACTIVE);
	if (!active)
		goto out;

	gizmo_cpld_intc_handle_one(gc, active, 5);

	for (offset = 0; offset < ARRAY_SIZE(gizmo_intc_gpio_names); offset++) {
		if (offset == 0 || offset == 5)
			continue;
		gizmo_cpld_intc_handle_one(gc, active, offset);
	}

	gizmo_cpld_intc_handle_one(gc, active, 0);
	gizmo_cpld_write(GIZMO_CPLD_INTERRUPT_STATUS, active);

out:
	chained_irq_exit(parent_chip, desc);
}

static struct gizmo_cpld_bit_gpio gizmo_enable_gpios = {
	.chip = {
		.label = "gizmo-gpio-enables",
		.base = -1,
		.ngpio = ARRAY_SIZE(gizmo_enable_gpio_names),
		.names = gizmo_enable_gpio_names,
		.can_sleep = false,
		.get_direction = gizmo_cpld_bit_get_direction,
		.direction_input = gizmo_cpld_bit_direction_input,
		.direction_output = gizmo_cpld_bit_direction_output,
		.get = gizmo_cpld_bit_get,
		.set = gizmo_cpld_bit_set,
	},
	.reg = GIZMO_CPLD_ENABLE_CONTROL,
};

static struct gizmo_cpld_bit_gpio gizmo_reset_gpios = {
	.chip = {
		.label = "gizmo-gpio-resets",
		.base = -1,
		.ngpio = ARRAY_SIZE(gizmo_reset_gpio_names),
		.names = gizmo_reset_gpio_names,
		.can_sleep = false,
		.get_direction = gizmo_cpld_bit_get_direction,
		.direction_input = gizmo_cpld_bit_direction_input,
		.direction_output = gizmo_cpld_bit_direction_output,
		.get = gizmo_cpld_bit_get,
		.set = gizmo_cpld_bit_set,
	},
	.reg = GIZMO_CPLD_RESET_CONTROL,
};

static struct gizmo_cpld_gpgpio gizmo_gpgpios = {
	.chip = {
		.label = "gizmo-gpgpio",
		.base = -1,
		.ngpio = 4,
		.can_sleep = false,
		.get_direction = gizmo_cpld_gpgpio_get_direction,
		.direction_input = gizmo_cpld_gpgpio_direction_input,
		.direction_output = gizmo_cpld_gpgpio_direction_output,
		.get = gizmo_cpld_gpgpio_get,
		.set = gizmo_cpld_gpgpio_set,
	},
};

static struct gizmo_cpld_intc gizmo_intc_gpios = {
	.chip = {
		.label = "gizmo-intc",
		.base = -1,
		.ngpio = ARRAY_SIZE(gizmo_intc_gpio_names),
		.names = gizmo_intc_gpio_names,
		.can_sleep = false,
		.get_direction = gizmo_cpld_intc_get_direction,
		.direction_input = gizmo_cpld_intc_direction_input,
		.get = gizmo_cpld_intc_get,
	},
};

static u16 gizmo_cpld_signature(void)
{
	u8 reg0 = inb(GIZMO_CPLD_BASE + 0);
	u8 reg1 = inb(GIZMO_CPLD_BASE + 1);

	return ((u16)reg0 << 8) | reg1;
}

static void gizmo_cpld_unregister_gpios(void)
{
	if (gizmo_intc_gpios.registered) {
		gpiochip_remove(&gizmo_intc_gpios.chip);
		gizmo_intc_gpios.registered = false;
		gizmo_intc_gpios.irq_enabled = false;
	}

	if (gizmo_gpgpios.registered) {
		gpiochip_remove(&gizmo_gpgpios.chip);
		gizmo_gpgpios.registered = false;
	}

	if (gizmo_reset_gpios.registered) {
		gpiochip_remove(&gizmo_reset_gpios.chip);
		gizmo_reset_gpios.registered = false;
	}

	if (gizmo_enable_gpios.registered) {
		gpiochip_remove(&gizmo_enable_gpios.chip);
		gizmo_enable_gpios.registered = false;
	}
}

static void gizmo_cpld_configure_intc_irq(void)
{
	struct gpio_irq_chip *girq = &gizmo_intc_gpios.chip.irq;

	memset(girq, 0, sizeof(*girq));
	gizmo_intc_gpios.irq_enabled = false;

	if (intc_irq < 0)
		return;

	gizmo_intc_gpios.parent_irq = intc_irq;
	gpio_irq_chip_set_chip(girq, &gizmo_cpld_intc_irq_chip);
	girq->handler = handle_bad_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->parent_handler = gizmo_cpld_intc_parent_handler;
	girq->parent_handler_data = &gizmo_intc_gpios.chip;
	girq->num_parents = 1;
	girq->parents = &gizmo_intc_gpios.parent_irq;
	girq->init_hw = gizmo_cpld_intc_init_hw;
	gizmo_intc_gpios.irq_enabled = true;
}

static int gizmo_cpld_register_intc(void)
{
	int ret;

	gizmo_cpld_configure_intc_irq();
	ret = gpiochip_add_data(&gizmo_intc_gpios.chip, &gizmo_intc_gpios);
	if (ret && gizmo_intc_gpios.irq_enabled) {
		pr_warn("gizmo-cpld-i2c: failed to register gizmo-intc with IRQ %d (%d); retrying GPIO-only\n",
			intc_irq, ret);
		intc_irq = -1;
		gizmo_cpld_configure_intc_irq();
		ret = gpiochip_add_data(&gizmo_intc_gpios.chip, &gizmo_intc_gpios);
	}
	if (ret)
		return ret;

	gizmo_intc_gpios.registered = true;
	return 0;
}

static int gizmo_cpld_register_gpios(void)
{
	int ret;

	ret = gpiochip_add_data(&gizmo_enable_gpios.chip, &gizmo_enable_gpios);
	if (ret)
		return ret;
	gizmo_enable_gpios.registered = true;

	ret = gpiochip_add_data(&gizmo_reset_gpios.chip, &gizmo_reset_gpios);
	if (ret)
		goto unregister_gpios;
	gizmo_reset_gpios.registered = true;

	ret = gpiochip_add_data(&gizmo_gpgpios.chip, &gizmo_gpgpios);
	if (ret)
		goto unregister_gpios;
	gizmo_gpgpios.registered = true;

	ret = gizmo_cpld_register_intc();
	if (ret)
		goto unregister_gpios;

	return 0;

unregister_gpios:
	gizmo_cpld_unregister_gpios();
	return ret;
}

static int __init gizmo_cpld_i2c_init(void)
{
	u16 signature;
	int ret;

	if (!request_region(GIZMO_CPLD_BASE, GIZMO_CPLD_SIZE, "gizmo-cpld")) {
		pr_err("gizmo-cpld-i2c: CPLD I/O region 0x%x-0x%x is busy\n",
		       GIZMO_CPLD_BASE, GIZMO_CPLD_BASE + GIZMO_CPLD_SIZE - 1);
		return -EBUSY;
	}

	signature = gizmo_cpld_signature();
	if (signature != GIZMO_CPLD_SIGNATURE) {
		pr_err("gizmo-cpld-i2c: bad CPLD signature 0x%04x, expected 0x%04x\n",
		       signature, GIZMO_CPLD_SIGNATURE);
		ret = -ENODEV;
		goto release_cpld_region;
	}

	ret = gizmo_cpld_register_gpios();
	if (ret) {
		pr_err("gizmo-cpld-i2c: failed to register CPLD GPIO chips: %d\n",
		       ret);
		goto release_cpld_region;
	}

	gizmo_ocores_pdata.num_devices = ARRAY_SIZE(gizmo_ocores_devices);
	gizmo_ocores_pdata.devices = gizmo_ocores_devices;

	if (irq >= 0) {
		gizmo_ocores_resources[1].start = irq;
		gizmo_ocores_resources[1].end = irq;
		gizmo_ocores_resources[1].flags = IORESOURCE_IRQ;
	}

	gizmo_ocores_device = platform_device_alloc("ocores-i2c", 0);
	if (!gizmo_ocores_device) {
		ret = -ENOMEM;
		goto unregister_gpios;
	}

	ret = platform_device_add_resources(gizmo_ocores_device,
					    gizmo_ocores_resources,
					    irq >= 0 ? ARRAY_SIZE(gizmo_ocores_resources) : 1);
	if (ret)
		goto put_device;

	ret = platform_device_add_data(gizmo_ocores_device, &gizmo_ocores_pdata,
				       sizeof(gizmo_ocores_pdata));
	if (ret)
		goto put_device;

	ret = platform_device_add(gizmo_ocores_device);
	if (ret)
		goto put_device;

	if (irq >= 0)
		pr_info("gizmo-cpld-i2c: registered CPLD GPIOs and ocores-i2c.0 at I/O 0x%x, irq %d, signature 0x%04x\n",
			GIZMO_CPLD_OCORES_BASE, irq, signature);
	else
		pr_info("gizmo-cpld-i2c: registered CPLD GPIOs and ocores-i2c.0 at I/O 0x%x in polling mode, signature 0x%04x\n",
			GIZMO_CPLD_OCORES_BASE, signature);
	return 0;

put_device:
	platform_device_put(gizmo_ocores_device);
	gizmo_ocores_device = NULL;
unregister_gpios:
	gizmo_cpld_unregister_gpios();
release_cpld_region:
	release_region(GIZMO_CPLD_BASE, GIZMO_CPLD_SIZE);
	return ret;
}

static void __exit gizmo_cpld_i2c_exit(void)
{
	platform_device_unregister(gizmo_ocores_device);
	gizmo_cpld_unregister_gpios();
	release_region(GIZMO_CPLD_BASE, GIZMO_CPLD_SIZE);
}

module_init(gizmo_cpld_i2c_init);
module_exit(gizmo_cpld_i2c_exit);

MODULE_AUTHOR("GizmoToolchain");
MODULE_DESCRIPTION("Gizmo CPLD GPIO and OpenCores I2C platform devices");
MODULE_LICENSE("GPL");
