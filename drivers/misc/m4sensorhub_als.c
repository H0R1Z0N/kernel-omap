/*
 *  Copyright (C) 2012-2014 Motorola, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Adds ability to program periodic interrupts from user space that
 *  can wake the phone out of low power modes.
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/m4sensorhub.h>
#include <linux/input.h>
#include <linux/slab.h>

#define m4als_err(format, args...)  KDEBUG(M4SH_ERROR, format, ## args)

#define M4ALS_DRIVER_NAME           "m4sensorhub_als"

#define M4ALS_IRQ_ENABLED_BIT       0

struct m4als_driver_data {
	struct platform_device      *pdev;
	struct m4sensorhub_data     *m4;
	struct mutex                mutex; /* controls driver entry points */
	struct input_dev            *indev;

	uint16_t        luminosity;
	int16_t         samplerate;

	uint16_t        status;
};


static void m4als_isr(enum m4sensorhub_irqs int_event, void *handle)
{
	int err = 0;
	struct m4als_driver_data *dd = handle;
	int size = 0;
	uint16_t luminosity = 0;

	mutex_lock(&(dd->mutex));

	size = m4sensorhub_reg_getsize(dd->m4, M4SH_REG_LIGHTSENSOR_SIGNAL);
	if (size < 0) {
		m4als_err("%s: Reading from invalid register %d.\n",
			  __func__, size);
		err = size;
		goto m4als_isr_fail;
	}

	err = m4sensorhub_reg_read(dd->m4, M4SH_REG_LIGHTSENSOR_SIGNAL,
		(char *)&luminosity);
	if (err < 0) {
		m4als_err("%s: Failed to read luminosity data.\n", __func__);
		goto m4als_isr_fail;
	} else if (err != size) {
		m4als_err("%s: Read %d bytes instead of %d.\n",
			  __func__, err, size);
		goto m4als_isr_fail;
	}

	dd->luminosity = luminosity;

	input_event(dd->indev, EV_MSC, MSC_RAW, dd->luminosity);
	input_sync(dd->indev);

m4als_isr_fail:
	if (err < 0)
		m4als_err("%s: Failed with error code %d.\n", __func__, err);

	mutex_unlock(&(dd->mutex));

	return;
}

static int m4als_set_samplerate(struct m4als_driver_data *dd, int16_t rate)
{
	int err = 0;
	int size = 0;

	if (rate == dd->samplerate)
		goto m4als_set_samplerate_fail;

	size = m4sensorhub_reg_getsize(dd->m4, M4SH_REG_LIGHTSENSOR_SAMPLERATE);
	if (size < 0) {
		m4als_err("%s: Writing to invalid register %d.\n",
			  __func__, size);
		err = size;
		goto m4als_set_samplerate_fail;
	}

	err = m4sensorhub_reg_write(dd->m4, M4SH_REG_LIGHTSENSOR_SAMPLERATE,
		(char *)&rate, m4sh_no_mask);
	if (err < 0) {
		m4als_err("%s: Failed to set sample rate.\n", __func__);
		goto m4als_set_samplerate_fail;
	} else if (err != size) {
		m4als_err("%s: Wrote %d bytes instead of %d.\n",
			  __func__, err, size);
		goto m4als_set_samplerate_fail;
	}

	dd->samplerate = rate;

m4als_set_samplerate_fail:
	return err;
}

static ssize_t m4als_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct m4als_driver_data *dd = dev_get_drvdata(dev);

	if (dd->status & (1 << M4ALS_IRQ_ENABLED_BIT))
		return sprintf(buf, "Sensor is ENABLED.\n");
	else
		return sprintf(buf, "Sensor is DISABLED.\n");
}
static ssize_t m4als_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int err = 0;
	struct m4als_driver_data *dd = dev_get_drvdata(dev);
	int value = 0;

	mutex_lock(&(dd->mutex));

	err = kstrtoint(buf, 10, &value);
	if (err < 0) {
		m4als_err("%s: Failed to convert value.\n", __func__);
		goto m4als_enable_store_exit;
	}

	switch (value) {
	case 0:
		if (dd->status & (1 << M4ALS_IRQ_ENABLED_BIT)) {
			err = m4sensorhub_irq_disable(dd->m4,
				M4SH_IRQ_LIGHTSENSOR_DATA_READY);
			if (err < 0) {
				m4als_err("%s: Failed to disable interrupt.\n",
					  __func__);
				goto m4als_enable_store_exit;
			}
			dd->status = dd->status & ~(1 << M4ALS_IRQ_ENABLED_BIT);
		}
		break;

	case 1:
		if (!(dd->status & (1 << M4ALS_IRQ_ENABLED_BIT))) {
			err = m4sensorhub_irq_enable(dd->m4,
				M4SH_IRQ_LIGHTSENSOR_DATA_READY);
			if (err < 0) {
				m4als_err("%s: Failed to enable interrupt.\n",
					  __func__);
				goto m4als_enable_store_exit;
			}
			dd->status = dd->status | (1 << M4ALS_IRQ_ENABLED_BIT);
		}
		break;

	default:
		m4als_err("%s: Invalid value %d passed.\n", __func__, value);
		err = -EINVAL;
		goto m4als_enable_store_exit;
	}

