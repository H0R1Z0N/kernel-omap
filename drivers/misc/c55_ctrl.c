/*
 * Copyright (C) 2013 Motorola Mobility LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/wakelock.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>

enum {
	C55_OFF,
	C55_ON,
	C55_MODE_MAX
};

struct c55_ctrl_data {
	int int_gpio;
	struct wake_lock wake_lock;
	struct regulator *reg_vddc;
	struct regulator *reg_vddldo;
	struct pinctrl *pctrl;
	struct pinctrl_state *states[C55_MODE_MAX];
	int c55_mode;
	struct mutex ctrl_mutex;	/* mutex to handle critical area */
};

const char *c55_pin_state_labels[C55_MODE_MAX] = {
	"off",
	"on"
};

#define NUM_GPIOS 3

const char *gpio_labels[NUM_GPIOS] = {
	"gpio_ap_int",
	"gpio_reset",
	"gpio_c55_int"
};

static irqreturn_t c55_ctrl_isr(int irq, void *data)
{
	struct c55_ctrl_data *cdata = data;

	pr_debug("%s: value=%d\n", __func__, gpio_get_value(cdata->int_gpio));

	/* Interrupt is active low */
	if (gpio_get_value(cdata->int_gpio) == 0)
		wake_lock(&cdata->wake_lock);
	else
		wake_unlock(&cdata->wake_lock);

	return IRQ_HANDLED;
}

static void c55_ctrl_int_setup(struct c55_ctrl_data *cdata, int gpio)
{
	int ret;
	int irq = __gpio_to_irq(gpio);
	unsigned int flags = 0;

	if (cdata->int_gpio >= 0) {
		/* Interrupt already registered */
		return;
	}

	/* Interrupt is shared with user space */
	flags |= IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	flags |= IRQF_SHARED;

	ret = request_threaded_irq(irq, c55_ctrl_isr, NULL,
				   flags, "c55_ctrl", cdata);
	if (ret) {
		pr_err("%s: IRQ request failed: %d\n", __func__, ret);
		return;
	}

	enable_irq_wake(irq);
	cdata->int_gpio = gpio;
}

static int c55_ctrl_gpio_setup(struct c55_ctrl_data *cdata, struct device *dev)
{
	int i;

	if (of_gpio_count(dev->of_node) != NUM_GPIOS) {
		dev_err(dev, "%s: gpio count is not %d.\n", __func__, NUM_GPIOS);
		return -EINVAL;
	}

	for (i = 0; i < NUM_GPIOS; i++) {
		enum of_gpio_flags flags;
		int gpio;

		gpio = of_get_gpio_flags(dev->of_node, i, &flags);
		if (gpio < 0) {
			pr_err("%s: of_get_gpio failed: %d\n", __func__, gpio);
			return gpio;
		}

		gpio_request(gpio, gpio_labels[i]);

		gpio_export(gpio, false);
		gpio_export_link(dev, gpio_labels[i], gpio);

		if ((flags & GPIOF_IN) == GPIOF_IN) {
			gpio_direction_input(gpio);
			c55_ctrl_int_setup(cdata, gpio);
		} else {
			if ((flags & GPIOF_OUT_INIT_HIGH) == GPIOF_OUT_INIT_HIGH)
				gpio_direction_output(gpio, 1);
			else
				gpio_direction_output(gpio, 0);
		}
	}

	return 0;
}

static ssize_t c55_ctrl_enable(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct c55_ctrl_data *cdata = dev_get_drvdata(dev);
	int mode;

	if (kstrtoint(buf, 10, &mode) < 0)
		return -EINVAL;

	if (mode >= C55_MODE_MAX) {
		dev_err(dev, "%s: Invalid mode %d\n", __func__, mode);
		return -EINVAL;
	}

	if (mode == cdata->c55_mode)
		return count;

	mutex_lock(&cdata->ctrl_mutex);

	if (mode == C55_ON) {
		pinctrl_select_state(cdata->pctrl, cdata->states[mode]);
		if (cdata->reg_vddc && regulator_enable(cdata->reg_vddc))
			dev_err(dev, "regulator_enable failed for vddc\n");
		if (cdata->reg_vddldo && regulator_enable(cdata->reg_vddldo))
			dev_err(dev, "regulator_enable failed for vddldo\n");
	} else {
		if (cdata->reg_vddldo)
			regulator_disable(cdata->reg_vddldo);
		if (cdata->reg_vddc)
			regulator_disable(cdata->reg_vddc);
		pinctrl_select_state(cdata->pctrl, cdata->states[mode]);
	}

	cdata->c55_mode = mode;

	mutex_unlock(&cdata->ctrl_mutex);

	dev_info(dev, "%s: power = %d\n", __func__, mode);

	return count;
}

