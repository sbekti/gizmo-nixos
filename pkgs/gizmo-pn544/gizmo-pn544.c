// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define PN544_I2C_ADDR		0x28
#define PN544_MAX_TRANSFER	512

#define PN544_MAGIC		0xe9
#define PN544_SET_PWR		_IOW(PN544_MAGIC, 0x01, unsigned int)
#define PN54X_CLK_REQ		_IOW(PN544_MAGIC, 0x02, unsigned int)

#define PN544_PWR_OFF		0
#define PN544_PWR_ON		1
#define PN544_PWR_FW		2

static int bus = -1;
module_param(bus, int, 0444);
MODULE_PARM_DESC(bus, "I2C bus number used to instantiate the PN547/PN544 client; disabled when negative");

static int address = PN544_I2C_ADDR;
module_param(address, int, 0444);
MODULE_PARM_DESC(address, "PN547/PN544 I2C address");

static int irq_gpio = -1;
module_param(irq_gpio, int, 0444);
MODULE_PARM_DESC(irq_gpio, "GPIO number for the NFC interrupt line");

static int ven_gpio = -1;
module_param(ven_gpio, int, 0444);
MODULE_PARM_DESC(ven_gpio, "GPIO number for the NFC VEN/enable line");

static int firm_gpio = -1;
module_param(firm_gpio, int, 0444);
MODULE_PARM_DESC(firm_gpio, "GPIO number for the NFC firmware-download line; negative when unused");

static char pn544_i2c_dev_id[16];
static bool pn544_gpio_lookup_registered;
static struct gpiod_lookup_table pn544_gpio_lookup = {
	.table = {
		GPIO_LOOKUP("gizmo-gpio-enables", 1, "ven", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gizmo-intc", 1, "irq", GPIO_ACTIVE_HIGH),
		{},
	},
};

struct pn544_regulator {
	const char *name;
	struct regulator *regulator;
	bool enabled;
};

struct pn544_dev {
	struct i2c_client *client;
	struct miscdevice miscdev;
	wait_queue_head_t read_wq;
	struct mutex read_mutex;
	struct mutex write_mutex;
	struct mutex power_mutex;
	spinlock_t irq_enabled_lock;
	struct pn544_regulator regulators[4];
	struct gpio_desc *irq_desc;
	struct gpio_desc *ven_desc;
	int irq;
	bool irq_enabled;
	bool irq_gpio_requested;
	bool ven_gpio_requested;
	bool firm_gpio_requested;
};

static struct i2c_client *created_client;

static bool pn544_gpio_valid(int gpio)
{
	return gpio >= 0 && gpio_is_valid(gpio);
}

static bool pn544_has_ven(struct pn544_dev *pn544)
{
	return pn544->ven_desc || pn544_gpio_valid(ven_gpio);
}

static int pn544_set_ven(struct pn544_dev *pn544, int value)
{
	if (pn544->ven_desc) {
		gpiod_set_value_cansleep(pn544->ven_desc, value);
		return 0;
	}

	if (pn544_gpio_valid(ven_gpio)) {
		gpio_set_value_cansleep(ven_gpio, value);
		return 0;
	}

	return -ENODEV;
}

static bool pn544_has_firm(void)
{
	return pn544_gpio_valid(firm_gpio);
}

static void pn544_set_firm(int value)
{
	if (pn544_has_firm())
		gpio_set_value_cansleep(firm_gpio, value);
}

static bool pn544_irq_line_enabled(struct pn544_dev *pn544)
{
	return pn544->irq > 0 && (pn544->irq_desc || pn544_gpio_valid(irq_gpio));
}

static int pn544_irq_level(struct pn544_dev *pn544)
{
	if (pn544->irq_desc)
		return gpiod_get_value_cansleep(pn544->irq_desc);

	if (pn544_gpio_valid(irq_gpio))
		return gpio_get_value_cansleep(irq_gpio);

	return 1;
}

static void pn544_disable_irq(struct pn544_dev *pn544)
{
	unsigned long flags;

	if (!pn544_irq_line_enabled(pn544))
		return;

	spin_lock_irqsave(&pn544->irq_enabled_lock, flags);
	if (pn544->irq_enabled) {
		disable_irq_nosync(pn544->irq);
		pn544->irq_enabled = false;
	}
	spin_unlock_irqrestore(&pn544->irq_enabled_lock, flags);
}

static void pn544_enable_irq(struct pn544_dev *pn544)
{
	unsigned long flags;

	if (!pn544_irq_line_enabled(pn544))
		return;

	spin_lock_irqsave(&pn544->irq_enabled_lock, flags);
	if (!pn544->irq_enabled) {
		pn544->irq_enabled = true;
		enable_irq(pn544->irq);
	}
	spin_unlock_irqrestore(&pn544->irq_enabled_lock, flags);
}

static irqreturn_t pn544_irq_handler(int irq_num, void *dev_id)
{
	struct pn544_dev *pn544 = dev_id;

	pn544_disable_irq(pn544);
	wake_up(&pn544->read_wq);

	return IRQ_HANDLED;
}

static int pn544_enable_regulators(struct pn544_dev *pn544)
{
	int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pn544->regulators); i++) {
		if (!pn544->regulators[i].regulator)
			continue;

		ret = regulator_enable(pn544->regulators[i].regulator);
		if (ret)
			goto disable_previous;
		pn544->regulators[i].enabled = true;
	}

	return 0;

