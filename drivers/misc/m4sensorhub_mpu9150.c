/*
 *  Copyright (C) 2012 Motorola, Inc.
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
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/input.h>
#include <linux/m4sensorhub.h>
#include <linux/m4sensorhub/MemMapGyroSensor.h>
#include <linux/m4sensorhub/MemMapAccelSensor.h>
#include <linux/m4sensorhub/MemMapCompassSensor.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#ifdef CONFIG_DEBUG_FS
#define MPU9150_DEBUG 1
#else
#define MPU9150_DEBUG 0
#endif

#define MPU9150_CLIENT_DRIVER_NAME	"m4sensorhub_mpu9150"
#define SENSOR_IRQ_ENABLE 1
#define SENSOR_IRQ_DISABLE 0

struct mpu9150_accel_data {
	int x;
	int y;
	int z;
};
struct mpu9150_gyro_data {
	int rx;
	int ry;
	int rz;
};
struct mpu9150_compass_data {
	int cx;
	int cy;
	int cz;
	int ca;
};
enum mpu9150_sensor {
	TYPE_GYRO,
	TYPE_COMPASS,
	TYPE_ACCEL,

	NUM_TYPES,    /* Leave as last element */
} sensor;

struct mpu9150_client {
	struct m4sensorhub_data *m4sensorhub;
	struct input_dev *input_dev;
	signed short samplerate[NUM_TYPES];
	signed short latest_samplerate[NUM_TYPES];
	struct mpu9150_accel_data accel_data;
	struct mpu9150_gyro_data gyro_data;
	struct mpu9150_compass_data compass_data;
};

struct mpu9150_client *misc_mpu9150_data;
static int mpu9150_irq_enable_disable(struct mpu9150_client *mpu9150_client_data,
					enum mpu9150_sensor type, int flag);

static int mpu9150_client_open(struct inode *inode, struct file *file)
{
	int err = 0;

	err = nonseekable_open(inode, file);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s failed\n", __func__);
		return err;
	}
	file->private_data = misc_mpu9150_data;

	return 0;
}

static int mpu9150_client_close(struct inode *inode, struct file *file)
{
	KDEBUG(M4SH_DEBUG, "mpu9150_client in %s\n", __func__);
	return 0;
}

static void m4_report_mpu9150_inputevent(
		struct mpu9150_client *mpu9150_client_data,
		enum mpu9150_sensor type)
{
	switch (type) {
	case TYPE_GYRO:
		input_report_rel(mpu9150_client_data->input_dev, REL_RX,
			mpu9150_client_data->gyro_data.rx);
		input_report_rel(mpu9150_client_data->input_dev, REL_RY,
			mpu9150_client_data->gyro_data.ry);
		input_report_rel(mpu9150_client_data->input_dev, REL_RZ,
			mpu9150_client_data->gyro_data.rz);
		input_sync(mpu9150_client_data->input_dev);
		break;
	case TYPE_ACCEL:
		input_report_abs(mpu9150_client_data->input_dev, ABS_X,
			mpu9150_client_data->accel_data.x);
		input_report_abs(mpu9150_client_data->input_dev, ABS_Y,
			mpu9150_client_data->accel_data.y);
		input_report_abs(mpu9150_client_data->input_dev, ABS_Z,
			mpu9150_client_data->accel_data.z);
		input_sync(mpu9150_client_data->input_dev);
		break;
	case TYPE_COMPASS:
		input_report_rel(mpu9150_client_data->input_dev, REL_X,
			 mpu9150_client_data->compass_data.cx);
		input_report_rel(mpu9150_client_data->input_dev, REL_Y,
			mpu9150_client_data->compass_data.cy);
		input_report_rel(mpu9150_client_data->input_dev, REL_Z,
			mpu9150_client_data->compass_data.cz);
		/* TODO : accuracy needs to be sent out through sysfs*/
		input_sync(mpu9150_client_data->input_dev);
		break;
	default:
		break;
	}
}

