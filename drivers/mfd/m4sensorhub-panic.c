/*
 * Copyright (C) 2012 Motorola, Inc.
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

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/m4sensorhub.h>
#include <linux/slab.h>



/* --------------- Global Declarations -------------- */
#define PANIC_BANK              0xFF /* Reserved for Panic bank */
#define PANIC_CMD_CHECK         0xCD /* Panic Handoff command */
#define PANIC_RESP_CHECK        0xDeadBeef /* Panic Handoff Magic code */

/* ------------ Local Function Prototypes ----------- */

/* --------------- Local Declarations -------------- */
static const char *callback_name[PANICHDL_MAX] = {
	[PANICHDL_IRQ_RESTORE] = "irq_restore",
};

struct m4sensorhub_panic_callback {
	void (*callback)(struct m4sensorhub_data *, void *);
	void *data;
};

struct m4sensorhub_panicdata {
	struct mutex lock;  /* lock callback and data */
	struct m4sensorhub_panic_callback funcs[PANICHDL_MAX];
};

union panic_buf {
	struct _in {
		unsigned char bank;
		unsigned char cmd;
	} in;
	unsigned int data;
};

/* -------------- Local Data Structures ------------- */

/* -------------- Global Functions ----------------- */

/* m4sensorhub_panic_init()

   Initialized panic private data structures.

   Returns 0 on success or negative error code on failure

     m4sensorhub - pointer to the main m4sensorhub data struct
*/
int m4sensorhub_panic_init(struct m4sensorhub_data *m4sensorhub)
{
	int retval = 0;
	struct m4sensorhub_panicdata *data;

	data = kzalloc(sizeof(struct m4sensorhub_panicdata), GFP_KERNEL);
	if (data) {
		mutex_init(&data->lock);
		m4sensorhub->panicdata = data;
	} else {
		KDEBUG(M4SH_ERROR, "m4sensorhub: Memory error in panic_init\n");
		retval = -ENOMEM;
	}
	return retval;
}
EXPORT_SYMBOL_GPL(m4sensorhub_panic_init);

/* m4sensorhub_panic_shutdown()

   Shutdown the M4 sensor hub Panic subsystem

     m4sensorhub - pointer to the main m4sensorhub data struct
*/
void m4sensorhub_panic_shutdown(struct m4sensorhub_data *m4sensorhub)
{
	if (m4sensorhub && m4sensorhub->panicdata) {
		struct m4sensorhub_panicdata *data = m4sensorhub->panicdata;
		m4sensorhub->panicdata = NULL;
		if (mutex_is_locked(&data->lock))
			mutex_unlock(&data->lock);
		mutex_destroy(&data->lock);
		kfree(data);
	}
}
EXPORT_SYMBOL_GPL(m4sensorhub_panic_shutdown);

/* m4sensorhub_panic_register()

   Register an panic handler to monitor M4 panic reset

   Returns 0 on success or negative error code on failure

     m4sensorhub - pointer to the main m4sensorhub data struct
     index - M4 Sensor Hub panic handler to resiter for
     cb_func - panic handler function to execute after M4 reset
     data - pointer to data for panic handler function
*/

int m4sensorhub_panic_register(struct m4sensorhub_data *m4sensorhub,
	enum m4sensorhub_panichdl_index index,
	void (*cb_func) (struct m4sensorhub_data *, void *),
	void *data)
{
	struct m4sensorhub_panicdata *panicdata;
	int retval = 0;

	if (!m4sensorhub || (index >= PANICHDL_MAX) || !cb_func)
		return -EINVAL;

	panicdata = (struct m4sensorhub_panicdata *)m4sensorhub->panicdata;
	mutex_lock(&panicdata->lock);
	if (panicdata->funcs[index].callback == NULL) {
		panicdata->funcs[index].callback = cb_func;
		panicdata->funcs[index].data = data;
		KDEBUG(M4SH_NOTICE, "m4sensorhub: %s callback registered\n",
			callback_name[index]);
	} else {
		KDEBUG(M4SH_ERROR, "m4sensorhub: %s callback"\
			" registration failed\n", callback_name[index]);
		retval = -EPERM;
	}
	mutex_unlock(&panicdata->lock);

	return retval;
}
EXPORT_SYMBOL_GPL(m4sensorhub_panic_register);