disable_previous:
	while (--i >= 0) {
		if (pn544->regulators[i].regulator && pn544->regulators[i].enabled) {
			regulator_disable(pn544->regulators[i].regulator);
			pn544->regulators[i].enabled = false;
		}
	}

	return ret;
}

static void pn544_disable_regulators(struct pn544_dev *pn544)
{
	int i;

	for (i = ARRAY_SIZE(pn544->regulators) - 1; i >= 0; i--) {
		if (pn544->regulators[i].regulator && pn544->regulators[i].enabled) {
			regulator_disable(pn544->regulators[i].regulator);
			pn544->regulators[i].enabled = false;
		}
	}
}

static int pn544_power_on(struct pn544_dev *pn544)
{
	int ret;

	if (!pn544_has_ven(pn544))
		return -ENODEV;

	ret = pn544_enable_regulators(pn544);
	if (ret)
		return ret;

	pn544_set_firm(0);

	ret = pn544_set_ven(pn544, 1);
	if (ret) {
		pn544_disable_regulators(pn544);
		return ret;
	}
	msleep(100);

	return 0;
}

static int pn544_fw_power_on(struct pn544_dev *pn544)
{
	int ret;

	if (!pn544_has_ven(pn544) || !pn544_has_firm())
		return -ENODEV;

	ret = pn544_set_ven(pn544, 1);
	if (ret)
		return ret;
	msleep(20);
	pn544_set_firm(1);
	msleep(20);
	ret = pn544_set_ven(pn544, 0);
	if (ret)
		return ret;
	msleep(100);
	ret = pn544_set_ven(pn544, 1);
	if (ret)
		return ret;
	msleep(20);

	return 0;
}

static void pn544_power_off(struct pn544_dev *pn544)
{
	pn544_set_firm(0);
	pn544_set_ven(pn544, 0);
	msleep(10);
	pn544_disable_regulators(pn544);
}

