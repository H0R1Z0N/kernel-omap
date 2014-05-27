/*
 * Copyright (C) 2012-2014 Motorola, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/rtc.h>
#include <linux/gpio.h>
#include <linux/string.h>
#include <linux/m4sensorhub/MemMapLog.h>
#include <linux/m4sensorhub.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/firmware.h>


#define M4SENSORHUB_NUM_GPIOS       6

/* --------------- Global Declarations -------------- */
char m4sensorhub_debug;
EXPORT_SYMBOL_GPL(m4sensorhub_debug);

/* ------------ Local Function Prototypes ----------- */

/* -------------- Local Data Structures ------------- */
static struct miscdevice m4sensorhub_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = M4SENSORHUB_DRIVER_NAME,
};

struct init_call {
	int(*initcb)(struct init_calldata *);
	void *pdata;
	struct init_call *next;
};

/* --------------- Local Declarations -------------- */
static struct m4sensorhub_data m4sensorhub_misc_data;
static DEFINE_MUTEX(m4sensorhub_driver_lock);
static struct init_call *inithead;
static int firmware_download_status = -1;
static char tcmd_exec_status;

unsigned short force_upgrade;
module_param(force_upgrade, short, 0644);
MODULE_PARM_DESC(force_upgrade, "Force FW download ignoring version check");

unsigned short debug_level;
module_param(debug_level, short, 0644);
MODULE_PARM_DESC(debug_level, "Set debug level 1 (CRITICAL) to "
				"7 (VERBOSE_DEBUG)");

/* -------------- Global Functions ----------------- */
struct m4sensorhub_data *m4sensorhub_client_get_drvdata(void)
{
	return &m4sensorhub_misc_data;
}
EXPORT_SYMBOL_GPL(m4sensorhub_client_get_drvdata);


/* -------------- Local Functions ----------------- */

static ssize_t m4sensorhub_get_dbg(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", m4sensorhub_debug);
}

/* BEGIN BOARD FILE */
/* TODO: replace with request array */

int m4sensorhub_set_bootmode(struct m4sensorhub_data *m4sensorhub,
			 enum m4sensorhub_bootmode bootmode)
{
	if (m4sensorhub == NULL) {
		pr_err("%s: M4 data is NULL\n", __func__);
		return -EINVAL;
	}