/* TO DO
***** implement the delay functionality when M4 changes will be ready
*/
static void m4_set_mpu9150_delay(struct mpu9150_client *mpu9150_client_data,
			int delay, enum mpu9150_sensor type)
{
	mpu9150_client_data->latest_samplerate[type] = delay;

	if (delay != mpu9150_client_data->samplerate[type]) {
		switch (type) {
		case TYPE_GYRO:
			m4sensorhub_reg_write(mpu9150_client_data->m4sensorhub,
				M4SH_REG_GYRO_SAMPLERATE, (char *)&delay, m4sh_no_mask);
			break;
		case TYPE_ACCEL:
			m4sensorhub_reg_write(mpu9150_client_data->m4sensorhub,
				M4SH_REG_ACCEL_SAMPLERATE, (char *)&delay, m4sh_no_mask);
			break;
		case TYPE_COMPASS:
			m4sensorhub_reg_write(mpu9150_client_data->m4sensorhub,
				M4SH_REG_COMPASS_SAMPLERATE, (char *)&delay, m4sh_no_mask);
			break;
		default:
			return;
			break;
		}
		KDEBUG(M4SH_DEBUG, "%s() updating samplerate for type %d from"
				   " %d to %d\n", __func__, type,
				   mpu9150_client_data->samplerate[type],
				   delay);

		mpu9150_client_data->samplerate[type] = delay;
	}
}

static void m4_read_mpu9150_data(struct mpu9150_client *mpu9150_client_data,
			enum mpu9150_sensor type)
{
	sCompassData compassdata;
	sAccelData acceldata;
	sGyroData gyrodata;

	switch (type) {
	case TYPE_GYRO:
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_GYRO_X, (char *)&gyrodata.x);
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_GYRO_Y, (char *)&gyrodata.y);
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_GYRO_Z, (char *)&gyrodata.z);
		mpu9150_client_data->gyro_data.rx = gyrodata.x;
		mpu9150_client_data->gyro_data.ry = gyrodata.y;
		mpu9150_client_data->gyro_data.rz = gyrodata.z;
		break;
	case TYPE_ACCEL:
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_ACCEL_X, (char *)&acceldata.x);
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_ACCEL_Y, (char *)&acceldata.y);
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_ACCEL_Z, (char *)&acceldata.z);
		mpu9150_client_data->accel_data.x = acceldata.x;
		mpu9150_client_data->accel_data.y = acceldata.y;
		mpu9150_client_data->accel_data.z = acceldata.z;
		break;
	case TYPE_COMPASS:
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_COMPASS_X, (char *)&compassdata.x);
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_COMPASS_Y, (char *)&compassdata.y);
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_COMPASS_Z, (char *)&compassdata.z);
		m4sensorhub_reg_read(mpu9150_client_data->m4sensorhub,
			M4SH_REG_COMPASS_ACCURACY,
			(char *)&compassdata.accuracy);

		mpu9150_client_data->compass_data.cx =  compassdata.x;
		mpu9150_client_data->compass_data.cy =  compassdata.y;
		mpu9150_client_data->compass_data.cz =  compassdata.z;
		mpu9150_client_data->compass_data.ca =  compassdata.accuracy;

		break;

	default:
		break;
	}

}
static void m4_handle_mpu9150_gyro_irq(enum m4sensorhub_irqs int_event,
					 void *mpu9150_data)
{
	struct mpu9150_client *mpu9150_client_data = mpu9150_data;
	m4_read_mpu9150_data(mpu9150_client_data, TYPE_GYRO);
	m4_report_mpu9150_inputevent(mpu9150_client_data, TYPE_GYRO);
}

static void m4_handle_mpu9150_accel_irq(enum m4sensorhub_irqs int_event,
					 void *mpu9150_data)
{
	struct mpu9150_client *mpu9150_client_data = mpu9150_data;

	m4_read_mpu9150_data(mpu9150_client_data, TYPE_ACCEL);
	m4_report_mpu9150_inputevent(mpu9150_client_data, TYPE_ACCEL);
}

static void m4_handle_mpu9150_compass_irq(enum m4sensorhub_irqs int_event,
					void *mpu9150_data)
{
	struct mpu9150_client *mpu9150_client_data = mpu9150_data;

	m4_read_mpu9150_data(mpu9150_client_data, TYPE_COMPASS);
	m4_report_mpu9150_inputevent(mpu9150_client_data, TYPE_COMPASS);
}

