// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define FT7511_ADDR			0x38
#define FT7511_REG_DATA			0x00
#define FT7511_REG_TOUCH_COUNT		0x02
#define FT7511_REG_TOUCH_DATA		0x03
#define FT7511_REG_DISABLE_MONITOR_1	0x86
#define FT7511_REG_PERIOD_ACTIVE	0x88
#define FT7511_REG_DISABLE_MONITOR_2	0xa5

#define FT7511_POINT_SIZE		6
#define FT7511_MAX_SUPPORTED_TOUCHES	10

static int bus = -1;
module_param(bus, int, 0444);
MODULE_PARM_DESC(bus, "I2C bus number used to instantiate the FT7511 client; disabled when negative");

static int address = FT7511_ADDR;
module_param(address, int, 0444);
MODULE_PARM_DESC(address, "FT7511 I2C address");

static int irq = -1;
module_param(irq, int, 0444);
MODULE_PARM_DESC(irq, "IRQ number for the FT7511 interrupt line");

static int irq_gpio = -1;
module_param(irq_gpio, int, 0444);
MODULE_PARM_DESC(irq_gpio, "GPIO number for the FT7511 interrupt line");

static int reset_gpio = -1;
module_param(reset_gpio, int, 0444);
MODULE_PARM_DESC(reset_gpio, "GPIO number for the FT7511 reset line");

static int max_touches = FT7511_MAX_SUPPORTED_TOUCHES;
module_param(max_touches, int, 0444);
MODULE_PARM_DESC(max_touches, "Maximum concurrent touches");

static int max_x = 1200;
module_param(max_x, int, 0444);
MODULE_PARM_DESC(max_x, "Maximum X coordinate");

static int max_y = 1920;
module_param(max_y, int, 0444);
MODULE_PARM_DESC(max_y, "Maximum Y coordinate");

static int report_period_active = 100;
module_param(report_period_active, int, 0444);
MODULE_PARM_DESC(report_period_active, "Active report period in Hz; zero leaves the controller default");

static bool disable_monitor_mode = true;
module_param(disable_monitor_mode, bool, 0444);
MODULE_PARM_DESC(disable_monitor_mode, "Write monitor-mode disable registers during probe");

static bool invert_x;
module_param(invert_x, bool, 0444);
MODULE_PARM_DESC(invert_x, "Invert reported X coordinate");

static bool invert_y;
module_param(invert_y, bool, 0444);
MODULE_PARM_DESC(invert_y, "Invert reported Y coordinate");

static bool swap_xy;
module_param(swap_xy, bool, 0444);
MODULE_PARM_DESC(swap_xy, "Swap reported X/Y coordinates");

static int poll_ms;
module_param(poll_ms, int, 0444);
MODULE_PARM_DESC(poll_ms, "Poll interval in milliseconds when no IRQ is configured; zero disables polling fallback");

static char ft7511_i2c_dev_id[16];
static bool ft7511_gpio_lookup_registered;
static struct gpiod_lookup_table ft7511_gpio_lookup = {
	.table = {
		GPIO_LOOKUP("gizmo-gpio-resets", 3, "reset", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gizmo-intc", 0, "irq", GPIO_ACTIVE_HIGH),
		{},
	},
};

struct ft7511_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work poll_work;
	struct gpio_desc *irq_desc;
	struct gpio_desc *reset_desc;
	int irq;
	bool irq_gpio_requested;
	bool reset_gpio_requested;
	bool polling;
};

static struct i2c_client *created_client;

static void ft7511_reset(struct ft7511_data *data)
{
	if (data->reset_desc) {
		gpiod_set_value_cansleep(data->reset_desc, 1);
		usleep_range(5000, 20000);
		gpiod_set_value_cansleep(data->reset_desc, 0);
		msleep(300);
		return;
	}

	if (reset_gpio < 0)
		return;

	gpio_set_value_cansleep(reset_gpio, 1);
	usleep_range(5000, 20000);
	gpio_set_value_cansleep(reset_gpio, 0);
	msleep(300);
}

static int ft7511_write_config(struct i2c_client *client)
{
	int ret;

	if (disable_monitor_mode) {
		ret = i2c_smbus_write_byte_data(client, FT7511_REG_DISABLE_MONITOR_1, 0x00);
		if (ret)
			return ret;
		msleep(500);

		ret = i2c_smbus_write_byte_data(client, FT7511_REG_DISABLE_MONITOR_2, 0x00);
		if (ret)
			return ret;
		msleep(500);
	}

	if (report_period_active > 0) {
		ret = i2c_smbus_write_byte_data(client, FT7511_REG_PERIOD_ACTIVE,
						report_period_active / 10);
		if (ret)
			return ret;
	}

	return 0;
}