	switch (bootmode) {
	case BOOTMODE00:
		gpio_set_value(m4sensorhub->hwconfig.boot0_gpio, 0);
		gpio_set_value(m4sensorhub->hwconfig.boot1_gpio, 0);
		break;
	case BOOTMODE01:
		gpio_set_value(m4sensorhub->hwconfig.boot0_gpio, 1);
		gpio_set_value(m4sensorhub->hwconfig.boot1_gpio, 0);
		break;
	case BOOTMODE10:
		gpio_set_value(m4sensorhub->hwconfig.boot0_gpio, 0);
		gpio_set_value(m4sensorhub->hwconfig.boot1_gpio, 1);
		break;
	case BOOTMODE11:
		gpio_set_value(m4sensorhub->hwconfig.boot0_gpio, 1);
		gpio_set_value(m4sensorhub->hwconfig.boot1_gpio, 1);
	default:
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(m4sensorhub_set_bootmode);

void m4sensorhub_hw_reset(struct m4sensorhub_data *m4sensorhub)
{
	int err = 0;

	if (m4sensorhub == NULL) {
		pr_err("%s: M4 data is NULL\n", __func__);
		err = -ENODATA;
		goto m4sensorhub_hw_reset_fail;
	} else if (m4sensorhub->i2c_client == NULL) {
		pr_err("%s: I2C client is missing\n", __func__);
		err = -ENODATA;
		goto m4sensorhub_hw_reset_fail;
	}

	if (m4sensorhub->i2c_client->addr == 0x39) {
		err = m4sensorhub_set_bootmode(m4sensorhub, BOOTMODE01);
		if (err < 0) {
			pr_err("%s: Failed to enter bootmode 01\n", __func__);
			goto m4sensorhub_hw_reset_fail;
		}
		usleep_range(5000, 10000);
		gpio_set_value(m4sensorhub->hwconfig.reset_gpio, 0);
		usleep_range(10000, 12000);
		gpio_set_value(m4sensorhub->hwconfig.reset_gpio, 1);
		msleep(400);
	} else {
		err = m4sensorhub_set_bootmode(m4sensorhub, BOOTMODE00);
		if (err < 0) {
			pr_err("%s: Failed to enter bootmode 00\n", __func__);
			goto m4sensorhub_hw_reset_fail;
		}
		gpio_set_value(m4sensorhub->hwconfig.reset_gpio, 1);
		usleep_range(5000, 10000);
		gpio_set_value(m4sensorhub->hwconfig.reset_gpio, 0);
		usleep_range(5000, 10000);
		gpio_set_value(m4sensorhub->hwconfig.reset_gpio, 1);
	}

m4sensorhub_hw_reset_fail:
	if (err < 0)
		pr_err("%s: Failed with error code %d", __func__, err);
}
EXPORT_SYMBOL_GPL(m4sensorhub_hw_reset);


/* callback from driver to initialize hardware on probe */
static int m4sensorhub_hw_init(struct m4sensorhub_data *m4sensorhub,
		struct device_node *node)
{
	int gpio;
	int err = -EINVAL;
	const char *fp = NULL;

	if (m4sensorhub == NULL) {
		pr_err("%s: M4 data is NULL\n", __func__);
		err = -ENODATA;
		goto error;
	} else if (node == NULL) {
		pr_err("%s: Device node is missing\n", __func__);
		err = -ENODATA;
		goto error;
	}

	of_property_read_string(node, "mot,fw-filename", &fp);
	if (fp == NULL) {
		pr_err("%s: Missing M4 sensorhub firmware filename\n",
			__func__);
		err = -EINVAL;
		goto error;
	}
	m4sensorhub->filename = (char *)fp;

	gpio = of_get_named_gpio_flags(node, "mot,irq-gpio", 0, NULL);
	err = (gpio < 0) ? -ENODEV : gpio_request(gpio, "m4sensorhub-intr");
	if (err) {
		pr_err("Failed acquiring M4 Sensor Hub IRQ GPIO-%d (%d)\n",
			gpio, err);
		goto error;
	}
	gpio_direction_input(gpio);
	m4sensorhub->hwconfig.irq_gpio = gpio;

	gpio = of_get_named_gpio_flags(node, "mot,reset-gpio", 0, NULL);
	err = (gpio < 0) ? -ENODEV : gpio_request(gpio, "m4sensorhub-reset");
	if (err) {
		pr_err("Failed acquiring M4 Sensor Hub Reset GPIO-%d (%d)\n",
			gpio, err);
		goto error_reset;
	}
	/* hold M4 reset till M4 load firmware procduce starts
	 * this is needed for snowflake touch determination
	 */
	gpio_direction_output(gpio, 0);
	m4sensorhub->hwconfig.reset_gpio = gpio;

	gpio = of_get_named_gpio_flags(node, "mot,wake-gpio", 0, NULL);
	err = (gpio < 0) ? -ENODEV : gpio_request(gpio, "m4sensorhub-wake");
	if (err) {
		pr_err("Failed acquiring M4 Sensor Hub Wake GPIO-%d (%d)\n",
			gpio, err);
		goto error_wake;
	}
	gpio_direction_output(gpio, 0);
	m4sensorhub->hwconfig.wake_gpio = gpio;

	gpio = of_get_named_gpio_flags(node, "mot,boot0-gpio", 0, NULL);
	err = (gpio < 0) ? -ENODEV : gpio_request(gpio, "m4sensorhub-boot0");
	if (err) {
		pr_err("Failed acquiring M4 Sensor Hub Boot0 GPIO-%d (%d)\n",
			gpio, err);
		goto error_boot0;
	}
	gpio_direction_output(gpio, 0);
	m4sensorhub->hwconfig.boot0_gpio = gpio;

	gpio = of_get_named_gpio_flags(node, "mot,boot1-gpio", 0, NULL);
	err = (gpio < 0) ? -ENODEV : gpio_request(gpio, "m4sensorhub-boot1");
	if (err) {
		pr_err("Failed acquiring M4 Sensor Hub Boot1 GPIO-%d (%d)\n",
			gpio, err);
		goto error_boot1;
	}
	gpio_direction_output(gpio, 0);
	m4sensorhub->hwconfig.boot1_gpio = gpio;

	gpio = of_get_named_gpio_flags(node, "mot,enable-gpio", 0, NULL);
	err = (gpio < 0) ? -ENODEV : gpio_request(gpio, "m4sensorhub-enable");
	if (err) {
		pr_err("Failed acquiring M4 Sensor Hub Enable GPIO-%d (%d)\n",
			gpio, err);
		goto error_enable;
	}
	gpio_direction_output(gpio, 0);
	m4sensorhub->hwconfig.mpu_9150_en_gpio = gpio;

	return 0;

error_enable:
	gpio_free(m4sensorhub->hwconfig.boot1_gpio);
	m4sensorhub->hwconfig.boot1_gpio = -1;
error_boot1:
	gpio_free(m4sensorhub->hwconfig.boot0_gpio);
	m4sensorhub->hwconfig.boot0_gpio = -1;
error_boot0:
	gpio_free(m4sensorhub->hwconfig.wake_gpio);
	m4sensorhub->hwconfig.wake_gpio = -1;
error_wake:
	gpio_free(m4sensorhub->hwconfig.reset_gpio);
	m4sensorhub->hwconfig.reset_gpio = -1;
error_reset:
	gpio_free(m4sensorhub->hwconfig.irq_gpio);
	m4sensorhub->hwconfig.irq_gpio = -1;
error:
	m4sensorhub->filename = NULL;
	return err;
}

/* callback from driver to free hardware on shutdown */
static void m4sensorhub_hw_free(struct m4sensorhub_data *m4sensorhub)
{

	if (m4sensorhub == NULL) {
		pr_err("%s: M4 data is NULL\n", __func__);
		return;
	}

	if (m4sensorhub->hwconfig.irq_gpio >= 0) {
		gpio_free(m4sensorhub->hwconfig.irq_gpio);
		m4sensorhub->hwconfig.irq_gpio = -1;
	}

	if (m4sensorhub->hwconfig.reset_gpio >= 0) {
		gpio_free(m4sensorhub->hwconfig.reset_gpio);
		m4sensorhub->hwconfig.reset_gpio = -1;
	}

	if (m4sensorhub->hwconfig.wake_gpio >= 0) {
		gpio_free(m4sensorhub->hwconfig.wake_gpio);
		m4sensorhub->hwconfig.wake_gpio = -1;
	}

	if (m4sensorhub->hwconfig.boot0_gpio >= 0) {
		gpio_free(m4sensorhub->hwconfig.boot0_gpio);
		m4sensorhub->hwconfig.boot0_gpio = -1;
	}

	if (m4sensorhub->hwconfig.boot1_gpio >= 0) {
		gpio_free(m4sensorhub->hwconfig.boot1_gpio);
		m4sensorhub->hwconfig.boot1_gpio = -1;
	}

	if (m4sensorhub->hwconfig.mpu_9150_en_gpio >= 0) {
		gpio_free(m4sensorhub->hwconfig.mpu_9150_en_gpio);
		m4sensorhub->hwconfig.mpu_9150_en_gpio = -1;
	}

	m4sensorhub->filename = NULL;
}

int m4sensorhub_register_initcall(int(*initfunc)(struct init_calldata *),
								void *pdata)
{
	struct init_call *inc = NULL;

	inc = kzalloc(sizeof(struct init_call), GFP_KERNEL);
	if (inc == NULL) {
		KDEBUG(M4SH_ERROR, "%s: Unable to allocate init call mem\n",
			__func__);
		return -ENOMEM;
	}

	inc->initcb = initfunc;
	inc->pdata = pdata;
	/* add it to the list */
	if (inithead == NULL)
		inc->next = NULL;
	else
		inc->next = inithead;

	inithead = inc;
	return 0;
}
EXPORT_SYMBOL_GPL(m4sensorhub_register_initcall);

void m4sensorhub_unregister_initcall(int(*initfunc)(struct init_calldata *))
{
	struct init_call *node = inithead;
	struct init_call *prev;

	for (node = inithead, prev = NULL;
		node != NULL;
		prev = node, node = node->next) {
		if (node->initcb == initfunc) {
			/* remove this node */
			if (node == inithead)
				inithead = node->next;
			else
				prev->next = node->next;
			kfree(node);
		}
	}
}
EXPORT_SYMBOL_GPL(m4sensorhub_unregister_initcall);
/* END BOARD FILE FUNCTIONS */

/* Downloads m4 firmware and also initializes all m4 drivers */
static void m4sensorhub_initialize(const struct firmware *firmware,
					void *context)
{
	int err = 0;
	struct init_call *inc, *prev;
	struct init_calldata arg;

	if (firmware == NULL) {
		KDEBUG(M4SH_ERROR, "%s: No firmware data recieved\n",
			__func__);
		return;
	}

	/* initiate m4 firmware download */
	KDEBUG(M4SH_CRITICAL, "%s: Starting M4 download %s = %d\n",
		__func__, "with force_upgrade", force_upgrade);
	if (m4sensorhub_misc_data.i2c_client->addr == 0x39)
		firmware_download_status = m4sensorhub_401_load_firmware(
						&m4sensorhub_misc_data,
						force_upgrade,
						firmware);
	else
		firmware_download_status = m4sensorhub_load_firmware(
						&m4sensorhub_misc_data,
						force_upgrade,
						firmware);

	if (firmware_download_status < 0) {
		KDEBUG(M4SH_ERROR, "%s: Failed to load M4 firmware = %d\n",
			__func__, err);
		/* Since firmware download failed, put m4 back into boot mode*/
		m4sensorhub_hw_reset(&m4sensorhub_misc_data);
		return;
	}

	err = m4sensorhub_irq_init(&m4sensorhub_misc_data);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: m4sensorhub irq init failed (err=%d)\n",
			__func__, err);
		return;
	}