static ssize_t m4_mpu9150_write_accel_setdelay(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int scanresult, err;

	err = kstrtoint(buf, 10, &scanresult);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: conversion failed\n", __func__);
		return err;
	}
	/* Input validation  */
	if (scanresult < -1) {
		KDEBUG(
			M4SH_ERROR, "%s() invalid input %d\n",
			__func__ , scanresult
			);
		return -EINVAL;
	}
	m4_set_mpu9150_delay(misc_mpu9150_data, scanresult, TYPE_ACCEL);

	if (scanresult == -1) {
		mpu9150_irq_enable_disable(
					misc_mpu9150_data, TYPE_ACCEL,
					SENSOR_IRQ_DISABLE
					);
	} else {
		mpu9150_irq_enable_disable(
					misc_mpu9150_data, TYPE_ACCEL,
					SENSOR_IRQ_ENABLE
					);
	}

	return count;
}

static DEVICE_ATTR(accel_setdelay, S_IRUSR | S_IWUSR, NULL,
				m4_mpu9150_write_accel_setdelay);

static ssize_t m4_mpu9150_write_gyro_setdelay(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int scanresult, err;

	err = kstrtoint(buf, 10, &scanresult);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: conversion failed\n", __func__);
		return err;
	}
	/* Input validation  */
	if (scanresult < -1) {
		KDEBUG(
			M4SH_ERROR, "%s() invalid input %d\n",
			__func__ , scanresult
			);
		return -EINVAL;
	}
	m4_set_mpu9150_delay(misc_mpu9150_data, scanresult, TYPE_GYRO);
	if (scanresult == -1) {
		mpu9150_irq_enable_disable(
					misc_mpu9150_data, TYPE_GYRO,
					SENSOR_IRQ_DISABLE
					);
	} else {
		mpu9150_irq_enable_disable(
					misc_mpu9150_data, TYPE_GYRO,
					SENSOR_IRQ_ENABLE
					);
	}
	return count;
}

static DEVICE_ATTR(gyro_setdelay, S_IRUSR | S_IWUSR, NULL,
				m4_mpu9150_write_gyro_setdelay);

static ssize_t m4_mpu9150_write_compass_setdelay(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int scanresult, err;

	err = kstrtoint(buf, 10, &scanresult);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: conversion failed\n", __func__);
		return err;
	}
	/* Input validation  */
	if (scanresult < -1) {
		KDEBUG(
			M4SH_ERROR, "%s() invalid input %d\n",
			__func__ , scanresult
			);
		return -EINVAL;
	}
	m4_set_mpu9150_delay(misc_mpu9150_data, scanresult, TYPE_COMPASS);
	if (scanresult == -1) {
		mpu9150_irq_enable_disable(
				misc_mpu9150_data, TYPE_COMPASS,
				SENSOR_IRQ_DISABLE
				);
	} else {
		mpu9150_irq_enable_disable(
				misc_mpu9150_data, TYPE_COMPASS,
				SENSOR_IRQ_ENABLE
			);
	}
	return count;
}
static DEVICE_ATTR(compass_setdelay, S_IRUSR | S_IWUSR, NULL,
				m4_mpu9150_write_compass_setdelay);
static struct attribute *mpu9150_control_attributes[] = {
	&dev_attr_accel_setdelay.attr,
	&dev_attr_gyro_setdelay.attr,
	&dev_attr_compass_setdelay.attr,
	NULL
};


static const struct attribute_group mpu9150_control_group = {
	.attrs = mpu9150_control_attributes,
};
#ifdef MPU9150_DEBUG
static ssize_t m4_mpu9150_x(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : raw x = %d\n",
			__func__, mpu9150_client_data->accel_data.x);
	return sprintf(buf, "%d\n", mpu9150_client_data->accel_data.x);
}

static ssize_t m4_mpu9150_y(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : raw y = %d\n",
			__func__, mpu9150_client_data->accel_data.y);
	return sprintf(buf, "%d\n", mpu9150_client_data->accel_data.y);
}