m4als_enable_store_exit:
	if (err < 0)
		m4als_err("%s: Failed with error code %d.\n", __func__, err);

	mutex_unlock(&(dd->mutex));

	return size;
}
static DEVICE_ATTR(als_enable, S_IRUSR | S_IWUSR,
		m4als_enable_show, m4als_enable_store);

static ssize_t m4als_setrate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct m4als_driver_data *dd = dev_get_drvdata(dev);

	return sprintf(buf, "Current rate: %hd\n", dd->samplerate);
}
static ssize_t m4als_setrate_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int err = 0;
	struct m4als_driver_data *dd = dev_get_drvdata(dev);
	int value = 0;

	mutex_lock(&(dd->mutex));

	err = kstrtoint(buf, 10, &value);
	if (err < 0) {
		m4als_err("%s: Failed to convert value.\n", __func__);
		goto m4als_enable_store_exit;
	}

	if ((value < -32768) || (value > 32767)) {
		m4als_err("%s: Value of %d is outside range of int16_t.\n",
			  __func__, value);
		err = -EOVERFLOW;
		goto m4als_enable_store_exit;
	}

	err = m4als_set_samplerate(dd, value);
	if (err < 0) {
		m4als_err("%s: Failed to set sample rate.\n", __func__);
		goto m4als_enable_store_exit;
	}

m4als_enable_store_exit:
	if (err < 0)
		m4als_err("%s: Failed with error code %d.\n", __func__, err);

	mutex_unlock(&(dd->mutex));

	return size;
}
static DEVICE_ATTR(als_setrate, S_IRUSR | S_IWUSR,
		m4als_setrate_show, m4als_setrate_store);

static ssize_t m4als_luminosity_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct m4als_driver_data *dd = dev_get_drvdata(dev);

	return sprintf(buf, "Current luminosity: %hd\n", dd->luminosity);
}
static DEVICE_ATTR(luminosity, S_IRUGO, m4als_luminosity_show, NULL);

static int m4als_create_sysfs(struct m4als_driver_data *dd)
{
	int err = 0;

	err = device_create_file(&(dd->pdev->dev), &dev_attr_als_enable);
	if (err < 0) {
		m4als_err("%s: Failed to create als_enable %s %d.\n",
			  __func__, "with error", err);
		goto m4als_create_sysfs_als_enable_fail;
	}

	err = device_create_file(&(dd->pdev->dev), &dev_attr_als_setrate);
	if (err < 0) {
		m4als_err("%s: Failed to create als_setrate %s %d.\n",
			  __func__, "with error", err);
		goto m4als_create_sysfs_als_setrate_fail;
	}

	err = device_create_file(&(dd->pdev->dev), &dev_attr_luminosity);
	if (err < 0) {
		m4als_err("%s: Failed to create luminosity %s %d.\n",
			  __func__, "with error", err);
		goto m4als_create_sysfs_luminosity_fail;
	}

	goto m4als_create_sysfs_als_enable_fail;

m4als_create_sysfs_luminosity_fail:
	device_remove_file(&(dd->pdev->dev), &dev_attr_als_setrate);
m4als_create_sysfs_als_setrate_fail:
	device_remove_file(&(dd->pdev->dev), &dev_attr_als_enable);
m4als_create_sysfs_als_enable_fail:
	return err;
}

static int m4als_remove_sysfs(struct m4als_driver_data *dd)
{
	int err = 0;

	device_remove_file(&(dd->pdev->dev), &dev_attr_als_enable);
	device_remove_file(&(dd->pdev->dev), &dev_attr_als_setrate);
	device_remove_file(&(dd->pdev->dev), &dev_attr_luminosity);

	return err;
}

static int m4als_create_m4eventdev(struct m4als_driver_data *dd)
{
	int err = 0;

	dd->indev = input_allocate_device();
	if (dd->indev == NULL) {
		m4als_err("%s: Failed to allocate input device.\n",
			  __func__);
		err = -ENODATA;
		goto m4als_create_m4eventdev_fail;
	}

	dd->indev->name = M4ALS_DRIVER_NAME;
	input_set_drvdata(dd->indev, dd);
	input_set_capability(dd->indev, EV_MSC, MSC_RAW);

	err = input_register_device(dd->indev);
	if (err < 0) {
		m4als_err("%s: Failed to register input device.\n",
			  __func__);
		input_free_device(dd->indev);
		dd->indev = NULL;
		goto m4als_create_m4eventdev_fail;
	}

m4als_create_m4eventdev_fail:
	return err;
}

