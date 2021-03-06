/*
 *  Copyright (C) 2012-2015 Motorola, Inc.
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
#include <linux/delay.h>

#define m4acc_err(format, args...)  KDEBUG(M4SH_ERROR, format, ## args)

#define M4ACC_DRIVER_NAME           "m4sensorhub_accelerometer"

#define M4ACC_IRQ_ENABLED_BIT       0
#define M4ACC_OVERSAMPLING_BIT      1
#define M4ACC_READ_OVERSAMPLE_BIT   2

struct m4acc_sensor_data {
	int32_t         x;
	int32_t         y;
	int32_t         z;
};

struct m4acc_driver_data {
	struct platform_device      *pdev;
	struct m4sensorhub_data     *m4;
	struct mutex                mutex; /* controls driver entry points */
	struct input_dev            *indev;
	struct delayed_work         m4acc_work;

	struct m4acc_sensor_data    sensdat;

	int16_t         samplerate;
	int16_t         latest_samplerate;
	int16_t         fastest_rate;
	int16_t         oversample_rate;
	uint16_t        status;
};

static void m4acc_work_func(struct work_struct *work)
{
	int err = 0;
	struct m4acc_driver_data *dd = container_of(work,
						    struct m4acc_driver_data,
						    m4acc_work.work);
	int size = 0;
	enum m4sensorhub_reg reg[3];
	struct timespec ts;

	mutex_lock(&(dd->mutex));

	get_monotonic_boottime(&ts);

	reg[0] = M4SH_REG_ACCEL_X;
	reg[1] = M4SH_REG_ACCEL_Y;
	reg[2] = M4SH_REG_ACCEL_Z;

	if (dd->status & (1 << M4ACC_OVERSAMPLING_BIT)) {
		if (dd->status & (1 << M4ACC_READ_OVERSAMPLE_BIT)) {
			reg[0] = M4SH_REG_ACCEL_X2;
			reg[1] = M4SH_REG_ACCEL_Y2;
			reg[2] = M4SH_REG_ACCEL_Z2;
			dd->status &= ~(1 << M4ACC_READ_OVERSAMPLE_BIT);
		} else {
			dd->status |= (1 << M4ACC_READ_OVERSAMPLE_BIT);
		}
	}

	size = m4sensorhub_reg_getsize(dd->m4, reg[0]);
	if (size < 0) {
		m4acc_err("%s: Reading from invalid register %d.\n",
			  __func__, size);
		err = size;
		goto m4acc_isr_fail;
	}

	err = m4sensorhub_reg_read(dd->m4, reg[0],
		(char *)&(dd->sensdat.x));
	if (err < 0) {
		m4acc_err("%s: Failed to read X data.\n", __func__);
		goto m4acc_isr_fail;
	} else if (err != size) {
		m4acc_err("%s: Read %d bytes instead of %d.\n",
			  __func__, err, size);
		goto m4acc_isr_fail;
	}

	size = m4sensorhub_reg_getsize(dd->m4, reg[1]);
	if (size < 0) {
		m4acc_err("%s: Reading from invalid register %d.\n",
			  __func__, size);
		err = size;
		goto m4acc_isr_fail;
	}

	err = m4sensorhub_reg_read(dd->m4, reg[1],
		(char *)&(dd->sensdat.y));
	if (err < 0) {
		m4acc_err("%s: Failed to read Y data.\n", __func__);
		goto m4acc_isr_fail;
	} else if (err != size) {
		m4acc_err("%s: Read %d bytes instead of %d.\n",
			  __func__, err, size);
		goto m4acc_isr_fail;
	}

	size = m4sensorhub_reg_getsize(dd->m4, reg[2]);
	if (size < 0) {
		m4acc_err("%s: Reading from invalid register %d.\n",
			  __func__, size);
		err = size;
		goto m4acc_isr_fail;
	}

	err = m4sensorhub_reg_read(dd->m4, reg[2],
		(char *)&(dd->sensdat.z));
	if (err < 0) {
		m4acc_err("%s: Failed to read Z data.\n", __func__);
		goto m4acc_isr_fail;
	} else if (err != size) {
		m4acc_err("%s: Read %d bytes instead of %d.\n",
			  __func__, err, size);
		goto m4acc_isr_fail;
	}

	input_event(dd->indev, EV_MSC, MSC_TIMESTAMP, ts.tv_sec);
	input_event(dd->indev, EV_MSC, MSC_TIMESTAMP, ts.tv_nsec);
	input_report_abs(dd->indev, ABS_X, dd->sensdat.x);
	input_report_abs(dd->indev, ABS_Y, dd->sensdat.y);
	input_report_abs(dd->indev, ABS_Z, dd->sensdat.z);
	input_sync(dd->indev);

	if (dd->samplerate > 0)
		queue_delayed_work(system_freezable_wq, &(dd->m4acc_work),
				      msecs_to_jiffies(dd->samplerate));

m4acc_isr_fail:
	if (err < 0)
		m4acc_err("%s: Failed with error code %d.\n", __func__, err);

	mutex_unlock(&(dd->mutex));

	return;
}