static ssize_t m4_mpu9150_z(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : raw z = %d\n",
			__func__, mpu9150_client_data->accel_data.z);
	return sprintf(buf, "%d\n", mpu9150_client_data->accel_data.z);
}

static ssize_t m4_mpu9150_cx(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : compass cx = %d\n",
			__func__, mpu9150_client_data->compass_data.cx);
	return sprintf(buf, "%d\n", mpu9150_client_data->compass_data.cx);
}

static ssize_t m4_mpu9150_cy(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : compass cy = %d\n",
			__func__, mpu9150_client_data->compass_data.cy);
	return sprintf(buf, "%d\n", mpu9150_client_data->compass_data.cy);
}

static ssize_t m4_mpu9150_cz(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : compass cz = %d\n",
			__func__, mpu9150_client_data->compass_data.cz);
	return sprintf(buf, "%d\n", mpu9150_client_data->compass_data.cz);
}

static ssize_t m4_mpu9150_ca(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : compass ca = %d\n",
			__func__, mpu9150_client_data->compass_data.ca);
	return sprintf(buf, "%d\n", mpu9150_client_data->compass_data.ca);
}

static ssize_t m4_mpu9150_rx(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : rx = %d\n",
			__func__, mpu9150_client_data->gyro_data.rx);
	return sprintf(buf, "%d\n", mpu9150_client_data->gyro_data.rx);
}
static ssize_t m4_mpu9150_ry(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : ry = %d\n",
			__func__, mpu9150_client_data->gyro_data.ry);
	return sprintf(buf, "%d\n", mpu9150_client_data->gyro_data.ry);
}

static ssize_t m4_mpu9150_rz(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mpu9150_client *mpu9150_client_data = platform_get_drvdata(pdev);

	KDEBUG(M4SH_DEBUG, "%s  : rz = %d\n",
			__func__, mpu9150_client_data->gyro_data.rz);
	return sprintf(buf, "%d\n", mpu9150_client_data->gyro_data.rz);
}


static DEVICE_ATTR(raw_x, 0444, m4_mpu9150_x, NULL);
static DEVICE_ATTR(raw_y, 0444, m4_mpu9150_y, NULL);
static DEVICE_ATTR(raw_z, 0444, m4_mpu9150_z, NULL);
static DEVICE_ATTR(compass_cx, 0444, m4_mpu9150_cx, NULL);
static DEVICE_ATTR(compass_cy, 0444, m4_mpu9150_cy, NULL);
static DEVICE_ATTR(compass_cz, 0444, m4_mpu9150_cz, NULL);
static DEVICE_ATTR(compass_ca, 0444, m4_mpu9150_ca, NULL);
static DEVICE_ATTR(rx, 0444, m4_mpu9150_rx, NULL);
static DEVICE_ATTR(ry, 0444, m4_mpu9150_ry, NULL);
static DEVICE_ATTR(rz, 0444, m4_mpu9150_rz, NULL);

static struct attribute *mpu9150_attributes[] = {
	&dev_attr_raw_x.attr,
	&dev_attr_raw_y.attr,
	&dev_attr_raw_z.attr,
	&dev_attr_compass_cx.attr,
	&dev_attr_compass_cy.attr,
	&dev_attr_compass_cz.attr,
	&dev_attr_compass_ca.attr,
	&dev_attr_rx.attr,
	&dev_attr_ry.attr,
	&dev_attr_rz.attr,
	NULL
};

static const struct attribute_group mpu9150_group = {
	.attrs = mpu9150_attributes,
};
#endif

static const struct file_operations mpu9150_client_fops = {
	.owner = THIS_MODULE,
	.open  = mpu9150_client_open,
	.release = mpu9150_client_close,
};

static struct miscdevice mpu9150_client_miscdrv = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = MPU9150_CLIENT_DRIVER_NAME,
	.fops = &mpu9150_client_fops,
};

