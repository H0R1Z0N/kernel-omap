/*
 * Copyright (c) 2012, Motorola, Inc. All Rights Reserved.
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __M4SENSORHUB_H__
#define __M4SENSORHUB_H__

#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/m4sensorhub/m4sensorhub_registers.h>
#include <linux/m4sensorhub/m4sensorhub_irqs.h>
#include <linux/firmware.h>

#ifdef __KERNEL__

extern char m4sensorhub_debug;

#define M4SENSORHUB_DRIVER_NAME     "m4sensorhub"
#define M4SENSORHUB_I2C_ADDR        0x18

#define KDEBUG(i, format, s...) 			\
	do {						\
		if (m4sensorhub_debug >= i)             \
			printk(KERN_CRIT format, ##s);	\
	} while (0)

#define CHECK_REG_ACCESS_RETVAL(m4sensorhub, retval, reg) \
		((retval == m4sensorhub_reg_getsize(m4sensorhub, reg)) \
		 ? 0 : -EFAULT);

enum m4sensorhub_debug_level {
	M4SH_NODEBUG = 0x0,
	M4SH_CRITICAL,
	M4SH_ERROR,
	M4SH_WARNING,
	M4SH_NOTICE,
	M4SH_INFO,
	M4SH_DEBUG,
	M4SH_VERBOSE_DEBUG
};

enum m4sensorhub_mode {
	UNINITIALIZED,
	BOOTMODE,
	NORMALMODE,
	FACTORYMODE
};

enum m4sensorhub_bootmode {
	BOOTMODE00,
	BOOTMODE01,
	BOOTMODE10,
	BOOTMODE11,
};

/* This enum is used to register M4 panic callback
 * The sequence of this enum is also the sequence of calling
 *   i.e. it will be called follow this enum 0, 1, 2 ... max
*/
enum m4sensorhub_panichdl_index {
	PANICHDL_DISPLAY_RESTORE,
	/* Please add enum before PANICHDL_IRQ_RESTORE
	   to make sure IRQ resotre will be called at last
	 */
	PANICHDL_IRQ_RESTORE, /* Keep it as the last one */
	PANICHDL_MAX = PANICHDL_IRQ_RESTORE+1
};

struct m4sensorhub_data;

struct m4sensorhub_platform_data {
	int (*hw_init)(struct m4sensorhub_data *);
	void (*hw_free)(struct m4sensorhub_data *);
	void (*hw_reset)(struct m4sensorhub_data *);
	int (*set_bootmode)(struct m4sensorhub_data *,
			    enum m4sensorhub_bootmode);
	int (*stillmode_exit)(void);
	int (*set_display_control)(int m4_ctrl, int gpio_mipi_mux);
};

struct m4sensorhub_hwconfig {
	int irq_gpio;
	int reset_gpio;
	int wake_gpio;
	int boot0_gpio;
	int boot1_gpio;
	int mpu_9150_en_gpio;
};

struct m4sensorhub_data {
	struct i2c_client *i2c_client;
	void *irqdata;
	void *panicdata;
	enum m4sensorhub_mode mode;
	struct m4sensorhub_platform_data *pdev;
	struct m4sensorhub_hwconfig hwconfig;
	char *filename;
};

/* Global (kernel) functions */

/* Client devices */
struct m4sensorhub_data *m4sensorhub_client_get_drvdata(void);

/* Register access */

/* m4sensorhub_reg_read()

   Read a register from the M4 sensor hub.

   Returns number of bytes read on success.
   Returns negative error code on failure

     m4sensorhub - pointer to the main m4sensorhub data struct
     reg - Register to be read
     value - array to return data.  Needs to be at least register's size
*/
#define m4sensorhub_reg_read(m4sensorhub, reg, value) \
	m4sensorhub_reg_read_n(m4sensorhub, reg, value, \
			       m4sensorhub_reg_getsize(m4sensorhub, reg))