	/* Initialize all the m4 drivers */
	inc = inithead;
	arg.p_m4sensorhub_data = &m4sensorhub_misc_data;
	prev = NULL;
	while (inc) {
		arg.p_data = inc->pdata;
		err = inc->initcb(&arg);
		if (err < 0) {
			KDEBUG(M4SH_ERROR,
				"%s: Callback failed with error code %d %s\n",
				__func__, err, "(dumping stack)");
			dump_stack();
		}
		prev = inc;
		inc = inc->next;
		kfree(prev);
	}

	/* Now that all drivers are kicked off, flag this
	as our normal mode of operation */
	m4sensorhub_misc_data.mode = NORMALMODE;
}

static ssize_t m4sensorhub_set_dbg(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long debug;

	if ((strict_strtol(buf, 10, &debug) < 0) ||
	    (debug < M4SH_NODEBUG) || (debug > M4SH_VERBOSE_DEBUG))
		return -EINVAL;

	m4sensorhub_debug = debug;
	KDEBUG(M4SH_CRITICAL, "%s: M4 Sensor Hub debug level = %d\n",
		__func__, m4sensorhub_debug);

	return count;
}

static DEVICE_ATTR(debug_level, S_IRUSR|S_IWUSR, m4sensorhub_get_dbg,
		m4sensorhub_set_dbg);