static int mpu9150_irq_init(struct mpu9150_client *mpu9150_client_data)
{
	int ret = -1;

	ret = m4sensorhub_irq_register(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_GYRO_DATA_READY,
				m4_handle_mpu9150_gyro_irq,
				mpu9150_client_data, 0);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "Error registering int %d (%d)\n",
					M4SH_IRQ_GYRO_DATA_READY, ret);
		return ret;
	}
	ret = m4sensorhub_irq_register(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_ACCEL_DATA_READY,
				m4_handle_mpu9150_accel_irq,
				mpu9150_client_data, 0);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "Error registering int %d (%d)\n",
					M4SH_IRQ_ACCEL_DATA_READY, ret);
		goto unregister_gyro_irq;
	}
	ret = m4sensorhub_irq_register(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_COMPASS_DATA_READY,
				m4_handle_mpu9150_compass_irq,
				mpu9150_client_data, 0);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "Error registering int %d (%d)\n",
				M4SH_IRQ_COMPASS_DATA_READY, ret);
		goto unregister_accel_irq;
	}
	return ret;

unregister_accel_irq:
	m4sensorhub_irq_unregister(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_ACCEL_DATA_READY);
unregister_gyro_irq:
	m4sensorhub_irq_unregister(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_GYRO_DATA_READY);
	return ret;
}

static void mpu9150_irq_deinit(struct mpu9150_client *mpu9150_client_data)
{
	m4sensorhub_irq_unregister(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_COMPASS_DATA_READY);
	m4sensorhub_irq_unregister(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_ACCEL_DATA_READY);
	m4sensorhub_irq_unregister(mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_GYRO_DATA_READY);
}

static int mpu9150_irq_enable_disable(struct mpu9150_client *mpu9150_client_data,
				enum mpu9150_sensor type, int flag)
{
	int ret = 0;
	int irq_status = 0;

	switch (type) {
	case TYPE_GYRO:
		irq_status = m4sensorhub_irq_enable_get(
				mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_GYRO_DATA_READY);
		if (flag && (!irq_status)) {
			ret = m4sensorhub_irq_enable(
					mpu9150_client_data->m4sensorhub,
					M4SH_IRQ_GYRO_DATA_READY);
			if (ret < 0) {
				KDEBUG(M4SH_ERROR, "Error enabling int %d (%d)\n",
					M4SH_IRQ_GYRO_DATA_READY, ret);
				return ret;
			}
		} else if ((!flag) && irq_status)
			m4sensorhub_irq_disable(
					mpu9150_client_data->m4sensorhub,
					M4SH_IRQ_GYRO_DATA_READY);
		break;
	case TYPE_ACCEL:
		irq_status = m4sensorhub_irq_enable_get(
				mpu9150_client_data->m4sensorhub,
				M4SH_IRQ_ACCEL_DATA_READY);
		if (flag && (!irq_status)) {
			ret = m4sensorhub_irq_enable(
					mpu9150_client_data->m4sensorhub,
					M4SH_IRQ_ACCEL_DATA_READY);
			if (ret < 0) {
				KDEBUG(M4SH_ERROR, "Error enabling int %d (%d)\n",
					M4SH_IRQ_ACCEL_DATA_READY, ret);
				return ret;
			}
		} else if ((!flag) && irq_status)
			m4sensorhub_irq_disable(
					mpu9150_client_data->m4sensorhub,
					M4SH_IRQ_ACCEL_DATA_READY);
		break;
	case TYPE_COMPASS:
		irq_status = m4sensorhub_irq_enable_get(
				mpu9150_client_data->m4sensorhub,
				 M4SH_IRQ_COMPASS_DATA_READY);
		if (flag && (!irq_status)) {
			ret = m4sensorhub_irq_enable(
					mpu9150_client_data->m4sensorhub,
					M4SH_IRQ_COMPASS_DATA_READY);
			if (ret < 0) {
				KDEBUG(M4SH_ERROR, "Error enabling int %d (%d)\n",
					M4SH_IRQ_COMPASS_DATA_READY, ret);
				return ret;
			}
		} else if ((!flag) && irq_status)
			m4sensorhub_irq_disable(
					mpu9150_client_data->m4sensorhub,
					M4SH_IRQ_COMPASS_DATA_READY);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static void mpu9150_panic_restore(struct m4sensorhub_data *m4sensorhub,
				void *data)
{
	struct mpu9150_client *dd = (struct mpu9150_client *)data;

	if (dd == NULL) {
		KDEBUG(M4SH_INFO, "%s: Driver data is null,unable to restore\n",
		       __func__);
		return;
	}
	KDEBUG(M4SH_INFO, "Executing mpu9150 panic restore\n");
	m4_set_mpu9150_delay(dd, dd->samplerate[TYPE_ACCEL], TYPE_ACCEL);
	m4_set_mpu9150_delay(dd, dd->samplerate[TYPE_GYRO], TYPE_GYRO);
	m4_set_mpu9150_delay(dd, dd->samplerate[TYPE_COMPASS], TYPE_COMPASS);
}

static int mpu9150_driver_init(struct init_calldata *p_arg)
{
	int ret;
	ret = mpu9150_irq_init(misc_mpu9150_data);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "mpu9150 irq init failed\n");
		goto driver_init_exit;
	}