static ssize_t pn544_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *offset)
{
	struct pn544_dev *pn544 = filp->private_data;
	u8 *tmp;
	int ret;

	if (count > PN544_MAX_TRANSFER)
		count = PN544_MAX_TRANSFER;
	if (!count)
		return 0;

	tmp = kzalloc(count, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	mutex_lock(&pn544->read_mutex);

	if (pn544_irq_line_enabled(pn544)) {
		ret = pn544_irq_level(pn544);
		if (ret < 0)
			goto out_unlock;

		if (ret == 0) {
			if (filp->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				goto out_unlock;
			}

			pn544_enable_irq(pn544);
			ret = wait_event_interruptible(pn544->read_wq,
						       pn544_irq_level(pn544) > 0);
			pn544_disable_irq(pn544);
			if (ret)
				goto out_unlock;

			ret = pn544_irq_level(pn544);
			if (ret < 0)
				goto out_unlock;
			if (ret == 0) {
				ret = -EIO;
				goto out_unlock;
			}
		}
	}

	ret = i2c_master_recv(pn544->client, tmp, count);
	if (ret < 0)
		goto out_unlock;

	usleep_range(1000, 1500);

	if (copy_to_user(buf, tmp, ret)) {
		ret = -EFAULT;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&pn544->read_mutex);
	kfree(tmp);
	return ret;
}

static ssize_t pn544_write(struct file *filp, const char __user *buf, size_t count,
			   loff_t *offset)
{
	struct pn544_dev *pn544 = filp->private_data;
	u8 *tmp;
	int ret;

	if (count > PN544_MAX_TRANSFER)
		count = PN544_MAX_TRANSFER;
	if (!count)
		return 0;

	tmp = memdup_user(buf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	mutex_lock(&pn544->write_mutex);
	ret = i2c_master_send(pn544->client, tmp, count);
	usleep_range(1000, 1500);
	mutex_unlock(&pn544->write_mutex);

	kfree(tmp);

	if (ret < 0)
		return ret;
	if (ret != count)
		return -EIO;

	return ret;
}

static long pn544_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct pn544_dev *pn544 = filp->private_data;
	unsigned int value;
	int ret = 0;

	switch (cmd) {
	case PN544_SET_PWR:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;

		mutex_lock(&pn544->power_mutex);
		switch (value) {
		case PN544_PWR_OFF:
			pn544_power_off(pn544);
			break;
		case PN544_PWR_ON:
			ret = pn544_power_on(pn544);
			break;
		case PN544_PWR_FW:
			ret = pn544_fw_power_on(pn544);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		mutex_unlock(&pn544->power_mutex);
		return ret;

	case PN54X_CLK_REQ:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static __poll_t pn544_poll(struct file *filp, poll_table *wait)
{
	struct pn544_dev *pn544 = filp->private_data;
	int level;

	poll_wait(filp, &pn544->read_wq, wait);

	if (!pn544_irq_line_enabled(pn544))
		return EPOLLIN | EPOLLRDNORM;

	level = pn544_irq_level(pn544);
	if (level > 0)
		return EPOLLIN | EPOLLRDNORM;

	/*
	 * Userspace NFC stacks commonly poll() before read(). Arm the IRQ here
	 * too; otherwise a delayed controller response can sit on nfc_intr
	 * without waking the waiter until a blocking read is attempted.
	 */
	pn544_enable_irq(pn544);

	return 0;
}

static int pn544_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct pn544_dev *pn544 = container_of(miscdev, struct pn544_dev, miscdev);

	filp->private_data = pn544;
	return 0;
}

static const struct file_operations pn544_fops = {
	.owner = THIS_MODULE,
	.open = pn544_open,
	.read = pn544_read,
	.write = pn544_write,
	.unlocked_ioctl = pn544_ioctl,
	.poll = pn544_poll,
	.llseek = noop_llseek,
};

static void pn544_free_gpios(void *dev_id)
{
	struct pn544_dev *pn544 = dev_id;

	if (pn544->irq_gpio_requested)
		gpio_free(irq_gpio);
	if (pn544->firm_gpio_requested)
		gpio_free(firm_gpio);
	if (pn544->ven_gpio_requested)
		gpio_free(ven_gpio);
}

static int pn544_request_gpios(struct device *dev, struct pn544_dev *pn544)
{
	int ret;

	if (!pn544_gpio_valid(ven_gpio)) {
		pn544->ven_desc = devm_gpiod_get_optional(dev, "ven", GPIOD_OUT_LOW);
		if (IS_ERR(pn544->ven_desc))
			return dev_err_probe(dev, PTR_ERR(pn544->ven_desc),
					     "failed to request VEN descriptor\n");
	}

	if (pn544_gpio_valid(ven_gpio)) {
		ret = gpio_request_one(ven_gpio, GPIOF_OUT_INIT_LOW, "pn544-ven");
		if (ret)
			return dev_err_probe(dev, ret, "failed to request VEN GPIO %d\n",
					     ven_gpio);
		pn544->ven_gpio_requested = true;
	}

	if (pn544_gpio_valid(firm_gpio)) {
		ret = gpio_request_one(firm_gpio, GPIOF_OUT_INIT_LOW, "pn544-firm");
		if (ret)
			return dev_err_probe(dev, ret, "failed to request FIRM GPIO %d\n",
					     firm_gpio);
		pn544->firm_gpio_requested = true;
	}

	if (pn544_gpio_valid(irq_gpio)) {
		ret = gpio_request_one(irq_gpio, GPIOF_IN, "pn544-irq");
		if (ret)
			return dev_err_probe(dev, ret, "failed to request IRQ GPIO %d\n",
					     irq_gpio);
		pn544->irq_gpio_requested = true;

		pn544->irq = gpio_to_irq(irq_gpio);
		if (pn544->irq < 0)
			return dev_err_probe(dev, pn544->irq,
					     "failed to map IRQ GPIO %d\n", irq_gpio);
	} else {
		pn544->irq_desc = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
		if (IS_ERR(pn544->irq_desc))
			return dev_err_probe(dev, PTR_ERR(pn544->irq_desc),
					     "failed to request IRQ descriptor\n");

		if (pn544->irq_desc) {
			pn544->irq = gpiod_to_irq(pn544->irq_desc);
			if (pn544->irq < 0) {
				dev_warn(dev, "failed to map IRQ descriptor (%d); NFC reads will not wait on nfc_intr\n",
					 pn544->irq);
				pn544->irq = 0;
			}
		}
	}

	ret = devm_add_action_or_reset(dev, pn544_free_gpios, pn544);
	if (ret)
		return ret;

	return 0;
}

static void pn544_get_regulators(struct device *dev, struct pn544_dev *pn544)
{
	static const char * const names[] = { "pvdd", "vbat", "pmuvcc", "sevdd" };
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		pn544->regulators[i].name = names[i];
		pn544->regulators[i].regulator =
			devm_regulator_get_optional(dev, names[i]);
		if (IS_ERR(pn544->regulators[i].regulator)) {
			pn544->regulators[i].regulator = NULL;
			dev_dbg(dev, "optional regulator %s not available\n", names[i]);
		}
	}
}

static int pn544_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pn544_dev *pn544;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP, "adapter does not support I2C transfers\n");

	pn544 = devm_kzalloc(dev, sizeof(*pn544), GFP_KERNEL);
	if (!pn544)
		return -ENOMEM;

	pn544->client = client;
	mutex_init(&pn544->read_mutex);
	mutex_init(&pn544->write_mutex);
	mutex_init(&pn544->power_mutex);
	spin_lock_init(&pn544->irq_enabled_lock);
	init_waitqueue_head(&pn544->read_wq);
	i2c_set_clientdata(client, pn544);

	pn544_get_regulators(dev, pn544);

	ret = pn544_request_gpios(dev, pn544);
	if (ret)
		return ret;

	if (pn544->irq > 0) {
		ret = devm_request_irq(dev, pn544->irq, pn544_irq_handler,
				       IRQF_TRIGGER_HIGH, "pn544", pn544);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request IRQ %d\n",
					     pn544->irq);
		pn544->irq_enabled = true;
		pn544_disable_irq(pn544);
	}

	pn544->miscdev.minor = MISC_DYNAMIC_MINOR;
	pn544->miscdev.name = "pn544";
	pn544->miscdev.fops = &pn544_fops;
	pn544->miscdev.parent = dev;

	ret = misc_register(&pn544->miscdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register /dev/pn544\n");

	dev_info(dev, "PN547/PN544 NFC registered addr=0x%02x irq_gpio=%d irq=%s ven=%s firm_gpio=%d irq_num=%d\n",
		 client->addr, irq_gpio,
		 pn544->irq_desc ? "gizmo-intc:nfc_intr" :
		 (pn544_gpio_valid(irq_gpio) ? "legacy-gpio" : "none"),
		 pn544->ven_desc ? "gizmo-gpio-enables:1" :
		 (pn544_gpio_valid(ven_gpio) ? "legacy-gpio" : "none"),
		 firm_gpio, pn544->irq);
	return 0;
}