/* m4sensorhub_reg_write()

   Read a register from the M4 sensor hub.

   Returns number of bytes write on success.
   Returns negative error code on failure

     m4sensorhub - pointer to the main m4sensorhub data struct
     reg - Register to be write
     value - array to return data.  Needs to be at least register's size
     mask - mask representing which bits to change in register.  If all bits
	    are to be changed, then &m4sh_no_mask can be passed here.
*/
#define m4sensorhub_reg_write(m4sensorhub, reg, value, mask) \
	m4sensorhub_reg_write_n(m4sensorhub, reg, value, mask, \
				m4sensorhub_reg_getsize(m4sensorhub, reg))
int m4sensorhub_reg_init(struct m4sensorhub_data *m4sensorhub);
int m4sensorhub_reg_shutdown(struct m4sensorhub_data *m4sensorhub);
int m4sensorhub_reg_read_n(struct m4sensorhub_data *m4sensorhub,
			   enum m4sensorhub_reg reg, unsigned char *value,
			   short num);
int m4sensorhub_reg_write_n(struct m4sensorhub_data *m4sensorhub,
			    enum m4sensorhub_reg reg, unsigned char *value,
			    unsigned char *mask, short num);
int m4sensorhub_reg_write_1byte(struct m4sensorhub_data *m4sensorhub,
				enum m4sensorhub_reg reg, unsigned char value,
				unsigned char mask);
int m4sensorhub_reg_getsize(struct m4sensorhub_data *m4sensorhub,
			    enum m4sensorhub_reg reg);
void m4sensorhub_reg_access_lock(void);
void m4sensorhub_reg_access_unlock(void);
int m4sensorhub_i2c_write_read(struct m4sensorhub_data *m4sensorhub,
				      u8 *buf, int writelen, int readlen);

int m4sensorhub_load_firmware(struct m4sensorhub_data *m4sensorhub,
	unsigned short force_upgrade,
	const struct firmware *firmware);

/* Interrupt handler */
int m4sensorhub_irq_init(struct m4sensorhub_data *m4sensorhub);
void m4sensorhub_irq_shutdown(struct m4sensorhub_data *m4sensorhub);
int m4sensorhub_irq_register(struct m4sensorhub_data *m4sensorhub,
			     enum m4sensorhub_irqs irq,
			     void (*cb_func) (enum m4sensorhub_irqs, void *),
			     void *data);
int m4sensorhub_irq_unregister(struct m4sensorhub_data *m4sensorhub,
			       enum m4sensorhub_irqs irq);
int m4sensorhub_irq_disable(struct m4sensorhub_data *m4sensorhub,
			    enum m4sensorhub_irqs irq);
int m4sensorhub_irq_enable(struct m4sensorhub_data *m4sensorhub,
			   enum m4sensorhub_irqs irq);
int m4sensorhub_irq_enable_get(struct m4sensorhub_data *m4sensorhub,
			       enum m4sensorhub_irqs irq);
void m4sensorhub_irq_pm_dbg_suspend(void);
void m4sensorhub_irq_pm_dbg_resume(void);

int m4sensorhub_panic_init(struct m4sensorhub_data *m4sensorhub);
void m4sensorhub_panic_shutdown(struct m4sensorhub_data *m4sensorhub);
int m4sensorhub_panic_register(struct m4sensorhub_data *m4sensorhub,
			   enum m4sensorhub_panichdl_index index,
			   void (*cb_func)(struct m4sensorhub_data *, void *),
			   void *data);
int m4sensorhub_panic_unregister(struct m4sensorhub_data *m4sensorhub,
				enum m4sensorhub_panichdl_index index);
void m4sensorhub_panic_process(struct m4sensorhub_data *m4sensorhub);
int m4sensorhub_register_initcall(int(*initfunc)(struct m4sensorhub_data *));
void m4sensorhub_unregister_initcall(
		int(*initfunc)(struct m4sensorhub_data *));


#endif /* __KERNEL__ */
#endif  /* __M4SENSORHUB_H__ */