static ssize_t m4sensorhub_get_loglevel(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long long loglevel;

	m4sensorhub_reg_read(&m4sensorhub_misc_data,
		M4SH_REG_LOG_LOGENABLE, (char *)&loglevel);
	KDEBUG(M4SH_INFO, "M4 loglevel = %llx", loglevel);
	return sprintf(buf, "%llu\n", loglevel);
}
void ParseAndUpdateLogLevels(char *tag, char *level,
			unsigned long long *logLevels)
{
	int i;
	int levelindex = -1;
	int tagindex = -1;
	unsigned long long mask;

	for (i = 0; i < LOG_LEVELS_MAX; i++) {
		if (strcmp(acLogLevels[i], level) == 0) {
			levelindex = i;
			break;
		}
	}

	for (i = 0; i < LOG_MAX; i++) {
		if (strcmp(acLogTags[i], tag) == 0) {
			tagindex = i;
			break;
		}
	}

	if ((tagindex == -1) || (levelindex == -1))
		return;

	/*Clear the revelant bits*/
	mask = 0x03;
	*logLevels &= ~(mask << (tagindex * 2));
	/*set debug level for the relevant bits*/
	*logLevels |= (levelindex << (tagindex * 2));
	KDEBUG(M4SH_INFO, "New M4 log levels = 0x%llx\n", *logLevels);
}