static void pn544_remove(struct i2c_client *client)
{
	struct pn544_dev *pn544 = i2c_get_clientdata(client);

	misc_deregister(&pn544->miscdev);
	pn544_power_off(pn544);
}

static const struct i2c_device_id pn544_id[] = {
	{ "pn547", 0 },
	{ "pn544", 0 },
	{ "pn54x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pn544_id);

static const struct of_device_id pn544_of_match[] = {
	{ .compatible = "nxp,pn547" },
	{ .compatible = "nxp,pn544" },
	{ }
};
MODULE_DEVICE_TABLE(of, pn544_of_match);

static struct i2c_driver pn544_driver = {
	.driver = {
		.name = "pn544",
		.of_match_table = pn544_of_match,
	},
	.probe = pn544_probe,
	.remove = pn544_remove,
	.id_table = pn544_id,
};

static void pn544_register_gpio_lookup(void)
{
	if (bus < 0 || pn544_gpio_lookup_registered)
		return;

	snprintf(pn544_i2c_dev_id, sizeof(pn544_i2c_dev_id), "%d-%04x",
		 bus, address);
	pn544_gpio_lookup.dev_id = pn544_i2c_dev_id;
	gpiod_add_lookup_table(&pn544_gpio_lookup);
	pn544_gpio_lookup_registered = true;
}

static void pn544_unregister_gpio_lookup(void)
{
	if (!pn544_gpio_lookup_registered)
		return;

	gpiod_remove_lookup_table(&pn544_gpio_lookup);
	pn544_gpio_lookup_registered = false;
}

static int __init pn544_init(void)
{
	struct i2c_adapter *adapter;
	struct i2c_board_info info = {
		I2C_BOARD_INFO("pn547", PN544_I2C_ADDR),
	};
	int ret;

	pn544_register_gpio_lookup();

	ret = i2c_add_driver(&pn544_driver);
	if (ret) {
		pn544_unregister_gpio_lookup();
		return ret;
	}

	if (bus < 0)
		return 0;

	info.addr = address;
	adapter = i2c_get_adapter(bus);
	if (!adapter) {
		i2c_del_driver(&pn544_driver);
		pn544_unregister_gpio_lookup();
		return -ENODEV;
	}

	created_client = i2c_new_client_device(adapter, &info);
	i2c_put_adapter(adapter);
	if (IS_ERR(created_client)) {
		ret = PTR_ERR(created_client);
		created_client = NULL;
		i2c_del_driver(&pn544_driver);
		pn544_unregister_gpio_lookup();
		return ret;
	}

	return 0;
}

static void __exit pn544_exit(void)
{
	if (created_client) {
		i2c_unregister_device(created_client);
		created_client = NULL;
	}
	i2c_del_driver(&pn544_driver);
	pn544_unregister_gpio_lookup();
}

module_init(pn544_init);
module_exit(pn544_exit);

MODULE_AUTHOR("GizmoToolchain");
MODULE_DESCRIPTION("Gizmo PN547/PN544 NFC misc device driver");
MODULE_LICENSE("GPL");