static void ft7511_report_contacts(struct ft7511_data *data)
{
	struct input_dev *input = data->input;
	u8 buf[FT7511_MAX_SUPPORTED_TOUCHES * FT7511_POINT_SIZE];
	bool seen[FT7511_MAX_SUPPORTED_TOUCHES] = { false };
	int count;
	int ret;
	int i;

	count = i2c_smbus_read_byte_data(data->client, FT7511_REG_TOUCH_COUNT);
	if (count < 0)
		return;

	count &= 0x0f;
	count = min(count, max_touches);
	if (count > 0) {
		ret = i2c_smbus_read_i2c_block_data(data->client, FT7511_REG_TOUCH_DATA,
						    count * FT7511_POINT_SIZE, buf);
		if (ret != count * FT7511_POINT_SIZE)
			return;
	}

	for (i = 0; i < count; i++) {
		u8 *point = &buf[i * FT7511_POINT_SIZE];
		u8 event = point[0] >> 6;
		u8 id = point[2] >> 4;
		bool active = event == 0 || event == 2;
		int x = ((point[0] & 0x0f) << 8) | point[1];
		int y = ((point[2] & 0x0f) << 8) | point[3];
		/*
		 * If the display is rotated with swap_xy, the usable coordinate
		 * ranges rotate too. Use post-transform limits for inversion and
		 * clamping so X receives coordinates in the rotated screen space.
		 */
		int x_limit = swap_xy ? max_y : max_x;
		int y_limit = swap_xy ? max_x : max_y;
		int pressure = point[4] & 0x7f;

		if (id >= max_touches)
			continue;

		if (swap_xy)
			swap(x, y);
		if (invert_x)
			x = x_limit - x;
		if (invert_y)
			y = y_limit - y;

		seen[id] = true;
		input_mt_slot(input, id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, active);
		if (active) {
			input_report_abs(input, ABS_MT_POSITION_X, clamp(x, 0, x_limit));
			input_report_abs(input, ABS_MT_POSITION_Y, clamp(y, 0, y_limit));
			input_report_abs(input, ABS_MT_PRESSURE, pressure);
		}
	}

	for (i = 0; i < max_touches; i++) {
		if (seen[i])
			continue;
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}

	input_mt_sync_frame(input);
	input_sync(input);
}

static irqreturn_t ft7511_irq_thread(int irq_num, void *dev_id)
{
	ft7511_report_contacts(dev_id);
	return IRQ_HANDLED;
}

static void ft7511_poll_work(struct work_struct *work)
{
	struct ft7511_data *data =
		container_of(to_delayed_work(work), struct ft7511_data, poll_work);

	ft7511_report_contacts(data);
	schedule_delayed_work(&data->poll_work, msecs_to_jiffies(poll_ms));
}

static void ft7511_cancel_poll(void *dev_id)
{
	struct ft7511_data *data = dev_id;

	cancel_delayed_work_sync(&data->poll_work);
}

static void ft7511_free_gpios(void *dev_id)
{
	struct ft7511_data *data = dev_id;

	if (data->irq_gpio_requested)
		gpio_free(irq_gpio);
	if (data->reset_gpio_requested)
		gpio_free(reset_gpio);
}

static int ft7511_request_gpios(struct device *dev, struct ft7511_data *data)
{
	int ret;

	if (reset_gpio < 0) {
		data->reset_desc = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
		if (IS_ERR(data->reset_desc))
			return dev_err_probe(dev, PTR_ERR(data->reset_desc),
					     "failed to request reset descriptor\n");
	} else {
		ret = gpio_request_one(reset_gpio, GPIOF_OUT_INIT_LOW, "ft7511-reset");
		if (ret)
			return dev_err_probe(dev, ret, "failed to request reset GPIO %d\n",
					     reset_gpio);
		data->reset_gpio_requested = true;
	}

	if (irq_gpio >= 0) {
		ret = gpio_request_one(irq_gpio, GPIOF_IN, "ft7511-irq");
		if (ret)
			return dev_err_probe(dev, ret, "failed to request IRQ GPIO %d\n",
					     irq_gpio);
		data->irq_gpio_requested = true;
		data->irq = gpio_to_irq(irq_gpio);
		if (data->irq < 0)
			return dev_err_probe(dev, data->irq, "failed to map IRQ GPIO %d\n",
					     irq_gpio);
	} else if (irq > 0) {
		data->irq = irq;
	} else if (data->client->irq > 0) {
		data->irq = data->client->irq;
	} else {
		data->irq_desc = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
		if (IS_ERR(data->irq_desc))
			return dev_err_probe(dev, PTR_ERR(data->irq_desc),
					     "failed to request IRQ descriptor\n");

		if (data->irq_desc) {
			data->irq = gpiod_to_irq(data->irq_desc);
			if (data->irq < 0) {
				if (poll_ms <= 0)
					return dev_err_probe(dev, data->irq,
							     "failed to map IRQ descriptor\n");
				dev_warn(dev, "failed to map IRQ descriptor (%d); polling every %d ms\n",
					 data->irq, poll_ms);
				data->irq = 0;
			}
		}
	}

	if (data->irq <= 0) {
		if (poll_ms <= 0)
			return dev_err_probe(dev, -EINVAL,
					     "missing FT7511 IRQ; set irq_gpio, irq, or poll_ms\n");
		/* Keep polling as a fallback if the CPLD IRQ line is unavailable. */
		data->irq = 0;
	}

	ret = devm_add_action_or_reset(dev, ft7511_free_gpios, data);
	if (ret)
		return ret;

	return 0;
}