static int m4acc_set_samplerate(struct m4acc_driver_data *dd, int16_t rate)
{
	int err = 0;
	int size = 0;

	if ((rate >= 0) && (rate < dd->fastest_rate)) {
		rate = dd->oversample_rate;
		dd->status |= (1 << M4ACC_OVERSAMPLING_BIT);
		dd->status |= (1 << M4ACC_READ_OVERSAMPLE_BIT);
	} else {
		dd->status &= ~(1 << M4ACC_OVERSAMPLING_BIT);
		dd->status &= ~(1 << M4ACC_READ_OVERSAMPLE_BIT);
	}

	/*
	 * This variable is always updated irrespective of
	 * the transaction status (used for later recovery).
	 */
	dd->latest_samplerate = rate;

	if (rate == dd->samplerate)
		goto m4acc_set_samplerate_fail;

	size = m4sensorhub_reg_getsize(dd->m4, M4SH_REG_ACCEL_SAMPLERATE);
	if (size < 0) {
		m4acc_err("%s: Writing to invalid register %d.\n",
			  __func__, size);
		err = size;
		goto m4acc_set_samplerate_fail;
	}

	err = m4sensorhub_reg_write(dd->m4, M4SH_REG_ACCEL_SAMPLERATE,
		(char *)&rate, m4sh_no_mask);
	if (err < 0) {
		m4acc_err("%s: Failed to set sample rate.\n", __func__);
		goto m4acc_set_samplerate_fail;
	} else if (err != size) {
		m4acc_err("%s: Wrote %d bytes instead of %d.\n",
			  __func__, err, size);
		err = -EBADE;
		goto m4acc_set_samplerate_fail;
	}
	cancel_delayed_work(&(dd->m4acc_work));
	dd->samplerate = rate;
	if (dd->samplerate > 0)
		queue_delayed_work(system_freezable_wq, &(dd->m4acc_work),
				      msecs_to_jiffies(rate));

m4acc_set_samplerate_fail:
	return err;
}

static ssize_t m4acc_setrate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct m4acc_driver_data *dd = dev_get_drvdata(dev);

	return sprintf(buf, "Current rate: %hd\n", dd->samplerate);
}
static ssize_t m4acc_setrate_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int err = 0;
	struct m4acc_driver_data *dd = dev_get_drvdata(dev);
	int value = 0;

	mutex_lock(&(dd->mutex));

	err = kstrtoint(buf, 10, &value);
	if (err < 0) {
		m4acc_err("%s: Failed to convert value.\n", __func__);
		goto m4acc_enable_store_exit;
	}

	if ((value < -1) || (value > 32767)) {
		m4acc_err("%s: Invalid sample rate requested = %d\n",
			  __func__, value);
		err = -EOVERFLOW;
		goto m4acc_enable_store_exit;
	}

	err = m4acc_set_samplerate(dd, value);
	if (err < 0) {
		m4acc_err("%s: Failed to set sample rate.\n", __func__);
		goto m4acc_enable_store_exit;
	}

m4acc_enable_store_exit:
	if (err < 0)
		m4acc_err("%s: Failed with error code %d.\n", __func__, err);

	mutex_unlock(&(dd->mutex));

	return size;
}
static DEVICE_ATTR(setrate, S_IRUSR | S_IWUSR,
		m4acc_setrate_show, m4acc_setrate_store);