static int m4als_driver_init(struct init_calldata *p_arg)
{
	struct m4als_driver_data *dd = p_arg->p_data;
	int err = 0;

	mutex_lock(&(dd->mutex));

	err = m4als_create_m4eventdev(dd);
	if (err < 0) {
		m4als_err("%s: Failed to create M4 event device.\n", __func__);
		goto m4als_driver_init_fail;
	}

	err = m4als_create_sysfs(dd);
	if (err < 0) {
		m4als_err("%s: Failed to create sysfs.\n", __func__);
		goto m4als_driver_init_sysfs_fail;
	}

	err = m4sensorhub_irq_register(dd->m4, M4SH_IRQ_LIGHTSENSOR_DATA_READY,
		m4als_isr, dd);
	if (err < 0) {
		m4als_err("%s: Failed to register M4 IRQ.\n", __func__);
		goto m4als_driver_init_irq_fail;
	}

	goto m4als_driver_init_exit;

m4als_driver_init_irq_fail:
	m4als_remove_sysfs(dd);
m4als_driver_init_sysfs_fail:
	input_unregister_device(dd->indev);
m4als_driver_init_fail:
	m4als_err("%s: Init failed with error code %d.\n", __func__, err);
m4als_driver_init_exit:
	mutex_unlock(&(dd->mutex));
	return err;
}

static int m4als_probe(struct platform_device *pdev)
{
	struct m4als_driver_data *dd = NULL;
	int err = 0;

	dd = kzalloc(sizeof(*dd), GFP_KERNEL);
	if (dd == NULL) {
		m4als_err("%s: Failed to allocate driver data.\n", __func__);
		err = -ENOMEM;
		goto m4als_probe_fail_nodd;
	}

	dd->pdev = pdev;
	mutex_init(&(dd->mutex));
	platform_set_drvdata(pdev, dd);

	dd->m4 = m4sensorhub_client_get_drvdata();
	if (dd->m4 == NULL) {
		m4als_err("%s: M4 sensor data is NULL.\n", __func__);
		err = -ENODATA;
		goto m4als_probe_fail;
	}

	err = m4sensorhub_register_initcall(m4als_driver_init, dd);
	if (err < 0) {
		m4als_err("%s: Failed to register initcall.\n", __func__);
		goto m4als_probe_fail;
	}

	return 0;

m4als_probe_fail:
	mutex_destroy(&(dd->mutex));
	kfree(dd);
m4als_probe_fail_nodd:
	m4als_err("%s: Probe failed with error code %d.\n", __func__, err);
	return err;
}

static int __exit m4als_remove(struct platform_device *pdev)
{
	struct m4als_driver_data *dd = platform_get_drvdata(pdev);

	mutex_lock(&(dd->mutex));
	m4als_remove_sysfs(dd);
	if (dd->status & (1 << M4ALS_IRQ_ENABLED_BIT)) {
		m4sensorhub_irq_disable(dd->m4, M4SH_IRQ_PRESSURE_DATA_READY);
		dd->status = dd->status & ~(1 << M4ALS_IRQ_ENABLED_BIT);
	}
	m4sensorhub_irq_unregister(dd->m4, M4SH_IRQ_PRESSURE_DATA_READY);
	m4sensorhub_unregister_initcall(m4als_driver_init);
	if (dd->indev != NULL)
		input_unregister_device(dd->indev);
	mutex_destroy(&(dd->mutex));
	kfree(dd);

	return 0;
}

static struct of_device_id m4als_match_tbl[] = {
	{ .compatible = "mot,m4als" },
	{},
};

static struct platform_driver m4als_driver = {
	.probe		= m4als_probe,
	.remove		= __exit_p(m4als_remove),
	.shutdown	= NULL,
	.suspend	= NULL,
	.resume		= NULL,
	.driver		= {
		.name	= M4ALS_DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(m4als_match_tbl),
	},
};

static int __init m4als_init(void)
{
	return platform_driver_register(&m4als_driver);
}

static void __exit m4als_exit(void)
{
	platform_driver_unregister(&m4als_driver);
}

module_init(m4als_init);
module_exit(m4als_exit);

MODULE_ALIAS("platform:m4als");
MODULE_DESCRIPTION("M4 Sensor Hub Ambient Light client driver");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