static int ft7511_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ft7511_data *data;
	struct input_dev *input;
	int ret;
	int id;
	int axis_x = max_x;
	int axis_y = max_y;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP, "adapter does not support I2C transfers\n");

	if (max_touches < 1 || max_touches > FT7511_MAX_SUPPORTED_TOUCHES)
		return dev_err_probe(dev, -EINVAL, "max_touches must be 1..%d\n",
				     FT7511_MAX_SUPPORTED_TOUCHES);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);

	ret = ft7511_request_gpios(dev, data);
	if (ret)
		return ret;

	ft7511_reset(data);

	id = i2c_smbus_read_byte_data(client, FT7511_REG_DATA);
	if (id < 0)
		return dev_err_probe(dev, id, "failed to read FT7511 register 0x00\n");

	ret = ft7511_write_config(client);
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure FT7511\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	data->input = input;
	input->name = "Gizmo FT7511 Touchscreen";
	input->id.bustype = BUS_I2C;
	input->dev.parent = dev;

	if (swap_xy)
		swap(axis_x, axis_y);

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, axis_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, axis_y, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 127, 0, 0);

	ret = input_mt_init_slots(input, max_touches, INPUT_MT_DIRECT);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize multitouch slots\n");

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input device\n");

	if (data->irq > 0) {
		ret = devm_request_threaded_irq(dev, data->irq, NULL, ft7511_irq_thread,
						IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
						"ft7511", data);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request IRQ %d\n",
					     data->irq);
	} else {
		INIT_DELAYED_WORK(&data->poll_work, ft7511_poll_work);
		ret = devm_add_action_or_reset(dev, ft7511_cancel_poll, data);
		if (ret)
			return ret;
		data->polling = true;
		schedule_delayed_work(&data->poll_work, msecs_to_jiffies(poll_ms));
	}

	dev_info(dev, "FT7511 touchscreen registered addr=0x%02x irq=%d poll_ms=%d id0=0x%02x max=%dx%d touches=%d\n",
		 client->addr, data->irq, data->polling ? poll_ms : 0, id, max_x,
		 max_y, max_touches);
	return 0;
}

static const struct i2c_device_id ft7511_id[] = {
	{ "ft7511", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ft7511_id);

static struct i2c_driver ft7511_driver = {
	.driver = {
		.name = "ft7511",
	},
	.probe = ft7511_probe,
	.id_table = ft7511_id,
};

static void ft7511_register_gpio_lookup(void)
{
	if (bus < 0 || ft7511_gpio_lookup_registered)
		return;

	snprintf(ft7511_i2c_dev_id, sizeof(ft7511_i2c_dev_id), "%d-%04x",
		 bus, address);
	ft7511_gpio_lookup.dev_id = ft7511_i2c_dev_id;
	gpiod_add_lookup_table(&ft7511_gpio_lookup);
	ft7511_gpio_lookup_registered = true;
}

static void ft7511_unregister_gpio_lookup(void)
{
	if (!ft7511_gpio_lookup_registered)
		return;

	gpiod_remove_lookup_table(&ft7511_gpio_lookup);
	ft7511_gpio_lookup_registered = false;
}

static int __init ft7511_init(void)
{
	struct i2c_adapter *adapter;
	struct i2c_board_info info = {
		I2C_BOARD_INFO("ft7511", FT7511_ADDR),
	};
	int ret;

	ft7511_register_gpio_lookup();

	ret = i2c_add_driver(&ft7511_driver);
	if (ret) {
		ft7511_unregister_gpio_lookup();
		return ret;
	}

	if (bus < 0)
		return 0;

	/*
	 * The controller is not described by firmware tables on the recovered
	 * images, so the NixOS bring-up service supplies the OpenCores I2C bus
	 * and this module creates the i2c_client explicitly.
	 */
	if (address < 0x03 || address > 0x77) {
		ret = -EINVAL;
		goto del_driver;
	}

	info.addr = address;
	if (irq > 0)
		info.irq = irq;

	adapter = i2c_get_adapter(bus);
	if (!adapter) {
		ret = -ENODEV;
		goto del_driver;
	}

	created_client = i2c_new_client_device(adapter, &info);
	i2c_put_adapter(adapter);
	if (IS_ERR(created_client)) {
		ret = PTR_ERR(created_client);
		created_client = NULL;
		goto del_driver;
	}

	pr_info("gizmo-ft7511: created FT7511 client on i2c-%d addr 0x%02x\n",
		bus, address);
	return 0;

del_driver:
	i2c_del_driver(&ft7511_driver);
	ft7511_unregister_gpio_lookup();
	return ret;
}

static void __exit ft7511_exit(void)
{
	if (created_client)
		i2c_unregister_device(created_client);
	i2c_del_driver(&ft7511_driver);
	ft7511_unregister_gpio_lookup();
}

module_init(ft7511_init);
module_exit(ft7511_exit);

MODULE_AUTHOR("GizmoToolchain");
MODULE_DESCRIPTION("Gizmo FT7511 touchscreen driver");
MODULE_LICENSE("GPL");