static ssize_t m4acc_sensordata_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct m4acc_driver_data *dd = dev_get_drvdata(dev);
	ssize_t size = 0;

	mutex_lock(&(dd->mutex));
	size = snprintf(buf, PAGE_SIZE, "%s%d\n%s%d\n%s%d\n",
		"X: ", dd->sensdat.x,
		"Y: ", dd->sensdat.y,
		"Z: ", dd->sensdat.z);
	mutex_unlock(&(dd->mutex));
	return size;
}
static DEVICE_ATTR(sensordata, S_IRUGO, m4acc_sensordata_show, NULL);

static int m4acc_create_sysfs(struct m4acc_driver_data *dd)
{
	int err = 0;

	err = device_create_file(&(dd->pdev->dev), &dev_attr_setrate);
	if (err < 0) {
		m4acc_err("%s: Failed to create setrate %s %d.\n",
			  __func__, "with error", err);
		goto m4acc_create_sysfs_exit;
	}

	err = device_create_file(&(dd->pdev->dev), &dev_attr_sensordata);
	if (err < 0) {
		m4acc_err("%s: Failed to create sensordata %s %d.\n",
			  __func__, "with error", err);
		goto m4acc_create_sysfs_sensordata_fail;
	}

	goto m4acc_create_sysfs_exit;

m4acc_create_sysfs_sensordata_fail:
	device_remove_file(&(dd->pdev->dev), &dev_attr_setrate);
m4acc_create_sysfs_exit:
	return err;
}

static int m4acc_remove_sysfs(struct m4acc_driver_data *dd)
{
	int err = 0;

	device_remove_file(&(dd->pdev->dev), &dev_attr_setrate);
	device_remove_file(&(dd->pdev->dev), &dev_attr_sensordata);

	return err;
}

