// SPDX-License-Identifier: GPL-2.0-only
/* MP2650 USB VBUS supply on the OP6893 21651. */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define MP2650_OTG_VOLTAGE		0x06
#define MP2650_OTG_VOLTAGE_MASK		GENMASK(6, 0)
#define MP2650_OTG_5V			0x05
#define MP2650_OTG_CURRENT		0x07
#define MP2650_OTG_CURRENT_MASK		GENMASK(3, 0)
#define MP2650_OTG_1250MA		0x05
#define MP2650_CONTROL			0x08
#define MP2650_CHARGE_EN		BIT(4)
#define MP2650_OTG_EN			BIT(5)
#define MP2650_RESET_MASK		GENMASK(7, 6)
#define MP2650_WATCHDOG			0x09
#define MP2650_WATCHDOG_MASK		GENMASK(5, 4)
#define MP2650_BURST_CONTROL		0x3e
#define MP2650_BOOST_CONTROL		0x3f
#define MP2650_KEY			0x53
#define MP2650_UNLOCK			0x95

struct mp2650_otg {
	struct regmap *map;
	struct gpio_desc *enable;
	struct gpio_desc *input_isolate;
	unsigned int charge;
	unsigned int watchdog;
	bool needs_restore;
};

static int mp2650_set_control(struct mp2650_otg *otg, unsigned int mask,
			      unsigned int value)
{
	/* Never echo either reset command in a read-modify-write. */
	return regmap_update_bits(otg->map, MP2650_CONTROL,
				  mask | MP2650_RESET_MASK, value);
}

static int mp2650_stop(struct mp2650_otg *otg)
{
	int ret, gpio_ret;

	gpio_ret = gpiod_set_value_cansleep(otg->enable, 0);
	ret = mp2650_set_control(otg, MP2650_OTG_EN, 0);
	if (ret || gpio_ret)
		return ret ?: gpio_ret;

	ret = gpiod_set_value_cansleep(otg->input_isolate, 0);
	if (ret || !otg->needs_restore)
		return ret;

	ret = regmap_update_bits(otg->map, MP2650_WATCHDOG,
				 MP2650_WATCHDOG_MASK, otg->watchdog);
	if (ret)
		return ret;

	ret = mp2650_set_control(otg, MP2650_CHARGE_EN, otg->charge);
	if (!ret)
		otg->needs_restore = false;

	return ret;
}

static int mp2650_enable(struct regulator_dev *rdev)
{
	struct mp2650_otg *otg = rdev_get_drvdata(rdev);
	struct regmap *map = otg->map;
	int ret, cleanup, lock_ret;
	unsigned int val;

	if (otg->needs_restore) {
		ret = mp2650_stop(otg);
		if (ret)
			return ret;
	}

	ret = regmap_read(map, MP2650_CONTROL, &val);
	if (ret)
		return ret;
	otg->charge = val & MP2650_CHARGE_EN;

	ret = regmap_read(map, MP2650_WATCHDOG, &val);
	if (ret)
		return ret;
	otg->watchdog = val & MP2650_WATCHDOG_MASK;
	otg->needs_restore = true;

	ret = mp2650_set_control(otg, MP2650_CHARGE_EN, 0);
	if (ret)
		goto stop;

	/* The vendor sequence enables burst mode before starting boost. */
	ret = regmap_write(map, MP2650_KEY, MP2650_UNLOCK);
	if (!ret)
		ret = regmap_write(map, MP2650_BURST_CONTROL, 0);
	lock_ret = regmap_write(map, MP2650_KEY, 0);
	ret = ret ?: lock_ret;
	if (ret)
		goto stop;

	ret = regmap_update_bits(map, MP2650_WATCHDOG, MP2650_WATCHDOG_MASK, 0);
	if (ret)
		goto stop;

	ret = regmap_update_bits(map, MP2650_OTG_VOLTAGE,
				 MP2650_OTG_VOLTAGE_MASK, MP2650_OTG_5V);
	if (ret)
		goto stop;

	ret = regmap_write(map, MP2650_KEY, MP2650_UNLOCK);
	if (!ret)
		ret = regmap_write(map, MP2650_BOOST_CONTROL, 0x20);
	if (!ret)
		ret = gpiod_set_value_cansleep(otg->enable, 1);
	if (!ret)
		ret = regmap_update_bits(map, MP2650_OTG_CURRENT,
					 MP2650_OTG_CURRENT_MASK, MP2650_OTG_1250MA);
	if (!ret)
		ret = mp2650_set_control(otg, MP2650_OTG_EN, MP2650_OTG_EN);
	if (!ret)
		usleep_range(10000, 12000);
	cleanup = regmap_write(map, MP2650_BOOST_CONTROL, 0);
	lock_ret = regmap_write(map, MP2650_KEY, 0);
	ret = ret ?: cleanup ?: lock_ret;
	if (ret)
		goto stop;

	ret = gpiod_set_value_cansleep(otg->input_isolate, 1);
	if (!ret)
		return 0;

stop:
	cleanup = mp2650_stop(otg);
	if (cleanup)
		dev_err(rdev_get_dev(rdev), "Could not roll back OTG: %d\n", cleanup);

	return ret;
}