/* Usage: adb shell into the directory of sysinterface log_level and
   echo LOG_ACCEL=LOG_DEGUB,LOG_POWER=LOG_ERROR > log_level */
static ssize_t m4sensorhub_set_loglevel(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long long currentLogLevels;
	char *tag, *level;
	char **logbuf = (char **) &buf;

	m4sensorhub_reg_read(&m4sensorhub_misc_data,
		M4SH_REG_LOG_LOGENABLE, (char *)&currentLogLevels);
	while (1) {
		tag = strsep(logbuf, "=,\n ");
		if (tag == NULL)
			break;
		level = strsep(logbuf, "=,\n ");
		if (level == NULL)
			break;
		ParseAndUpdateLogLevels(tag, level, &currentLogLevels);
	}

	return m4sensorhub_reg_write(&m4sensorhub_misc_data,
		M4SH_REG_LOG_LOGENABLE, (char *)&currentLogLevels,
		m4sh_no_mask);
}

static DEVICE_ATTR(log_level, S_IRUSR|S_IWUSR, m4sensorhub_get_loglevel,
		m4sensorhub_set_loglevel);

static ssize_t m4sensorhub_get_tcmd_response(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (tcmd_exec_status)
		return sprintf(buf, "TCMD execution passed\n");
	else
		return sprintf(buf, "TCMD execution failed\n");
}

static ssize_t m4sensorhub_execute_tcmd(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int opcode, subopcode;
	int ret, tcmd_resp_len, i;
	char tcmd_buf[20];
	tcmd_exec_status = 0;

	sscanf(buf, "0x%x 0x%x 0x%x", &opcode, &subopcode, &tcmd_resp_len);
	tcmd_buf[0] = M4SH_TYPE_TCMD;
	tcmd_buf[1] = (opcode & 0xFF);
	tcmd_buf[2] = (subopcode & 0xFF);
	ret = m4sensorhub_i2c_write_read(&m4sensorhub_misc_data,
			tcmd_buf, 3, tcmd_resp_len);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "m4sensorhub tcmd i2c failed\n");
		return ret;
	}
	if (ret != tcmd_resp_len) {
		KDEBUG(M4SH_ERROR, "m4sensorhub tcmd wrong num bytes read\n");
		return -EBADE;
	}
	for (i = 0; i < tcmd_resp_len; i++)
		KDEBUG(M4SH_INFO, "%#x ", (unsigned char)tcmd_buf[i]);
	KDEBUG(M4SH_INFO, "\n");

	if (tcmd_buf[0] == 0x00)
		tcmd_exec_status = 1;

	return count;
}
static DEVICE_ATTR(tcmd, S_IRUSR|S_IWUSR, m4sensorhub_get_tcmd_response,
		m4sensorhub_execute_tcmd);