static int m4acc_create_m4eventdev(struct m4acc_driver_data *dd)
{
	int err = 0;

	dd->indev = input_allocate_device();
	if (dd->indev == NULL) {
		m4acc_err("%s: Failed to allocate input device.\n",
			  __func__);
		err = -ENODATA;
		goto m4acc_create_m4eventdev_fail;
	}

	dd->indev->name = M4ACC_DRIVER_NAME;
	input_set_drvdata(dd->indev, dd);
	set_bit(EV_ABS, dd->indev->evbit);
	input_set_abs_params(dd->indev, ABS_X, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(dd->indev, ABS_Y, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(dd->indev, ABS_Z, INT_MIN, INT_MAX, 0, 0);
	input_set_capability(dd->indev, EV_MSC, MSC_TIMESTAMP);

	err = input_register_device(dd->indev);
	if (err < 0) {
		m4acc_err("%s: Failed to register input device.\n",
			  __func__);
		input_free_device(dd->indev);
		dd->indev = NULL;
		goto m4acc_create_m4eventdev_fail;
	}

m4acc_create_m4eventdev_fail:
	return err;
}

static void m4acc_panic_restore(struct m4sensorhub_data *m4sensorhub,
				void *data)
{
	int size, err;
	struct m4acc_driver_data *dd = (struct m4acc_driver_data *)data;

	if (dd == NULL) {
		m4acc_err("%s: Driver data is null, unable to restore\n",
			  __func__);
		return;
	}

	mutex_lock(&(dd->mutex));

	size = m4sensorhub_reg_getsize(dd->m4,
				       M4SH_REG_ACCEL_SAMPLERATE);
	err = m4sensorhub_reg_write(dd->m4, M4SH_REG_ACCEL_SAMPLERATE,
				   (char *)&dd->samplerate, m4sh_no_mask);
	if (err < 0) {
		m4acc_err("%s: Failed to set sample rate.\n", __func__);
	} else if (err != size) {
		m4acc_err("%s: Wrote %d bytes instead of %d.\n",
			  __func__, err, size);
	}
	cancel_delayed_work(&(dd->m4acc_work));
	if (dd->samplerate > 0)
		queue_delayed_work(system_freezable_wq, &(dd->m4acc_work),
				      msecs_to_jiffies(dd->samplerate));
	mutex_unlock(&(dd->mutex));
}

static int m4acc_driver_init(struct init_calldata *p_arg)
{
	struct m4acc_driver_data *dd = p_arg->p_data;
	int err = 0;

	mutex_lock(&(dd->mutex));

	err = m4acc_create_m4eventdev(dd);
	if (err < 0) {
		m4acc_err("%s: Failed to create M4 event device.\n", __func__);
		goto m4acc_driver_init_fail;
	}

	INIT_DELAYED_WORK(&(dd->m4acc_work), m4acc_work_func);

	err = m4sensorhub_panic_register(dd->m4, PANICHDL_ACCEL_RESTORE,
					 m4acc_panic_restore, dd);
	if (err < 0)
		KDEBUG(M4SH_ERROR, "Acc panic callback register failed\n");
	goto m4acc_driver_init_exit;

m4acc_driver_init_fail:
	m4acc_err("%s: Init failed with error code %d.\n", __func__, err);
m4acc_driver_init_exit:
	mutex_unlock(&(dd->mutex));
	return err;
}

static int m4acc_probe(struct platform_device *pdev)
{
	struct m4acc_driver_data *dd = NULL;
	int err = 0;

	dd = kzalloc(sizeof(*dd), GFP_KERNEL);
	if (dd == NULL) {
		m4acc_err("%s: Failed to allocate driver data.\n", __func__);
		err = -ENOMEM;
		goto m4acc_probe_fail_nodd;
	}

	dd->pdev = pdev;
	mutex_init(&(dd->mutex));
	platform_set_drvdata(pdev, dd);
	dd->samplerate = -1; /* We always start disabled */
	dd->latest_samplerate = dd->samplerate;
	dd->fastest_rate = 40; /* in milli secs */
	dd->oversample_rate = 20; /* in milli secs */

	dd->m4 = m4sensorhub_client_get_drvdata();
	if (dd->m4 == NULL) {
		m4acc_err("%s: M4 sensor data is NULL.\n", __func__);
		err = -ENODATA;
		goto m4acc_probe_fail;
	}

	err = m4sensorhub_register_initcall(m4acc_driver_init, dd);
	if (err < 0) {
		m4acc_err("%s: Failed to register initcall.\n", __func__);
		goto m4acc_probe_fail;
	}

	err = m4acc_create_sysfs(dd);
	if (err < 0) {
		m4acc_err("%s: Failed to create sysfs.\n", __func__);
		goto m4acc_driver_init_sysfs_fail;
	}

	return 0;

m4acc_driver_init_sysfs_fail:
	m4sensorhub_unregister_initcall(m4acc_driver_init);
m4acc_probe_fail:
	mutex_destroy(&(dd->mutex));
	kfree(dd);
m4acc_probe_fail_nodd:
	m4acc_err("%s: Probe failed with error code %d.\n", __func__, err);
	return err;
}

static int __exit m4acc_remove(struct platform_device *pdev)
{
	struct m4acc_driver_data *dd = platform_get_drvdata(pdev);

	mutex_lock(&(dd->mutex));
	cancel_delayed_work(&(dd->m4acc_work));
	m4acc_remove_sysfs(dd);
	m4sensorhub_unregister_initcall(m4acc_driver_init);
	if (dd->indev != NULL)
		input_unregister_device(dd->indev);
	mutex_destroy(&(dd->mutex));
	kfree(dd);

	return 0;
}

static int m4acc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct m4acc_driver_data *dd = platform_get_drvdata(pdev);
	if (m4acc_set_samplerate(dd, dd->latest_samplerate) < 0)
		m4acc_err("%s: setrate retry failed\n", __func__);
	return 0;
}

static struct of_device_id m4acc_match_tbl[] = {
	{ .compatible = "mot,m4accelerometer" },
	{},
};

static struct platform_driver m4acc_driver = {
	.probe		= m4acc_probe,
	.remove		= __exit_p(m4acc_remove),
	.shutdown	= NULL,
	.suspend	= m4acc_suspend,
	.resume		= NULL,
	.driver		= {
		.name	= M4ACC_DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(m4acc_match_tbl),
	},
};

static int __init m4acc_init(void)
{
	return platform_driver_register(&m4acc_driver);
}

static void __exit m4acc_exit(void)
{
	platform_driver_unregister(&m4acc_driver);
}

module_init(m4acc_init);
module_exit(m4acc_exit);

MODULE_ALIAS("platform:m4acc");
MODULE_DESCRIPTION("M4 Sensor Hub Accelerometer client driver");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