static int mp2650_disable(struct regulator_dev *rdev)
{
	return mp2650_stop(rdev_get_drvdata(rdev));
}

static int mp2650_is_enabled(struct regulator_dev *rdev)
{
	struct mp2650_otg *otg = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret;

	ret = regmap_read(otg->map, MP2650_CONTROL, &val);
	return ret ?: !!(val & MP2650_OTG_EN);
}

static const struct regulator_ops mp2650_ops = {
	.enable = mp2650_enable,
	.disable = mp2650_disable,
	.is_enabled = mp2650_is_enabled,
	.list_voltage = regulator_list_voltage_linear,
};

static const struct regulator_desc mp2650_desc = {
	.name = "mp2650-vbus",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &mp2650_ops,
	.n_voltages = 1,
	.min_uV = 5000000,
	.fixed_uV = 5000000,
};

static const struct regmap_config mp2650_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MP2650_KEY,
};

static int mp2650_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct mp2650_otg *otg;
	unsigned int val;
	int ret;

	otg = devm_kzalloc(dev, sizeof(*otg), GFP_KERNEL);
	if (!otg)
		return -ENOMEM;

	otg->map = devm_regmap_init_i2c(client, &mp2650_regmap);
	if (IS_ERR(otg->map))
		return PTR_ERR(otg->map);

	ret = regmap_read(otg->map, MP2650_CONTROL, &val);
	if (ret)
		return dev_err_probe(dev, ret, "Could not read OTG control\n");

	if (val & MP2650_OTG_EN)
		return dev_err_probe(dev, -EBUSY, "OTG already enabled at boot\n");

	otg->enable = devm_gpiod_get(dev, "otg-enable", GPIOD_OUT_LOW);
	if (IS_ERR(otg->enable))
		return dev_err_probe(dev, PTR_ERR(otg->enable), "Missing OTG enable GPIO\n");

	otg->input_isolate = devm_gpiod_get(dev, "input-isolate", GPIOD_OUT_LOW);
	if (IS_ERR(otg->input_isolate))
		return dev_err_probe(dev, PTR_ERR(otg->input_isolate),
				     "Missing charge input isolation GPIO\n");

	config.driver_data = otg;
	config.of_node = dev->of_node;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node, &mp2650_desc);
	if (!config.init_data)
		return -EINVAL;

	rdev = devm_regulator_register(dev, &mp2650_desc, &config);
	return PTR_ERR_OR_ZERO(rdev);
}

static const struct of_device_id mp2650_of_match[] = {
	{ .compatible = "oplus,21651-mp2650-otg" },
	{ }
};
MODULE_DEVICE_TABLE(of, mp2650_of_match);

static struct i2c_driver mp2650_driver = {
	.driver = {
		.name = "mp2650-otg",
		.of_match_table = mp2650_of_match,
	},
	.probe = mp2650_probe,
};
module_i2c_driver(mp2650_driver);

MODULE_DESCRIPTION("OP6893 21651 MP2650 USB VBUS supply");
MODULE_LICENSE("GPL");