static ssize_t m4sensorhub_get_download_status(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n",
		m4sensorhub_misc_data.mode == NORMALMODE ? "1" : "0");
}

static DEVICE_ATTR(download_status, S_IRUGO,
				m4sensorhub_get_download_status, NULL);

static ssize_t m4sensorhub_get_firmware_version(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%#hx\n",
		m4sensorhub_misc_data.mode == NORMALMODE ?
				m4sensorhub_misc_data.fw_version : 0xFFFF);
}

static DEVICE_ATTR(firmware_version, S_IRUGO,
				m4sensorhub_get_firmware_version, NULL);

static ssize_t m4sensorhub_disable_interrupts(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;

	ret = m4sensorhub_irq_disable_all(&m4sensorhub_misc_data);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "%s: Unable to disable all m4 interrupts\n",
			__func__);
		return ret;
	}
	return count;
}
static DEVICE_ATTR(disable_interrupts, S_IWUSR, NULL,
			m4sensorhub_disable_interrupts);

static struct attribute *m4sensorhub_control_attributes[] = {
	&dev_attr_tcmd.attr,
	&dev_attr_log_level.attr,
	&dev_attr_debug_level.attr,
	&dev_attr_firmware_version.attr,
	&dev_attr_download_status.attr,
	&dev_attr_disable_interrupts.attr,
	NULL
};

static const struct attribute_group m4sensorhub_control_group = {
	.attrs = m4sensorhub_control_attributes,
};

static int m4sensorhub_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct m4sensorhub_data *m4sensorhub = &m4sensorhub_misc_data;
	struct device_node *node = client->dev.of_node;
	int err = -EINVAL;

	/* Set debug based on module argument if set, otherwise use
	   default logging rate based on build type */
	if (debug_level)
		m4sensorhub_debug = debug_level;
	else {
#ifdef CONFIG_DEBUG_FS
		/* engineering build */
		m4sensorhub_debug = M4SH_INFO;
#else
		/* user/userdebug builds */
		m4sensorhub_debug = M4SH_ERROR;
#endif
	}

	/* Enabling detailed level M4 logs for all builds*/
	m4sensorhub_debug = M4SH_INFO;
	KDEBUG(M4SH_ERROR, "%s: Initializing M4 Sensor Hub debug=%d\n",
			__func__, m4sensorhub_debug);

	m4sensorhub->mode = UNINITIALIZED;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		KDEBUG(M4SH_ERROR, "%s: client not i2c capable\n", __func__);
		err = -ENODEV;
		goto err_unload;
	}

	/* link m4sensorhub to i2c_client, hw_init uses i2c_client */
	m4sensorhub->i2c_client = client;

	err = m4sensorhub_hw_init(m4sensorhub, node);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: hw_init failed!", __func__);
		goto done;
	}

	/* link i2c_client to m4sensorhub */
	i2c_set_clientdata(client, m4sensorhub);

	err = misc_register(&m4sensorhub_misc_device);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: misc_register failed: %d\n",
			__func__, err);
		goto err_hw_free;
	}

	err = sysfs_create_group(&client->dev.kobj, &m4sensorhub_control_group);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: Failed to create sysfs group\n",
			__func__);
		goto err_deregister;
	}

	if (m4sensorhub->hwconfig.irq_gpio >= 0)
		client->irq = gpio_to_irq(m4sensorhub->hwconfig.irq_gpio);
	else {
		KDEBUG(M4SH_ERROR, "%s: No IRQ configured\n", __func__);
		err = -ENODEV;
		goto err_unregister_control_group;
	}

	err = m4sensorhub_panic_init(m4sensorhub);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: Panic init failed\n", __func__);
		goto err_reg_shutdown;
	}

	err = request_firmware_nowait(THIS_MODULE,
			FW_ACTION_HOTPLUG, m4sensorhub->filename,
			&(m4sensorhub->i2c_client->dev),
			GFP_KERNEL, m4sensorhub,
			m4sensorhub_initialize);
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: request_firmware_nowait failed: %d\n",
			__func__, err);
		goto err_panic_shutdown;
	}
	KDEBUG(M4SH_NOTICE, "Registered M4 Sensor Hub\n");

	goto done;