static DEVICE_ATTR(enable, S_IWUSR, NULL, c55_ctrl_enable);

static int c55_ctrl_pin_setup(struct device *dev, struct c55_ctrl_data *cdata)
{
	int i, ret = 0;

	cdata->pctrl = devm_pinctrl_get(dev);
	if (IS_ERR(cdata->pctrl)) {
		ret = PTR_ERR(cdata->pctrl);
		dev_dbg(dev, "no pinctrl handle\n");
	}

	for (i = 0; !ret && (i < C55_MODE_MAX); i++) {
		cdata->states[i] = pinctrl_lookup_state(cdata->pctrl,
					c55_pin_state_labels[i]);
		if (IS_ERR(cdata->states[i])) {
			ret = PTR_ERR(cdata->states[i]);
			dev_dbg(dev, "no %s pinctrl state\n",
				c55_pin_state_labels[i]);
		}
	}

	if (!ret) {
		ret = pinctrl_select_state(cdata->pctrl,
					cdata->states[C55_OFF]);
		if (ret)
			dev_dbg(dev, "failed to activate %s pinctrl state\n",
				c55_pin_state_labels[C55_OFF]);
	}

	return ret;
}

static int  c55_ctrl_probe(struct platform_device *pdev)
{
	struct c55_ctrl_data *cdata;
	int ret;
	if (!pdev->dev.of_node) {
		/* Platform data not currently supported */
		dev_err(&pdev->dev, "%s: of devtree data not found\n", __func__);
		return -EINVAL;
	}

	cdata = devm_kzalloc(&pdev->dev, sizeof(*cdata), GFP_KERNEL);
	if (cdata == NULL) {
		dev_err(&pdev->dev, "%s: devm_kzalloc failed.\n", __func__);
		return -ENOMEM;
	}

	mutex_init(&cdata->ctrl_mutex);

	ret = c55_ctrl_pin_setup(&pdev->dev, cdata);
	if (ret) {
		dev_err(&pdev->dev, "%s: c55_ctrl_pin_setup failed.\n",
			__func__);
		return ret;
	}

	cdata->int_gpio = -1;
	ret = c55_ctrl_gpio_setup(cdata, &pdev->dev);

	if (ret) {
		dev_err(&pdev->dev, "%s: c55_ctrl_gpio_setup failed.\n", __func__);
		return ret;
	}

	cdata->reg_vddc = devm_regulator_get(&pdev->dev, "vddc");
	if (IS_ERR(cdata->reg_vddc))
		cdata->reg_vddc = NULL;

	cdata->reg_vddldo = devm_regulator_get(&pdev->dev, "vddldo");
	if (IS_ERR(cdata->reg_vddldo))
		cdata->reg_vddldo = NULL;

	cdata->c55_mode = C55_OFF;

	ret = device_create_file(&pdev->dev, &dev_attr_enable);
	if (ret) {
		dev_err(&pdev->dev, "%s: c55_ctrl creating set_mode failed.\n",
			__func__);
		return ret;
	}

	wake_lock_init(&cdata->wake_lock, WAKE_LOCK_SUSPEND, "c55_ctrl");

	platform_set_drvdata(pdev, cdata);
	return 0;
}

static int  c55_ctrl_remove(struct platform_device *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_enable);
	return 0;
}

static struct of_device_id c55_ctrl_match[] = {
	{.compatible = "ti,c55-ctrl",},
	{},
};
MODULE_DEVICE_TABLE(of, c55_ctrl_match);

static const struct platform_device_id c55_ctrl_id_table[] = {
	{"c55_ctrl", 0},
	{},
};
MODULE_DEVICE_TABLE(of, c55_ctrl_id_table);

static struct platform_driver c55_ctrl_driver = {
	.driver = {
		.name = "c55_ctrl",
		.owner = THIS_MODULE,
		.of_match_table = c55_ctrl_match,
	},
	.probe = c55_ctrl_probe,
	.remove = c55_ctrl_remove,
	.id_table = c55_ctrl_id_table,
};

static int __init c55_ctrl_init(void)
{
	return platform_driver_register(&c55_ctrl_driver);
}

static void __exit c55_ctrl_exit(void)
{
	platform_driver_unregister(&c55_ctrl_driver);
}

module_init(c55_ctrl_init);
module_exit(c55_ctrl_exit);

MODULE_ALIAS("platform:c55_ctrl");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Motorola");
MODULE_DESCRIPTION("TI C55 control driver");