/* m4sensorhub_panic_unregister()

   Unregister an panic handler to monitor M4 panic reset

   Returns 0 on success or negative error code on failure

     m4sensorhub - pointer to the main m4sensorhub data struct
     index - M4 Sensor Hub panic handler to unresiter for
*/
int m4sensorhub_panic_unregister(struct m4sensorhub_data *m4sensorhub,
	enum m4sensorhub_panichdl_index index)
{
	struct m4sensorhub_panicdata *panicdata;

	if (!m4sensorhub || (index >= PANICHDL_MAX))
		return -EINVAL;

	panicdata = (struct m4sensorhub_panicdata *)m4sensorhub->panicdata;
	mutex_lock(&panicdata->lock);
	panicdata->funcs[index].callback = NULL;
	panicdata->funcs[index].data = NULL;
	mutex_unlock(&panicdata->lock);
	KDEBUG(M4SH_NOTICE, "m4sensorhub: %s callback un-registered\n",
			callback_name[index]);

	return 0;
}
EXPORT_SYMBOL_GPL(m4sensorhub_panic_unregister);


/* m4sensorhub_panic_process()

   Check M4 if it's panicked, use I2C to communicate with M4 panic handler
   OMAP use the same i2c sequences to send command via i2c master, then M4
   i2c slave program will handle these commands, it may have 2 slave programs
   1. Normal i2c slave program handles all vaild banks'(limit on
      M4SH_TYPE__NUM) command, for invalid bank, it always responses 0xFF
   2. Panic i2c slave program handles panic bank(reserved 0xFF for it) command,
      for others, it always responses 0x00

   To detect whether M4 is panicked, the process should be
   i. When OMAP got interrupt from M4, OMAP will check which irq is raised, it
      send normal banks' command to M4, for panic case, it always returns 0x00,
      so OMAP has a checkpoint as there's interrupt request from M4 without
      active IRQ
   ii.Then OMAP will confirm if M4 is panic via send panic bank command, if M4
      is panicked, it will handle this bank and response panic magic code;
      Otherwise, if no panic magic code returned from M4, it always means M4
      isn't panicked.

     m4sensorhub - pointer to the main m4sensorhub data struct
	*/
void m4sensorhub_panic_process(struct m4sensorhub_data *m4sensorhub)
{
	int i, ret;
	union panic_buf buf;
	struct m4sensorhub_panic_callback handler;

	if (!m4sensorhub || !m4sensorhub->panicdata) {
		KDEBUG(M4SH_ERROR, "m4sensorhub: Invalid parameter in %s!\n",\
					__func__);
		return;
	}

	m4sensorhub_reg_access_lock();

	buf.in.bank = PANIC_BANK;
	buf.in.cmd = PANIC_CMD_CHECK;
	ret = m4sensorhub_i2c_write_read(m4sensorhub,\
			(u8 *)&buf, sizeof(buf.in), sizeof(buf.data));
	if ((ret != sizeof(buf.data)) || (buf.data != PANIC_RESP_CHECK)) {
		/* TODO maybe we shall check if M4/OMAP i2c broken */
		KDEBUG(M4SH_ERROR, "m4sensorhub: Unknown IRQ status! "\
				"M4 panic handoff ret=%d, data=0x%x\n",\
				ret, buf.data);
		m4sensorhub_reg_access_unlock();
		return;
	}

	KDEBUG(M4SH_ERROR, "m4sensorhub_panic: Detected M4 panic, reset M4!\n");
	m4sensorhub->pdev->hw_reset(m4sensorhub);
	msleep(100);
	ret = m4sensorhub_load_firmware(m4sensorhub, 0);
	if (ret < 0) {
		KDEBUG(M4SH_ERROR, "m4sensorhub_panic: "\
			"Failed to restart M4, ret = %d\n", ret);
		BUG();
	}

	m4sensorhub_reg_access_unlock();

	for (i = 0; i < PANICHDL_MAX; i++) {
		handler = ((struct m4sensorhub_panicdata *)\
				(m4sensorhub->panicdata))->funcs[i];
		if (handler.callback) {
			KDEBUG(M4SH_NOTICE, "m4sensorhub_panic: "\
				"Calling %s as M4 restarted!\n",\
				callback_name[i]);
			handler.callback(m4sensorhub, handler.data);
		}
	}
}
EXPORT_SYMBOL_GPL(m4sensorhub_panic_process);