err_panic_shutdown:
	m4sensorhub_panic_shutdown(m4sensorhub);
err_reg_shutdown:
	m4sensorhub_reg_shutdown(m4sensorhub);
err_unregister_control_group:
	sysfs_remove_group(&client->dev.kobj, &m4sensorhub_control_group);
err_deregister:
	misc_deregister(&m4sensorhub_misc_device);
err_hw_free:
	m4sensorhub->i2c_client = NULL;
	i2c_set_clientdata(client, NULL);
	m4sensorhub_hw_free(m4sensorhub);
	m4sensorhub = NULL;
err_unload:
done:
	if (err < 0) {
		KDEBUG(M4SH_ERROR, "%s: Probe failed with error code %d\n",
			__func__, err);
	}

	return err;
}

static int __exit m4sensorhub_remove(struct i2c_client *client)
{
	struct m4sensorhub_data *m4sensorhub = i2c_get_clientdata(client);
	KDEBUG(M4SH_INFO, "Removing M4 Sensor Hub Driver\n");

	if (m4sensorhub == NULL)
		return 0;

	m4sensorhub_irq_shutdown(m4sensorhub);
	m4sensorhub_panic_shutdown(m4sensorhub);
	m4sensorhub_reg_shutdown(m4sensorhub);
	sysfs_remove_group(&client->dev.kobj, &m4sensorhub_control_group);
	m4sensorhub_hw_reset(m4sensorhub);
	misc_deregister(&m4sensorhub_misc_device);
	m4sensorhub->i2c_client = NULL;
	i2c_set_clientdata(client, NULL);
	m4sensorhub_hw_free(m4sensorhub);
	m4sensorhub = NULL;

	return 0;
}

#ifdef CONFIG_PM
static int m4sensorhub_suspend(struct i2c_client *client, pm_message_t mesg)
{
	KDEBUG(M4SH_INFO, "%s\n", __func__);
	m4sensorhub_irq_pm_dbg_suspend();
	return 0;
}

static int m4sensorhub_resume(struct i2c_client *client)
{

	KDEBUG(M4SH_INFO, "%s\n", __func__);
	m4sensorhub_irq_pm_dbg_resume();
	return 0;
}
#endif /* CONFIG_PM */

static const struct of_device_id of_m4sensorhub_match[] = {
	{ .compatible = "mot,m4sensorhub", },
	{},
};

static const struct i2c_device_id m4sensorhub_id[] = {
	{M4SENSORHUB_DRIVER_NAME, 0},
	{ },
};

MODULE_DEVICE_TABLE(i2c, m4sensorhub_id);

static struct i2c_driver m4sensorhub_driver = {
	.driver = {
		.name = M4SENSORHUB_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_m4sensorhub_match),
	},
	.probe = m4sensorhub_probe,
	.remove = __exit_p(m4sensorhub_remove),
#ifdef CONFIG_PM
	.suspend = m4sensorhub_suspend,
	.resume = m4sensorhub_resume,
#endif /* CONFIG_PM */
	.id_table = m4sensorhub_id,
};

static int __init m4sensorhub_init(void)
{
	return i2c_add_driver(&m4sensorhub_driver);
}

static void __exit m4sensorhub_exit(void)
{
	i2c_del_driver(&m4sensorhub_driver);
	return;
}

module_init(m4sensorhub_init);
module_exit(m4sensorhub_exit);

MODULE_ALIAS("platform:m4sensorhub");
MODULE_DESCRIPTION("M4 Sensor Hub driver");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