	ret = m4sensorhub_panic_register(misc_mpu9150_data->m4sensorhub,
					 PANICHDL_MPU9150_RESTORE,
					 mpu9150_panic_restore,
					 misc_mpu9150_data);
	if (ret < 0)
		KDEBUG(M4SH_ERROR, "HR panic callback register failed\n");

driver_init_exit:
	return ret;
}

static int mpu9150_client_probe(struct platform_device *pdev)
{
	int ret = -1;
	struct mpu9150_client *mpu9150_client_data;
	struct m4sensorhub_data *m4sensorhub = m4sensorhub_client_get_drvdata();

	if (!m4sensorhub)
		return -EFAULT;

	mpu9150_client_data = kzalloc(sizeof(*mpu9150_client_data),
						GFP_KERNEL);
	if (!mpu9150_client_data)
		return -ENOMEM;

	mpu9150_client_data->m4sensorhub = m4sensorhub;
	platform_set_drvdata(pdev, mpu9150_client_data);
	mpu9150_client_data->samplerate[TYPE_ACCEL] = -1;
	mpu9150_client_data->samplerate[TYPE_GYRO] = -1;
	mpu9150_client_data->samplerate[TYPE_COMPASS] = -1;
	mpu9150_client_data->latest_samplerate[TYPE_ACCEL] =
			mpu9150_client_data->samplerate[TYPE_ACCEL];
	mpu9150_client_data->latest_samplerate[TYPE_GYRO] =
			mpu9150_client_data->samplerate[TYPE_GYRO];
	mpu9150_client_data->latest_samplerate[TYPE_COMPASS] =
			mpu9150_client_data->samplerate[TYPE_COMPASS];

	mpu9150_client_data->input_dev = input_allocate_device();
	if (!mpu9150_client_data->input_dev) {
		ret = -ENOMEM;
		KDEBUG(M4SH_ERROR, "%s: input device allocate failed: %d\n",
			__func__, ret);
		goto free_mem;
	}

	mpu9150_client_data->input_dev->name = MPU9150_CLIENT_DRIVER_NAME;
	set_bit(EV_ABS, mpu9150_client_data->input_dev->evbit);
	set_bit(EV_REL, mpu9150_client_data->input_dev->evbit);
	input_set_abs_params(mpu9150_client_data->input_dev,
		ABS_X, -2147483647, 2147483647, 0, 0);
	input_set_abs_params(mpu9150_client_data->input_dev,
		ABS_Y, -2147483647, 2147483647, 0, 0);
	input_set_abs_params(mpu9150_client_data->input_dev,
		ABS_Z, -2147483647, 2147483647, 0, 0);

	set_bit(REL_X, mpu9150_client_data->input_dev->relbit);
	set_bit(REL_Y, mpu9150_client_data->input_dev->relbit);
	set_bit(REL_Z, mpu9150_client_data->input_dev->relbit);
	set_bit(REL_RX, mpu9150_client_data->input_dev->relbit);
	set_bit(REL_RY, mpu9150_client_data->input_dev->relbit);
	set_bit(REL_RZ, mpu9150_client_data->input_dev->relbit);

	if (input_register_device(mpu9150_client_data->input_dev)) {
		KDEBUG(M4SH_ERROR, "%s: input device register failed\n",
			__func__);
		input_free_device(mpu9150_client_data->input_dev);
		goto free_mem;
	}

	ret = misc_register(&mpu9150_client_miscdrv);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "Error registering %s driver\n", __func__);
		goto unregister_input_device;
	}
	misc_mpu9150_data = mpu9150_client_data;
	ret = m4sensorhub_register_initcall(mpu9150_driver_init,
					mpu9150_client_data);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "Unable to register init function"
			"for mpu9150 client = %d\n", ret);
		goto unregister_misc_device;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &mpu9150_control_group);
	if (ret)
		goto unregister_initcall;
#ifdef MPU9150_DEBUG
	ret = sysfs_create_group(&pdev->dev.kobj, &mpu9150_group);
	if (ret)
		goto unregister_control_group;
#endif
	KDEBUG(M4SH_INFO, "Initialized %s driver\n", __func__);
	return 0;

#ifdef MPU9150_DEBUG
unregister_control_group:
	sysfs_remove_group(&pdev->dev.kobj, &mpu9150_control_group);
#endif
unregister_initcall:
	m4sensorhub_unregister_initcall(mpu9150_driver_init);
unregister_misc_device:
	misc_mpu9150_data = NULL;
	misc_deregister(&mpu9150_client_miscdrv);
unregister_input_device:
	input_unregister_device(mpu9150_client_data->input_dev);
free_mem:
	platform_set_drvdata(pdev, NULL);
	mpu9150_client_data->m4sensorhub = NULL;
	kfree(mpu9150_client_data);
	mpu9150_client_data = NULL;
	return ret;
}

static int __exit mpu9150_client_remove(struct platform_device *pdev)
{
	struct mpu9150_client *mpu9150_client_data =
						platform_get_drvdata(pdev);
	sysfs_remove_group(&pdev->dev.kobj, &mpu9150_control_group);
#ifdef MPU9150_DEBUG
	sysfs_remove_group(&pdev->dev.kobj, &mpu9150_group);
#endif
	mpu9150_irq_deinit(mpu9150_client_data);
	m4sensorhub_unregister_initcall(mpu9150_driver_init);
	misc_mpu9150_data = NULL;
	misc_deregister(&mpu9150_client_miscdrv);
	input_unregister_device(mpu9150_client_data->input_dev);
	platform_set_drvdata(pdev, NULL);
	mpu9150_client_data->m4sensorhub = NULL;
	kfree(mpu9150_client_data);
	mpu9150_client_data = NULL;
	return 0;
}

static int mpu9150_client_suspend(struct platform_device *pdev,
				  pm_message_t state)
{
	struct mpu9150_client *dd = platform_get_drvdata(pdev);
	m4_set_mpu9150_delay(dd, dd->latest_samplerate[TYPE_ACCEL], TYPE_ACCEL);
	m4_set_mpu9150_delay(dd, dd->latest_samplerate[TYPE_GYRO], TYPE_GYRO);
	m4_set_mpu9150_delay(dd, dd->latest_samplerate[TYPE_COMPASS],
			     TYPE_COMPASS);
	return 0;
}

static struct of_device_id m4mpu9150_match_tbl[] = {
	{ .compatible = "mot,m4mpu9150" },
	{},
};

static struct platform_driver mpu9150_client_driver = {
	.probe		= mpu9150_client_probe,
	.remove		= __exit_p(mpu9150_client_remove),
	.shutdown	= NULL,
	.suspend	= mpu9150_client_suspend,
	.resume		= NULL,
	.driver		= {
		.name	= MPU9150_CLIENT_DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(m4mpu9150_match_tbl),
	},
};

static int __init mpu9150_client_init(void)
{
	return platform_driver_register(&mpu9150_client_driver);
}

static void __exit mpu9150_client_exit(void)
{
	platform_driver_unregister(&mpu9150_client_driver);
}

module_init(mpu9150_client_init);
module_exit(mpu9150_client_exit);

MODULE_ALIAS("platform:mpu9150_client");
MODULE_DESCRIPTION("M4 Sensor Hub Mpu9150 client driver");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
