/* Touch_synaptics.c
 *
 * Copyright (C) 2011 LGE.
 *
 * Author: yehan.ahn@lge.com, hyesung.shin@lge.com
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/input/mt.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/sysdev.h>
#include <linux/platform_device.h>
#include <linux/gpio_event.h>
#include <mach/vreg.h>
#include <mach/rpc_server_handset.h>
#include <mach/board.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/machine.h>
#include <linux/of_gpio.h>
#include <linux/input/touch_synaptics.h>

#include "SynaImage.h"

static struct workqueue_struct *synaptics_wq;

/* RMI4 spec from 511-000405-01 Rev.D
 * Function      Purpose                           See page
 * $01      RMI Device Control                      45
 * $1A      0-D capacitive button sensors           61
 * $05      Image Reporting                         68
 * $07      Image Reporting                         75
 * $08      BIST                                    82
 * $09      BIST                                    87
 * $11      2-D TouchPad sensors                    93
 * $19      0-D capacitive button sensors           141
 * $30      GPIO/LEDs                               148
 * $31      LEDs                                    162
 * $34      Flash Memory Management                 163
 * $36      Auxiliary ADC                           174
 * $54      Test Reporting                          176
 */
#define RMI_DEVICE_CONTROL                          0x01
#define TOUCHPAD_SENSORS                            0x11
#define CAPACITIVE_BUTTON_SENSORS                   0x1A
#define GPIO_LEDS                                   0x30
#define LEDS                                        0x31
#define ANALOG_CONTROL                              0x54
#define TIMER                                       0x32
#define FLASH_MEMORY_MANAGEMENT                     0x34
#define AUXILIARY_ADC                               0x36

/* Register Map & Register bit mask
 * - Please check "One time" this map before using this device driver
 */
/* RMI_DEVICE_CONTROL */
#define MANUFACTURER_ID_REG             (ts->common_fc.dsc.query_base) /* Manufacturer ID */
#define FW_REVISION_REG                 (ts->common_fc.dsc.query_base+3)  /* FW revision */
#define PRODUCT_ID_REG                  (ts->common_fc.dsc.query_base+11) /* Product ID */

#define DEVICE_COMMAND_REG              (ts->common_fc.dsc.command_base)

#define DEVICE_CONTROL_REG              (ts->common_fc.dsc.control_base) /* Device Control */
#define DEVICE_CONTROL_NORMAL_OP        0x00 /* sleep mode : go to doze mode after 500 ms */
#define DEVICE_CONTROL_SLEEP            0x01 /* sleep mode : go to sleep */
#define DEVICE_CONTROL_SPECIFIC         0x02 /* sleep mode : go to doze mode after 5 sec */
#define DEVICE_CONTROL_NOSLEEP          0x04
#define DEVICE_CONTROL_CONFIGURED       0x80

#define INTERRUPT_ENABLE_REG (ts->common_fc.dsc.control_base+1) /* Interrupt Enable 0 */

#define DEVICE_STATUS_REG               (ts->common_fc.dsc.data_base)			/* Device Status */
#define DEVICE_FAILURE_MASK             0x03
#define DEVICE_CRC_ERROR_MASK           0x04
#define DEVICE_STATUS_FLASH_PROG        0x40
#define DEVICE_STATUS_UNCONFIGURED      0x80

#define INTERRUPT_STATUS_REG            (ts->common_fc.dsc.data_base+1)        /* Interrupt Status */
#define INTERRUPT_MASK_FLASH            0x01
#define INTERRUPT_MASK_ABS0             0x04
#define INTERRUPT_MASK_BUTTON           0x10

/* TOUCHPAD_SENSORS */
#define FINGER_COMMAND_REG              (ts->finger_fc.dsc.command_base)

#define FINGER_STATE_REG                (ts->finger_fc.dsc.data_base)            /* Finger State */
#define FINGER_DATA_REG_START           (ts->finger_fc.dsc.data_base+3)        /* Finger Data Register */
#define FINGER_STATE_MASK                0x03
#define REG_X_POSITION                   0
#define REG_Y_POSITION                   1
#define REG_YX_POSITION                  2
#define REG_WY_WX                        3
#define REG_Z                            4
#define TWO_D_EXTEND_STATUS              (ts->finger_fc.dsc.data_base+53)

#define TWO_D_REPORTING_MODE             (ts->finger_fc.dsc.control_base+0)        /* 2D Reporting Mode */
#define REPORT_MODE_CONTINUOUS           0x00
#define REPORT_MODE_REDUCED              0x01
#define ABS_FILTER                       0x08
#define PALM_DETECT_REG                  (ts->finger_fc.dsc.control_base+1)        /* Palm Detect */
#define DELTA_X_THRESH_REG               (ts->finger_fc.dsc.control_base+2)        /* Delta-X Thresh */
#define DELTA_Y_THRESH_REG               (ts->finger_fc.dsc.control_base+3)        /* Delta-Y Thresh */
#define SENSOR_MAX_X_POS                 (ts->finger_fc.dsc.control_base+6)        /* SensorMaxXPos */
#define SENSOR_MAX_Y_POS                 (ts->finger_fc.dsc.control_base+8)        /* SensorMaxYPos */

/* CAPACITIVE_BUTTON_SENSORS */
#define BUTTON_COMMAND_REG               (ts->button_fc.dsc.command_base)
#define BUTTON_DATA_REG                  (ts->button_fc.dsc.data_base) /* Button Data */
#define MAX_NUM_OF_BUTTON                4

/* ANALOG_CONTROL */
#define ANALOG_COMMAND_REG               (ts->analog_fc.dsc.command_base)
#define FORCE_UPDATE                     0x04

#define ANALOG_CONTROL_REG               (ts->analog_fc.dsc.control_base)
#define FORCE_FAST_RELAXATION            0x04

#define FAST_RELAXATION_RATE             (ts->analog_fc.dsc.control_base+16)

/* FLASH_MEMORY_MANAGEMENT */
#define FLASH_CONFIG_ID_REG              (ts->flash_fc.dsc.control_base) /* Flash Control */
#define FLASH_CONTROL_REG                (ts->flash_fc.dsc.data_base+18)
#define FLASH_STATUS_MASK                0xF0

/* Page number */
#define COMMON_PAGE                      (ts->common_fc.function_page)
#define FINGER_PAGE                      (ts->finger_fc.function_page)
#define BUTTON_PAGE                      (ts->button_fc.function_page)
#define ANALOG_PAGE                      (ts->analog_fc.function_page)
#define FLASH_PAGE                       (ts->flash_fc.function_page)
#define DEFAULT_PAGE                     0x00

/* Get user-finger-data from register.
 */
#define TS_SNTS_GET_X_POSITION(_high_reg, _low_reg) \
	(((u16)(((_high_reg) << 4) & 0x0FF0) | (u16)((_low_reg) & 0x0F)))
#define TS_SNTS_GET_Y_POSITION(_high_reg, _low_reg) \
	(((u16)(((_high_reg) << 4) & 0x0FF0) | (u16)(((_low_reg) >> 4) & 0x0F)))
#define TS_POS(_high, _low) \
	(((u16)(((_high) << 8) & 0xFF00) | (u16)((_low ) & 0xFF)))

/* GET_BIT_MASK & GET_INDEX_FROM_MASK
 *
 * For easily checking the user input.
 * Usually, User use only one or two fingers.
 * However, we should always check all finger-status-register
 * because we can't know the total number of fingers.
 * These Macro will prevent it.
 */
#define GET_BIT_MASK(_f_status)	\
	(((_f_status[2] & 0x04)<<7) | ((_f_status[2] & 0x01)<<8) | \
	((_f_status[1] & 0x40)<<1) | ((_f_status[1] & 0x10)<<2) | \
	((_f_status[1] & 0x04)<<3) | ((_f_status[1] & 0x01)<<4) | \
	((_f_status[0] & 0x40)>>3) | ((_f_status[0] & 0x10)>>2) | \
	((_f_status[0] & 0x04)>>1) | (_f_status[0] & 0x01))

#define IS_FIRST_PRESS(__p) (__p.curr_data[id].status == FINGER_PRESSED && \
			__p.prev_data[id].status == FINGER_RELEASED)
#define IS_RELEASE(__p) (__p.curr_data[id].status == FINGER_RELEASED && \
			__p.prev_data[id].status == FINGER_PRESSED)

#define BOOTING_DELAY                   400
#define RESET_DELAY                     20
#define I2C_RETRY_CNT                   10
#define MAX_RETRY_COUNT                 3

#define SYNAPTICS_COORDS_ARR_SIZE       4

#define FW_OFFSET_PRODUCT_ID            0x40
#define FW_OFFSET_IMAGE_VERSION         0xB100

#if defined(CONFIG_HAS_EARLYSUSPEND)
static void touch_early_suspend(struct early_suspend *h);
static void touch_late_resume(struct early_suspend *h);
#endif

int synaptics_t1320_power_on(struct i2c_client *client, int on)
{
	int rc = 0;
	static struct regulator *vreg_l22;
	static struct regulator *vreg_lvs3;

	/* 3.3V_TOUCH_VDD, VREG_L15: 2.75 ~ 3.3 */
	if (!vreg_l22) {
		vreg_l22 = regulator_get(&client->dev, "vdd_ana");
		if (IS_ERR(vreg_l22)) {
			pr_err("%s: regulator get of pm8941_l22 failed (%ld)\n",
					__func__, PTR_ERR(vreg_l22));
			rc = PTR_ERR(vreg_l22);
			vreg_l22 = NULL;
			return rc;
		}
	}

	/* 1.8V_TOUCH_IO, VREG_L22: 1.7 ~ 2.85 */
	if (!vreg_lvs3) {
		vreg_lvs3 = regulator_get(&client->dev, "vcc_i2c");
		if (IS_ERR(vreg_lvs3)) {
			pr_err("%s: regulator get of pm8941_lvs3 failed (%ld)\n",
					__func__, PTR_ERR(vreg_lvs3));
			rc = PTR_ERR(vreg_lvs3);
			vreg_lvs3 = NULL;
			return rc;
		}
	}

        rc = regulator_set_voltage(vreg_l22, 3000000, 3000000);
	if (rc < 0) {
		TOUCH_ERR_MSG("%s: cannot control regulator:%d\n", __func__, rc);
		return rc;
	}

	if (on) {
		TOUCH_INFO_MSG("touch enable\n");
		regulator_enable(vreg_l22);
		regulator_enable(vreg_lvs3);
	} else {
		TOUCH_INFO_MSG("touch disable\n");
		regulator_disable(vreg_l22);
		regulator_disable(vreg_lvs3);
	}

	return rc;
}

/* Debug mask value
 * usage: echo [debug_mask] > /sys/module/touch_synaptics/parameters/debug_mask
 */
u32 touch_debug_mask = DEBUG_BASE_INFO;
module_param_named(debug_mask, touch_debug_mask, int, S_IRUGO|S_IWUSR|S_IWGRP);

static void check_log_finger_changed(struct synaptics_ts_data *ts, u8 total_num);
irqreturn_t touch_irq_handler(int irq, void *dev_id);
int touch_power_cntl(struct synaptics_ts_data *ts, int onoff);
int touch_ic_init(struct synaptics_ts_data *ts);
void touch_init_func(struct work_struct *work_init);
static void safety_reset(struct synaptics_ts_data *ts);
static void release_all_ts_event(struct synaptics_ts_data *ts);
int synaptics_ts_get_data(struct i2c_client *client, struct touch_data* data);
int synaptics_ts_power(struct i2c_client *client, int power_ctrl);
int synaptics_init_panel(struct i2c_client *client, struct synaptics_ts_fw_info *fw_info);
int get_ic_info(struct synaptics_ts_data *ts, struct synaptics_ts_fw_info *fw_info);

/* touch_asb_input_report
 *
 * finger status report
 */
int touch_asb_input_report(struct synaptics_ts_data *ts, int status)
{
	u16 id = 0;
	u8 total = 0;

	if (status == FINGER_PRESSED) {
		for (id = 0; id < ts->pdata->max_id; id++) {
			if (ts->ts_data.curr_data[id].status == FINGER_PRESSED) {
				input_report_abs(ts->input_dev,
					ABS_MT_POSITION_X,
					ts->ts_data.curr_data[id].x_position);

				input_report_abs(ts->input_dev,
					ABS_MT_POSITION_Y,
					ts->ts_data.curr_data[id].y_position);

				TOUCH_DEBUG_MSG("finger pressed report: <%d> x[%4d] y[%4d]\n",
					id,
					ts->ts_data.curr_data[id].x_position,
					ts->ts_data.curr_data[id].y_position);

				total++;

				if (unlikely(touch_debug_mask & DEBUG_ABS))
					TOUCH_DEBUG_MSG("pos[%4d,%4d]\n",
						ts->ts_data.curr_data[id].x_position,
						ts->ts_data.curr_data[id].y_position);

				input_mt_sync(ts->input_dev);
			}
		}
		input_sync(ts->input_dev);

	} else if (status == FINGER_RELEASED) {
		for (id = 0; id < ts->pdata->max_id; id++) {
			if (ts->ts_data.prev_data[id].status == FINGER_PRESSED) {
				input_mt_sync(ts->input_dev);
				TOUCH_DEBUG_MSG("finger released[%d]\n", id);
			}
		}
		input_sync(ts->input_dev);
	}

	return total;
}

/* touch_work_pre_proc
 *
 * Pre-process work at touch_work
 */
int touch_work_pre_proc(struct synaptics_ts_data *ts)
{
	atomic_dec(&ts->next_work);
	ts->ts_data.total_num = 0;
	ts->int_pin_state = 0;

	if (unlikely(ts->work_sync_err_cnt >= MAX_RETRY_COUNT)) {
		TOUCH_ERR_MSG("Work Sync Failed: Irq-pin has some unknown problems\n");
		return -EIO;
	}

	if (unlikely(touch_debug_mask & DEBUG_TRACE))
		TOUCH_DEBUG_MSG("\n");

	if (synaptics_ts_get_data(ts->client, &ts->ts_data) < 0) {
		TOUCH_ERR_MSG("get data fail\n");
		return -EIO;
	}

	ts->int_pin_state = gpio_get_value(ts->pdata->irq_gpio);

	TOUCH_DEBUG_MSG("[Touch] touch_work_pre_proc int_pin %d", ts->int_pin_state);

	return 0;
}

/* touch_work_post_proc
 *
 * Post-process work at touch_work
 */
void touch_work_post_proc(struct synaptics_ts_data *ts, int post_proc)
{
	int next_work = 0;

	if (post_proc >= WORK_POST_MAX)
		return;

	switch (post_proc) {
	case WORK_POST_OUT:
		next_work = atomic_read(&ts->next_work);

		if (unlikely(ts->int_pin_state != 1 && next_work <= 0)) {
			TOUCH_ERR_MSG("WARN: Interrupt pin is low - next_work: %d, try_count: %d]\n", next_work, ts->work_sync_err_cnt);
			post_proc = WORK_POST_ERR_RETRY;
			break;
		}

		ts->work_sync_err_cnt = 0;
		post_proc = WORK_POST_COMPLATE;
		break;

	case WORK_POST_ERR_RETRY:
		ts->work_sync_err_cnt++;
		atomic_inc(&ts->next_work);
		queue_work(synaptics_wq, &ts->work);
		post_proc = WORK_POST_COMPLATE;
		break;

	case WORK_POST_ERR_CIRTICAL:
		ts->work_sync_err_cnt = 0;
		safety_reset(ts);
		touch_ic_init(ts);
		post_proc = WORK_POST_COMPLATE;
		break;

	default:
		post_proc = WORK_POST_COMPLATE;
		break;
	}

	if (post_proc != WORK_POST_COMPLATE)
		touch_work_post_proc(ts, post_proc);
}

static void synaptics_ts_work_func(struct work_struct *work)
{
	struct synaptics_ts_data *ts =
			container_of(work, struct synaptics_ts_data, work);
	u8 report_enable = 0;
	int ret = 0;

	ret = touch_work_pre_proc(ts);
	if (ret == -EIO)
		goto err_out_critical;
	else if (ret == -EAGAIN)
		goto out;

	if (!ts->ts_data.total_num) {
		touch_asb_input_report(ts, FINGER_RELEASED);
		report_enable = 1;
		ts->ts_data.prev_total_num = 0;
	} else if (ts->ts_data.total_num <= ts->pdata->max_id) {
		if (likely(touch_debug_mask & (DEBUG_BASE_INFO | DEBUG_ABS)))
			check_log_finger_changed(ts, ts->ts_data.total_num);

		ts->ts_data.prev_total_num = ts->ts_data.total_num;

		touch_asb_input_report(ts, FINGER_PRESSED);
		report_enable = 1;

		memcpy(ts->ts_data.prev_data, ts->ts_data.curr_data,
				sizeof(ts->ts_data.curr_data));
	}

	/* Reset finger position data */
	memset(&ts->ts_data.curr_data, 0x0, sizeof(ts->ts_data.curr_data));

	if (report_enable)
		input_sync(ts->input_dev);

out:
	touch_work_post_proc(ts, WORK_POST_OUT);
	return;

err_out_critical:
	touch_work_post_proc(ts, WORK_POST_ERR_CIRTICAL);
	return;
}

/* touch_power_cntl
 *
 * 1. POWER_ON
 * 2. POWER_OFF
 * 3. POWER_SLEEP
 * 4. POWER_WAKE
 */
int touch_power_cntl(struct synaptics_ts_data *ts, int onoff)
{
	int ret = 0;

	switch (onoff) {
	case POWER_ON:
		ret = synaptics_ts_power(ts->client, POWER_ON);
		if (ret < 0)
			TOUCH_ERR_MSG("power on failed\n");
		else
			ts->curr_pwr_state = POWER_ON;

		break;
	case POWER_OFF:
		ret = synaptics_ts_power(ts->client, POWER_OFF);
		if (ret < 0)
			TOUCH_ERR_MSG("power off failed\n");
		else
			ts->curr_pwr_state = POWER_OFF;

		msleep(RESET_DELAY);

		atomic_set(&ts->device_init, 0);
		break;
	case POWER_SLEEP:
		ret = synaptics_ts_power(ts->client, POWER_SLEEP);
		if (ret < 0)
			TOUCH_ERR_MSG("power sleep failed\n");
		else
			ts->curr_pwr_state = POWER_SLEEP;
		break;
	case POWER_WAKE:
		ret = synaptics_ts_power(ts->client, POWER_WAKE);
		if (ret < 0)
			TOUCH_ERR_MSG("power wake failed\n");
		else
			ts->curr_pwr_state = POWER_WAKE;
		break;
	default:
		break;
	}

	if (unlikely(touch_debug_mask & DEBUG_POWER))
		if (ret >= 0)
			TOUCH_INFO_MSG("%s: power_state[%d]",
					__FUNCTION__, ts->curr_pwr_state);

	return ret;
}

int synaptics_ts_fw_upgrade(struct i2c_client *client,
				struct synaptics_ts_fw_info *fw_info)
{
	struct synaptics_ts_data *ts =
			(struct synaptics_ts_data *)get_touch_handle(client);
	int ret = 0;

	ts->is_probed = 0;

	ret = FirmwareUpgrade(ts, fw_info->fw_upgrade.fw_path);

	/* update IC info */
	if (ret >= 0)
		get_ic_info(ts, fw_info);

	return ret;
}


/* touch_fw_upgrade_func
 *
 * it used to upgrade the firmware of touch IC.
 */
static void touch_fw_upgrade_func(struct work_struct *work_fw_upgrade)
{
	struct synaptics_ts_data *ts =
			container_of(work_fw_upgrade,
				struct synaptics_ts_data, work_fw_upgrade);
	u8	saved_state = ts->curr_pwr_state;
	int	ver, img_ver;

	if (unlikely(touch_debug_mask & DEBUG_TRACE))
		TOUCH_DEBUG_MSG("\n");

	if (!ts->fw_info.fw_upgrade.fw_force_upgrade && 
		(!ts->fw_info.fw_start ||
			kstrtoint(&ts->fw_info.config_id[1], 10, &ver) != 0 ||
			kstrtoint(&ts->fw_info.image_config_id[1], 10,
								&img_ver) != 0||
			ver >= img_ver)) {
		/* No Upgrade */
		TOUCH_INFO_MSG("FW-upgrade is not executed\n");
		goto out;
	}

	ts->fw_info.fw_upgrade.is_downloading = UNDER_DOWNLOADING;

	if (ts->curr_pwr_state == POWER_ON) {
		disable_irq(ts->client->irq);
	}
	else {
		touch_power_cntl(ts, POWER_ON);
		msleep(BOOTING_DELAY);
	}

	if (likely(touch_debug_mask & (DEBUG_FW_UPGRADE | DEBUG_BASE_INFO)))
		TOUCH_INFO_MSG("F/W upgrade - Start\n");

	if (synaptics_ts_fw_upgrade(ts->client, &ts->fw_info) < 0) {
		TOUCH_ERR_MSG("Firmware upgrade was failed\n");
		if (ts->curr_resume_state)
			enable_irq(ts->client->irq);
		goto err_out;
	}

	if (!ts->curr_resume_state) {
		touch_power_cntl(ts, POWER_OFF);
	}
	else {
		enable_irq(ts->client->irq);

		touch_ic_init(ts);

		if (saved_state == POWER_WAKE || saved_state == POWER_SLEEP)
			touch_power_cntl(ts, saved_state);
	}

	if (likely(touch_debug_mask & (DEBUG_FW_UPGRADE | DEBUG_BASE_INFO)))
		TOUCH_INFO_MSG("F/W upgrade - Finish\n");

	goto out;

err_out:
	safety_reset(ts);
	touch_ic_init(ts);

out:
	memset(&ts->fw_info.fw_upgrade, 0, sizeof(ts->fw_info.fw_upgrade));

	return;
}

/* touch_init_func
 *
 * In order to reduce the booting-time,
 * we used delayed_work_queue instead of msleep or mdelay.
 */
void touch_init_func(struct work_struct *work_init)
{
	struct synaptics_ts_data *ts =
			container_of(to_delayed_work(work_init),
					struct synaptics_ts_data, work_init);

	if (unlikely(touch_debug_mask & DEBUG_TRACE))
		TOUCH_DEBUG_MSG("\n");

	/* Specific device initialization */
	touch_ic_init(ts);
}

/* touch_ic_init
 *
 * initialize the device_IC and variables.
 */
int touch_ic_init(struct synaptics_ts_data *ts)
{
	int next_work = 0;
	ts->int_pin_state = 0;

	if (unlikely(ts->ic_init_err_cnt >= MAX_RETRY_COUNT)) {
		TOUCH_ERR_MSG("Init Failed: Irq-pin has some unknown problems\n");
		goto err_out_critical;
	}

	atomic_set(&ts->next_work, 0);
	atomic_set(&ts->device_init, 1);
	if (synaptics_init_panel(ts->client, &ts->fw_info) < 0) {
		TOUCH_ERR_MSG("specific device initialization fail\n");
		goto err_out_retry;
	}

	/* Interrupt pin check after IC init - avoid Touch lockup */
	msleep(100);
	ts->int_pin_state = gpio_get_value(ts->pdata->irq_gpio);

	TOUCH_DEBUG_MSG("[Touch] touch_ic_init int_pin %d", ts->int_pin_state);

	next_work = atomic_read(&ts->next_work);

	if (unlikely(ts->int_pin_state != 1 && next_work <= 0)) {
		TOUCH_ERR_MSG("WARN: Interrupt pin is low - next_work: %d, try_count: %d]\n",
			next_work, ts->ic_init_err_cnt);
		goto err_out_retry;
	}

	memset(&ts->ts_data, 0, sizeof(ts->ts_data));
	memset(&ts->fw_info.fw_upgrade, 0, sizeof(ts->fw_info.fw_upgrade));
	ts->ic_init_err_cnt = 0;
	return 0;

err_out_retry:
	ts->ic_init_err_cnt++;
	safety_reset(ts);
	queue_delayed_work(synaptics_wq, &ts->work_init, msecs_to_jiffies(10));

	return 0;

err_out_critical:
	ts->ic_init_err_cnt = 0;

	return -1;
}

/* safety_reset
 *
 * 1. disable irq/timer.
 * 2. turn off the power.
 * 3. turn on the power.
 * 4. sleep (booting_delay)ms, usually 400ms(synaptics).
 * 5. enable irq/timer.
 *
 * After 'safety_reset', we should call 'touch_init'.
 */
static void safety_reset(struct synaptics_ts_data *ts)
{
	TOUCH_INFO_MSG(">>>safety_reset\n");
	disable_irq(ts->client->irq);

	release_all_ts_event(ts);

	touch_power_cntl(ts, POWER_OFF);
	touch_power_cntl(ts, POWER_ON);
	msleep(BOOTING_DELAY);

	enable_irq(ts->client->irq);

	TOUCH_INFO_MSG("<<safety_reset\n");
	return;
}

/* release_all_ts_event
 *
 * When system enters suspend-state,
 * if user press touch-panel, release them automatically.
 */
static void release_all_ts_event(struct synaptics_ts_data *ts)
{
	ts->ts_data.prev_total_num = 0;

	input_sync(ts->input_dev);
}

/* check_log_finger_changed
 *
 * Check finger state change for Debug
 */
static void check_log_finger_changed(struct synaptics_ts_data *ts, u8 total_num)
{
	u16 tmp_p = 0;
	u16 tmp_r = 0;
	u16 id = 0;

	if (ts->ts_data.prev_total_num != total_num) {
		/* Finger added */
		if (ts->ts_data.prev_total_num <= total_num) {
			for (id = 0; id < ts->pdata->max_id; id++) {
				if (IS_FIRST_PRESS(ts->ts_data))
					break;
			}
			TOUCH_DEBUG_MSG("%d finger pressed\n", total_num);
		} else {
		/* Finger subtracted */
			TOUCH_DEBUG_MSG("%d finger pressed\n", total_num);
		}
	}
	if (ts->ts_data.prev_total_num == total_num && total_num == 1) {
		/* Finger changed at one finger status - IC bug check */
		for (id = 0, tmp_p = 0; id < ts->pdata->max_id; id++) {
			/* find pressed */
			if (IS_FIRST_PRESS(ts->ts_data))
				tmp_p = id;

			/* find released */
			if (IS_RELEASE(ts->ts_data))
				tmp_r = id;
		}

		if (tmp_p != tmp_r && (ts->ts_data.curr_data[tmp_p].status
				!= ts->ts_data.prev_data[tmp_p].status)) {
			TOUCH_DEBUG_MSG("%d finger changed : <%d -> %d> x[%4d] y[%4d]\n",
					total_num, tmp_r, tmp_p,
					ts->ts_data.curr_data[id].x_position,
					ts->ts_data.curr_data[id].y_position);
		}
	}
}

/* touch_irq_handler
 */
irqreturn_t touch_irq_handler(int irq, void *dev_id)
{
	struct synaptics_ts_data *ts = (struct synaptics_ts_data *)dev_id;

	if (unlikely(atomic_read(&ts->device_init) != 1))
		return IRQ_HANDLED;

	atomic_inc(&ts->next_work);

	queue_work(synaptics_wq, &ts->work);

	return IRQ_HANDLED;
}

/* set_touch_handle / get_touch_handle
 *
 * Developer can save their object using 'set_touch_handle'.
 * Also, they can restore that using 'get_touch_handle'.
 */
void set_touch_handle(struct i2c_client *client, void* h_touch)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	ts->h_touch = h_touch;
}

void *get_touch_handle(struct i2c_client *client)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	return ts->h_touch;
}

/* touch_i2c_read / touch_i2c_write
 *
 * Developer can use these fuctions to communicate with touch_device through I2C.
 */
int touch_i2c_read(struct i2c_client *client, u8 reg, int len, u8 *buf)
{
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};

	if (i2c_transfer(client->adapter, msgs, 2) < 0) {
		if (printk_ratelimit())
			TOUCH_ERR_MSG("transfer error\n");
		return -EIO;
	}

	return 0;
}

int touch_i2c_write(struct i2c_client *client, u8 reg, int len, u8 * buf)
{
	unsigned char send_buf[len + 1];
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = client->flags,
			.len = len+1,
			.buf = send_buf,
		},
	};

	send_buf[0] = (unsigned char)reg;
	memcpy(&send_buf[1], buf, len);

	if (i2c_transfer(client->adapter, msgs, 1) < 0) {
		if (printk_ratelimit())
			TOUCH_ERR_MSG("transfer error\n");
		return -EIO;
	}

	return 0;
}

int touch_i2c_write_byte(struct i2c_client *client, u8 reg, u8 data)
{
	unsigned char send_buf[2];
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = client->flags,
			.len = 2,
			.buf = send_buf,
		},
	};

	send_buf[0] = (unsigned char)reg;
	send_buf[1] = (unsigned char)data;

	if (i2c_transfer(client->adapter, msgs, 1) < 0) {
		if (printk_ratelimit())
			TOUCH_ERR_MSG("transfer error\n");
		return -EIO;
	}

	return 0;
}

/* wrapper function for i2c communication - except defalut page
 * if you have to select page for reading or writing, then using this wrapper function */
int synaptics_ts_page_data_read(struct i2c_client *client, u8 page, u8 reg, int size, u8 *data)
{
	if (unlikely(touch_i2c_write_byte(client, PAGE_SELECT_REG, page) < 0)) {
		TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_read(client, reg, size, data) < 0)) {
		TOUCH_ERR_MSG("[%dP:%d]register read fail\n", page, reg);
		return -EIO;
	}

	if (unlikely(touch_i2c_write_byte(client, PAGE_SELECT_REG, DEFAULT_PAGE) < 0)) {
		TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
		return -EIO;
	}

	return 0;
}

int synaptics_ts_page_data_write(struct i2c_client *client, u8 page, u8 reg, int size, u8 *data)
{
	if (unlikely(touch_i2c_write_byte(client, PAGE_SELECT_REG, page) < 0)) {
		TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_write(client, reg, size, data) < 0)) {
		TOUCH_ERR_MSG("[%dP:%d]register read fail\n", page, reg);
		return -EIO;
	}

	if (unlikely(touch_i2c_write_byte(client, PAGE_SELECT_REG, DEFAULT_PAGE) < 0)) {
		TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
		return -EIO;
	}

	return 0;
}

int synaptics_ts_page_data_write_byte(struct i2c_client *client, u8 page, u8 reg, u8 data)
{
	if (unlikely(touch_i2c_write_byte(client, PAGE_SELECT_REG, page) < 0)) {
		TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_write_byte(client, reg, data) < 0)) {
		TOUCH_ERR_MSG("[%dP:%d]register write fail\n", page, reg);
		return -EIO;
	}

	if (unlikely(touch_i2c_write_byte(client, PAGE_SELECT_REG, DEFAULT_PAGE) < 0)) {
		TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
		return -EIO;
	}

	return 0;
}

int synaptics_ts_get_data(struct i2c_client *client, struct touch_data* data)
{
	struct synaptics_ts_data *ts =
			(struct synaptics_ts_data *)get_touch_handle(client);
	u8 f_index = 0;
	u8 index = 0;
	u8 buf = 0;
	u16 touch_finger_bit_mask = 0;

	data->total_num = 0;

	if (unlikely(touch_debug_mask & DEBUG_TRACE))
		TOUCH_DEBUG_MSG("\n");

	if (unlikely(touch_i2c_read(client, DEVICE_STATUS_REG,
			sizeof(ts->ts_data.interrupt_status_reg),
			&ts->ts_data.device_status_reg) < 0)) {
		TOUCH_ERR_MSG("DEVICE_STATUS_REG read fail\n");
		goto err_synaptics_getdata;
	}

	/* ESD damage check */
	if ((ts->ts_data.device_status_reg & DEVICE_FAILURE_MASK) == DEVICE_FAILURE_MASK) {
		TOUCH_ERR_MSG("ESD damage occured. Reset Touch IC\n");
		goto err_synaptics_device_damage;
	}

	/* Internal reset check */
	if (((ts->ts_data.device_status_reg & DEVICE_STATUS_UNCONFIGURED) >> 7) == 1) {
		TOUCH_ERR_MSG("Touch IC resetted internally. Reconfigure register setting\n");
		goto err_synaptics_device_damage;
	}

	if (unlikely(touch_i2c_read(client, INTERRUPT_STATUS_REG,
			sizeof(ts->ts_data.interrupt_status_reg),
			&ts->ts_data.interrupt_status_reg) < 0)) {
		TOUCH_ERR_MSG("INTERRUPT_STATUS_REG read fail\n");
		goto err_synaptics_getdata;
	}

	if (unlikely(touch_debug_mask & DEBUG_GET_DATA))
		TOUCH_DEBUG_MSG("Interrupt_status : 0x%x\n", ts->ts_data.interrupt_status_reg);

	/* IC bug Exception handling - Interrupt status reg is 0 when interrupt occur */
	if (ts->ts_data.interrupt_status_reg == 0) {
		TOUCH_ERR_MSG("Interrupt_status reg is 0. Something is wrong in IC\n");
		goto err_synaptics_device_damage;
	}

	/* Because of ESD damage... */
	if (unlikely(ts->ts_data.interrupt_status_reg & INTERRUPT_MASK_FLASH)) {
		TOUCH_ERR_MSG("Impossible Interrupt\n");
		goto err_synaptics_device_damage;
	}

	/* Finger */
	if (likely(ts->ts_data.interrupt_status_reg & INTERRUPT_MASK_ABS0)) {
		if (unlikely(touch_i2c_read(client, FINGER_STATE_REG,
				sizeof(ts->ts_data.finger.finger_status_reg),
				ts->ts_data.finger.finger_status_reg) < 0)) {
			TOUCH_ERR_MSG("FINGER_STATE_REG read fail\n");
			goto err_synaptics_getdata;
		}

		touch_finger_bit_mask =
			GET_BIT_MASK(ts->ts_data.finger.finger_status_reg);
		if (unlikely(touch_debug_mask & DEBUG_GET_DATA)) {
			TOUCH_DEBUG_MSG("Finger_status : 0x%x, 0x%x, 0x%x\n",
				ts->ts_data.finger.finger_status_reg[0],
				ts->ts_data.finger.finger_status_reg[1],
				ts->ts_data.finger.finger_status_reg[2]);
			TOUCH_DEBUG_MSG("Touch_bit_mask: 0x%x\n", touch_finger_bit_mask);
		}

		while (touch_finger_bit_mask) {

			for ( ;!((touch_finger_bit_mask >> f_index) & 0x01) && f_index <= MAX_NUM_OF_FINGERS; f_index++)
				;

			if (f_index <= MAX_NUM_OF_FINGERS)
				touch_finger_bit_mask &=
					~(touch_finger_bit_mask & (1<<(f_index))) ;
			if (unlikely(touch_i2c_read(ts->client,
					FINGER_DATA_REG_START + (NUM_OF_EACH_FINGER_DATA_REG * f_index),
					NUM_OF_EACH_FINGER_DATA_REG,
					ts->ts_data.finger.finger_reg[f_index]) < 0)) {
				TOUCH_ERR_MSG("FINGER_DATA_REG read fail\n");
				goto err_synaptics_getdata;
			}

			data->curr_data[f_index].id = f_index;
			data->curr_data[f_index].x_position =
				TS_SNTS_GET_X_POSITION(ts->ts_data.finger.finger_reg[f_index][REG_X_POSITION], ts->ts_data.finger.finger_reg[f_index][REG_YX_POSITION]);
			data->curr_data[f_index].y_position =
				TS_SNTS_GET_Y_POSITION(ts->ts_data.finger.finger_reg[f_index][REG_Y_POSITION], ts->ts_data.finger.finger_reg[f_index][REG_YX_POSITION]);
			data->curr_data[f_index].status = FINGER_PRESSED;

			if (unlikely(touch_debug_mask & DEBUG_GET_DATA))
				TOUCH_DEBUG_MSG("<%d> pos(%4d,%4d) \n",
					f_index,
					data->curr_data[f_index].x_position,
					data->curr_data[f_index].y_position);

			index++;
		}
		data->total_num = index;
		if (unlikely(touch_debug_mask & DEBUG_GET_DATA))
			TOUCH_DEBUG_MSG("Total_num: %d\n", data->total_num);
	}

	/* Palm check */
	if (unlikely(touch_i2c_read(client, TWO_D_EXTEND_STATUS, 1, &buf) < 0)) {
	       TOUCH_ERR_MSG("TWO_D_EXTEND_STATUS read fail\n");
	       goto err_synaptics_getdata;
	}
	data->palm = buf & 0x2;

	return 0;

err_synaptics_device_damage:
err_synaptics_getdata:
	return -EIO;
}

static int read_page_description_table(struct i2c_client *client)
{
	struct synaptics_ts_data *ts =
			(struct synaptics_ts_data *)get_touch_handle(client);
	struct function_descriptor buffer;
	unsigned short u_address = 0;
	unsigned short page_num = 0;

	if (touch_debug_mask & DEBUG_TRACE)
		TOUCH_DEBUG_MSG("\n");

	memset(&buffer, 0x0, sizeof(struct function_descriptor));
	memset(&ts->common_fc, 0x0, sizeof(struct ts_ic_function));
	memset(&ts->finger_fc, 0x0, sizeof(struct ts_ic_function));
	memset(&ts->button_fc, 0x0, sizeof(struct ts_ic_function));
	memset(&ts->analog_fc, 0x0, sizeof(struct ts_ic_function));
	memset(&ts->flash_fc, 0x0, sizeof(struct ts_ic_function));

	for (page_num = 0; page_num < PAGE_MAX_NUM; page_num++) {
		if (unlikely(touch_i2c_write_byte(client,
					PAGE_SELECT_REG, page_num) < 0)) {
			TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
			return -EIO;
		}

		for (u_address = DESCRIPTION_TABLE_START; u_address > 10;
			u_address -= sizeof(struct function_descriptor)) {

			if (unlikely(touch_i2c_read(client,
					u_address, sizeof(buffer),
					(unsigned char *)&buffer) < 0)) {
				TOUCH_ERR_MSG("RMI4 Function Descriptor read fail\n");
				return -EIO;
			}

			if (buffer.id == 0)
				break;

			TOUCH_DEBUG_MSG("buffer.id=[%x], [%x][%x][%x][%x][%x][%x]\n",
				buffer.id,
				buffer.query_base, buffer.command_base,
				buffer.control_base, buffer.data_base,
				buffer.int_source_count, buffer.id);

			switch (buffer.id) {
			case RMI_DEVICE_CONTROL:
				ts->common_fc.dsc = buffer;
				ts->common_fc.function_page = page_num;
				break;
			case TOUCHPAD_SENSORS:
				ts->finger_fc.dsc = buffer;
				ts->finger_fc.function_page = page_num;
				break;
			case CAPACITIVE_BUTTON_SENSORS:
				ts->button_fc.dsc = buffer;
				ts->button_fc.function_page = page_num;
				break;
			case ANALOG_CONTROL:
				ts->analog_fc.dsc = buffer;
				ts->analog_fc.function_page = page_num;
				break;
			case FLASH_MEMORY_MANAGEMENT:
				ts->flash_fc.dsc = buffer;
				ts->flash_fc.function_page = page_num;
			default:
				break;
			}
		}
	}

	if (unlikely(touch_i2c_write_byte(client, PAGE_SELECT_REG, 0x00) < 0)) {
		TOUCH_ERR_MSG("PAGE_SELECT_REG write fail\n");
		return -EIO;
	}

	if (ts->common_fc.dsc.id == 0 || ts->finger_fc.dsc.id == 0
		|| ts->analog_fc.dsc.id == 0 || ts->flash_fc.dsc.id == 0) {
		TOUCH_ERR_MSG("common/finger/analog/flash are not initiailized\n");
		return -EPERM;
	}

	if (touch_debug_mask & DEBUG_BASE_INFO)
		TOUCH_DEBUG_MSG("common[%dP:0x%02x] finger[%dP:0x%02x] button[%dP:0x%02x] analog[%dP:0x%02x] flash[%dP:0x%02x]\n",
			ts->common_fc.function_page, ts->common_fc.dsc.id,
			ts->finger_fc.function_page, ts->finger_fc.dsc.id,
			ts->button_fc.function_page, ts->button_fc.dsc.id,
			ts->analog_fc.function_page, ts->analog_fc.dsc.id,
			ts->flash_fc.function_page, ts->flash_fc.dsc.id);
	return 0;
}

int get_ic_info(struct synaptics_ts_data *ts, struct synaptics_ts_fw_info *fw_info)
{
	u8 device_status = 0;
	u8 flash_control = 0;
	int i;

	read_page_description_table(ts->client);

	memset(&ts->fw_info, 0, sizeof(struct synaptics_ts_fw_info));

	if (unlikely(touch_i2c_read(ts->client, FW_REVISION_REG,
			sizeof(ts->fw_info.fw_rev), &ts->fw_info.fw_rev) < 0)) {
		TOUCH_ERR_MSG("FW_REVISION_REG read fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_read(ts->client, MANUFACTURER_ID_REG,
			sizeof(ts->fw_info.manufacturer_id),
					&ts->fw_info.manufacturer_id) < 0)) {
		TOUCH_ERR_MSG("MANUFACTURER_ID_REG read fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_read(ts->client, PRODUCT_ID_REG,
			sizeof(ts->fw_info.product_id) - 1,
					ts->fw_info.product_id) < 0)) {
		TOUCH_ERR_MSG("PRODUCT_ID_REG read fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_read(ts->client, FLASH_CONFIG_ID_REG,
			sizeof(ts->fw_info.config_id) - 1,
					ts->fw_info.config_id) < 0)) {
		TOUCH_ERR_MSG("FLASH_CONFIG_ID_REG read fail\n");
		return -EIO;
	}

	snprintf(fw_info->ic_fw_identifier, sizeof(fw_info->ic_fw_identifier),
		"%s - %d", ts->fw_info.product_id, ts->fw_info.manufacturer_id);

	for (i = 0; i < sizeof(SynaFirmware)/sizeof(SynaFirmware[0]); i++) {
		if (!strncmp(ts->fw_info.product_id,
				&SynaFirmware[i][FW_OFFSET_PRODUCT_ID],
				sizeof(ts->fw_info.product_id) - 1)) {
			ts->fw_info.fw_start = (unsigned char*)SynaFirmware[i];
			ts->fw_info.fw_size = sizeof(SynaFirmware[i]);
			break;
		}
	}

	if (likely(touch_debug_mask & (DEBUG_FW_UPGRADE | DEBUG_BASE_INFO)))
		TOUCH_INFO_MSG("IC identifier[%s] fw_version[%s]\n",
			ts->fw_info.ic_fw_identifier, ts->fw_info.config_id);


	if (ts->fw_info.fw_start) {
		strncpy(ts->fw_info.fw_image_product_id,
			&ts->fw_info.fw_start[FW_OFFSET_PRODUCT_ID], 10);
		strncpy(ts->fw_info.image_config_id,
			&ts->fw_info.fw_start[FW_OFFSET_IMAGE_VERSION], 4);
		ts->fw_info.fw_image_rev = ts->fw_info.fw_start[31];

		if (likely(touch_debug_mask &
					(DEBUG_FW_UPGRADE | DEBUG_BASE_INFO)))
			TOUCH_INFO_MSG("image_version[%s] : force[%d]\n",
				ts->fw_info.image_config_id,
				ts->fw_info.fw_upgrade.fw_force_upgrade);
	} else {
		TOUCH_ERR_MSG("No matched firmware found! SKIP firmware upgrade\n");
	}

	if (unlikely(touch_i2c_read(ts->client, FLASH_CONTROL_REG,
				sizeof(flash_control), &flash_control) < 0)) {
		TOUCH_ERR_MSG("FLASH_CONTROL_REG read fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_read(ts->client, DEVICE_STATUS_REG,
				sizeof(device_status), &device_status) < 0)) {
		TOUCH_ERR_MSG("DEVICE_STATUS_REG read fail\n");
		return -EIO;
	}

	/* Firmware has a problem, so we should firmware-upgrade */
	if (device_status & DEVICE_STATUS_FLASH_PROG
			|| (device_status & DEVICE_CRC_ERROR_MASK) != 0
			|| (flash_control & FLASH_STATUS_MASK) != 0) {
		TOUCH_ERR_MSG("Firmware has a unknown-problem, so it needs firmware-upgrade.\n");
		TOUCH_ERR_MSG("FLASH_CONTROL[%x] DEVICE_STATUS_REG[%x]\n", (u32)flash_control, (u32)device_status);
		TOUCH_ERR_MSG("FW-upgrade Force Rework.\n");

		/* firmware version info change by force for rework */
		ts->fw_info.fw_upgrade.fw_force_upgrade = 1;
		snprintf(ts->fw_info.config_id, sizeof(ts->fw_info.config_id), "ERR");
	}

	return 0;
}

int synaptics_init_panel(struct i2c_client *client, struct synaptics_ts_fw_info *fw_info)
{
	struct synaptics_ts_data *ts =
			(struct synaptics_ts_data *)get_touch_handle(client);
	u8 buf;

	if (touch_debug_mask & DEBUG_TRACE)
		TOUCH_DEBUG_MSG("\n");

	if (!ts->is_probed)
		if (unlikely(get_ic_info(ts, fw_info) < 0))
			return -EIO;

	if (unlikely(touch_i2c_write_byte(client, DEVICE_CONTROL_REG,
		DEVICE_CONTROL_NOSLEEP | DEVICE_CONTROL_CONFIGURED) < 0)) {
		TOUCH_ERR_MSG("DEVICE_CONTROL_REG write fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_read(client,
				INTERRUPT_ENABLE_REG, 1, &buf) < 0)) {
		TOUCH_ERR_MSG("INTERRUPT_ENABLE_REG read fail\n");
		return -EIO;
	}
	if (unlikely(touch_i2c_write_byte(client, INTERRUPT_ENABLE_REG,
			buf | INTERRUPT_MASK_ABS0 | INTERRUPT_MASK_BUTTON) < 0)) {
		TOUCH_ERR_MSG("INTERRUPT_ENABLE_REG write fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_write_byte(client, TWO_D_REPORTING_MODE,
				REPORT_MODE_CONTINUOUS) < 0)) {
			TOUCH_ERR_MSG("TWO_D_REPORTING_MODE write fail\n");
			return -EIO;
	}

	if (unlikely(touch_i2c_read(client, INTERRUPT_STATUS_REG, 1, &buf) < 0)) {
		TOUCH_ERR_MSG("INTERRUPT_STATUS_REG read fail\n");
		return -EIO;
	}

	if (unlikely(touch_i2c_read(client, FINGER_STATE_REG,
			sizeof(ts->ts_data.finger.finger_status_reg),
			ts->ts_data.finger.finger_status_reg) < 0)) {
		TOUCH_ERR_MSG("FINGER_STATE_REG read fail\n");
		return -EIO;
	}

	ts->is_probed = 1;

	return 0;
}

int synaptics_ts_power(struct i2c_client *client, int power_ctrl)
{
	struct synaptics_ts_data *ts =
			(struct synaptics_ts_data *)get_touch_handle(client);

	if (touch_debug_mask & DEBUG_TRACE)
		TOUCH_DEBUG_MSG("\n");

	switch (power_ctrl) {
	case POWER_OFF:
		gpio_set_value(ts->pdata->reset_gpio, 0);
		synaptics_t1320_power_on(client, 0);
		break;
	case POWER_ON:
		synaptics_t1320_power_on(client, 1);
		gpio_set_value(ts->pdata->reset_gpio, 1);
		break;
	case POWER_SLEEP:
		if (unlikely(touch_i2c_write_byte(client, DEVICE_CONTROL_REG,
				DEVICE_CONTROL_SLEEP | DEVICE_CONTROL_CONFIGURED) < 0)) {
			TOUCH_ERR_MSG("DEVICE_CONTROL_REG write fail\n");
			return -EIO;
		}
		break;
	case POWER_WAKE:
		if (unlikely(touch_i2c_write_byte(client, DEVICE_CONTROL_REG,
				DEVICE_CONTROL_NORMAL_OP | DEVICE_CONTROL_CONFIGURED) < 0)) {
			TOUCH_ERR_MSG("DEVICE_CONTROL_REG write fail\n");
			return -EIO;
		}
		break;
	default:
		return -EIO;
		break;
	}

	return 0;
}

int synaptics_ts_ic_ctrl(struct i2c_client *client, u8 code, u16 value)
{
	struct synaptics_ts_data *ts =
			(struct synaptics_ts_data *)get_touch_handle(client);
	u8 buf = 0;

	switch (code) {
	case IC_CTRL_BASELINE:
		switch (value) {
		case BASELINE_OPEN:
			if (unlikely(synaptics_ts_page_data_write_byte(client,
					ANALOG_PAGE, ANALOG_CONTROL_REG,
					FORCE_FAST_RELAXATION) < 0)) {
				TOUCH_ERR_MSG("ANALOG_CONTROL_REG write fail\n");
				return -EIO;
			}

			msleep(10);

			if (unlikely(synaptics_ts_page_data_write_byte(client,
					ANALOG_PAGE, ANALOG_COMMAND_REG,
					FORCE_UPDATE) < 0)) {
				TOUCH_ERR_MSG("ANALOG_COMMAND_REG write fail\n");
				return -EIO;
			}

			if (unlikely(touch_debug_mask & DEBUG_GHOST))
				TOUCH_INFO_MSG("BASELINE_OPEN\n");

			break;
		case BASELINE_FIX:
			if (unlikely(synaptics_ts_page_data_write_byte(client,
				ANALOG_PAGE, ANALOG_CONTROL_REG, 0x00) < 0)) {
				TOUCH_ERR_MSG("ANALOG_CONTROL_REG write fail\n");
				return -EIO;
			}

			msleep(10);

			if (unlikely(synaptics_ts_page_data_write_byte(client,
					ANALOG_PAGE, ANALOG_COMMAND_REG,
					FORCE_UPDATE) < 0)) {
				TOUCH_ERR_MSG("ANALOG_COMMAND_REG write fail\n");
				return -EIO;
			}

			if (unlikely(touch_debug_mask & DEBUG_GHOST))
				TOUCH_INFO_MSG("BASELINE_FIX\n");

			break;
		case BASELINE_REBASE:
			/* rebase base line */
			if (likely(ts->finger_fc.dsc.id != 0)) {
				if (unlikely(touch_i2c_write_byte(client,
						FINGER_COMMAND_REG, 0x1) < 0)) {
					TOUCH_ERR_MSG("finger baseline reset command write fail\n");
					return -EIO;
				}
			}
			break;
		default:
			break;
		}
		break;
	case IC_CTRL_READ:
		if (touch_i2c_read(client, value, 1, &buf) < 0) {
			TOUCH_ERR_MSG("IC register read fail\n");
			return -EIO;
		}
		break;
	case IC_CTRL_WRITE:
		if (touch_i2c_write_byte(client, ((value & 0xFF00) >> 8),
						(value & 0xFF)) < 0) {
			TOUCH_ERR_MSG("IC register write fail\n");
			return -EIO;
		}
		break;
	case IC_CTRL_RESET_CMD:
		if (unlikely(touch_i2c_write_byte(client,
					DEVICE_COMMAND_REG, 0x1) < 0)) {
			TOUCH_ERR_MSG("IC Reset command write fail\n");
			return -EIO;
		}
		break;
	default:
		break;
	}

	return buf;
}

static ssize_t
show_fw_ver(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", ts->fw_info.config_id);
}

static ssize_t
store_fw_upgrade(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int value = 0;
	int repeat = 0;
	char path[256] = {0};
	struct synaptics_ts_data *ts = dev_get_drvdata(dev);

	sscanf(buf, "%d %s", &value, path);

	TOUCH_INFO_MSG("Firmware image path: %s\n", path[0] != 0 ? path : "Internal");

	if (value) {
		for (repeat = 0; repeat < value; repeat++) {
			/* sync for n-th repeat test */
			while (ts->fw_info.fw_upgrade.is_downloading)
				;

			msleep(BOOTING_DELAY * 2);
			TOUCH_INFO_MSG("Firmware image upgrade: No.%d", repeat+1);

			/* for n-th repeat test - because ts->fw_info.fw_upgrade is setted 0 after FW upgrade */
			memcpy(ts->fw_info.fw_upgrade.fw_path,
				path, sizeof(ts->fw_info.fw_upgrade.fw_path)-1);

			/* set downloading flag for sync for n-th test */
			ts->fw_info.fw_upgrade.is_downloading = UNDER_DOWNLOADING;
			ts->fw_info.fw_upgrade.fw_force_upgrade = 1;
			queue_work(synaptics_wq, &ts->work_fw_upgrade);
		}

		/* sync for fw_upgrade test */
		while (ts->fw_info.fw_upgrade.is_downloading)
			;
	}

	return count;
}

static ssize_t
ic_register_ctrl(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned char string[6];
	int reg = 0;
	int value = 0;
	int ret = 0;
	u32 write_data;
	struct synaptics_ts_data *ts = dev_get_drvdata(dev);

	sscanf(buf, "%s %d %d", string, &reg, &value);

	if (ts->curr_pwr_state == POWER_ON || ts->curr_pwr_state == POWER_WAKE) {
		if (!strncmp(string, "read", 4)) {
			do {
				ret = synaptics_ts_ic_ctrl(ts->client,
							IC_CTRL_READ, reg);
				if (ret >= 0)
					TOUCH_INFO_MSG("register[0x%x] = 0x%x\n", reg, ret);
				else
					TOUCH_ERR_MSG("cannot read register[0x%x]\n", reg);
				reg++;
			} while (--value > 0);
		} else if (!strncmp(string, "write", 4)) {
			write_data = ((0xFF & reg) << 8) | (0xFF & value);
			ret = synaptics_ts_ic_ctrl(ts->client,
						IC_CTRL_WRITE, write_data);
			if (ret >= 0)
				TOUCH_INFO_MSG("register[0x%x] is set to 0x%x\n", reg, value);
			else
				TOUCH_ERR_MSG("cannot write register[0x%x]\n", reg);
		} else {
			TOUCH_INFO_MSG("Usage: echo [read | write] reg_num value > ic_rw\n");
			TOUCH_INFO_MSG(" - reg_num : register address\n");
			TOUCH_INFO_MSG(" - value [read] : number of register starting form reg_num\n");
			TOUCH_INFO_MSG(" - value [write] : set value into reg_num\n");
		}
	} else {
		TOUCH_INFO_MSG("state=[suspend]. we cannot use I2C, now\n");
	}

	return count;
}

static ssize_t
store_ts_reset(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = dev_get_drvdata(dev);
	unsigned char string[5];
	u8 saved_state = ts->curr_pwr_state;
	int ret = 0;

	sscanf(buf, "%s", string);

	disable_irq_nosync(ts->client->irq);

	cancel_work_sync(&ts->work);
	cancel_delayed_work_sync(&ts->work_init);

	release_all_ts_event(ts);

	if (saved_state == POWER_ON || saved_state == POWER_WAKE) {
		if (!strncmp(string, "soft", 4)) {
			synaptics_ts_ic_ctrl(ts->client, IC_CTRL_RESET_CMD, 0);
		} else if (!strncmp(string, "pin", 3)) {
			gpio_set_value(ts->pdata->reset_gpio, 0);
			msleep(RESET_DELAY);
			gpio_set_value(ts->pdata->reset_gpio, 1);
		} else if (!strncmp(string, "vdd", 3)) {
			touch_power_cntl(ts, POWER_OFF);
			touch_power_cntl(ts, POWER_ON);
		} else {
			TOUCH_INFO_MSG("Usage: echo [soft | pin | vdd] > power_control\n");
			TOUCH_INFO_MSG(" - soft : reset using IC register setting\n");
			TOUCH_INFO_MSG(" - soft : reset using reset pin\n");
			TOUCH_INFO_MSG(" - hard : reset using VDD\n");
		}

		if (ret < 0)
			TOUCH_ERR_MSG("reset fail\n");
		else
			atomic_set(&ts->device_init, 0);

		msleep(BOOTING_DELAY);

	} else {
		TOUCH_INFO_MSG("Touch is suspend state. Don't need reset\n");
	}

	enable_irq(ts->client->irq);

	if (saved_state == POWER_ON || saved_state == POWER_WAKE)
		touch_ic_init(ts);

	return count;
}

static struct device_attribute synaptics_device_attrs[] = {
	__ATTR(firmware, S_IRUGO | S_IWUSR, NULL, store_fw_upgrade),
	__ATTR(reg_control, S_IRUGO | S_IWUSR, NULL, ic_register_ctrl),
	__ATTR(power_control, S_IRUGO | S_IWUSR, NULL, store_ts_reset),
	__ATTR(version, S_IRUGO | S_IWUSR, show_fw_ver, NULL),
};

static int synaptics_get_dt_coords(struct device *dev, char *name,
				u32 *x, u32 *y)
{
	u32 coords[SYNAPTICS_COORDS_ARR_SIZE];
	struct property *prop;
	struct device_node *np = dev->of_node;
	int coords_size, rc;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;

	if (!prop->value)
		return -ENODATA;

	coords_size = prop->length / sizeof(u32);
	if (coords_size != SYNAPTICS_COORDS_ARR_SIZE) {
		dev_err(dev, "invalid %s\n", name);
		return -EINVAL;
	}

	rc = of_property_read_u32_array(np, name, coords, coords_size);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Unable to read %s\n", name);
		return rc;
	}

	*x = coords[2];
	*y = coords[3];

	return 0;
}

static int synaptics_parse_dt(struct device *dev, struct touch_platform_data *pdata)
{
	int rc;
	u32 temp_val;
	struct device_node *np = dev->of_node;

	rc = synaptics_get_dt_coords(dev, "synaptics,panel-coords",
					&pdata->x_max, &pdata->y_max);
	if (rc)
		return rc;

	rc = synaptics_get_dt_coords(dev, "synaptics,display-coords",
					&pdata->lcd_x, &pdata->lcd_y);
	if (rc)
		return rc;

	rc = of_property_read_u32(np, "synaptics,max_id", &temp_val);
	if (rc) {
		dev_err(dev, "Unable to read max_id\n");
		return rc;
	} else {
		pdata->max_id = temp_val;
	}

	/* reset, irq gpio info */
	pdata->reset_gpio = of_get_named_gpio_flags(np, "synaptics,reset-gpio",
				0, &pdata->reset_gpio_flags);
	pdata->irq_gpio = of_get_named_gpio_flags(np, "synaptics,irq-gpio",
				0, &pdata->irq_gpio_flags);

	return 0;
}

static int synaptics_ts_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{
	struct synaptics_ts_data *ts;
	int ret = 0;
	u8 i2c_test = 0;
	int i;

	if (touch_debug_mask & DEBUG_TRACE)
		TOUCH_DEBUG_MSG("\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		TOUCH_ERR_MSG("i2c functionality check error\n");
		return -EPERM;
	}

	ts = kzalloc(sizeof(struct synaptics_ts_data), GFP_KERNEL);
	if (!ts) {
		TOUCH_ERR_MSG("Can not allocate memory\n");
		return -ENOMEM;
	}

	if (client->dev.of_node) {
		ts->pdata  = devm_kzalloc(&client->dev,
			sizeof(struct touch_platform_data), GFP_KERNEL);
		if (!ts->pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err_alloc_data_failed;
		}

		ret = synaptics_parse_dt(&client->dev, ts->pdata);
		if (ret)
			goto err_alloc_platformdata_failed;
	} else {
		ts->pdata  = client->dev.platform_data;
		if (!ts->pdata) {
			ret = -EINVAL;
			goto err_alloc_data_failed;
		}
	}

	ts->ic_init_err_cnt = 0;
	ts->work_sync_err_cnt = 0;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	set_touch_handle(client, ts);

	/* reset pin setting */
	ret = gpio_request(ts->pdata->reset_gpio, "touch_reset");
	if (ret < 0) {
		TOUCH_ERR_MSG("FAIL: touch_reset gpio_request\n");
		goto err_assign_platform_data;
	}
	gpio_direction_output(ts->pdata->reset_gpio, 1);

	atomic_set(&ts->device_init, 0);
	ts->curr_resume_state = 1;

	/* Power on */
	if (touch_power_cntl(ts, POWER_ON) < 0)
		goto err_power_failed;

	msleep(BOOTING_DELAY);

	/* init work_queue */
	INIT_WORK(&ts->work, synaptics_ts_work_func);
	INIT_DELAYED_WORK(&ts->work_init, touch_init_func);
	INIT_WORK(&ts->work_fw_upgrade, touch_fw_upgrade_func);

	/* input dev setting */
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		TOUCH_ERR_MSG("Failed to allocate input device\n");
		ret = -ENOMEM;
		goto err_input_dev_alloc_failed;
	}

	ts->input_dev->name = "touch_dev";

	set_bit(EV_SYN, ts->input_dev->evbit);
	set_bit(EV_ABS, ts->input_dev->evbit);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 0, 0))
	set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
#endif

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0,
						ts->pdata->x_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0,
						ts->pdata->y_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0,
						ts->pdata->max_id, 0, 0);

	ret = input_register_device(ts->input_dev);
	if (ret < 0) {
		TOUCH_ERR_MSG("Unable to register %s input device\n",
				ts->input_dev->name);
		goto err_input_register_device_failed;
	}

	/* interrupt mode */
	ret = gpio_request(ts->pdata->irq_gpio, "touch_int");
	if (ret < 0) {
		TOUCH_ERR_MSG("FAIL: touch_int gpio_request\n");
		goto err_interrupt_failed;
	}
	gpio_direction_input(ts->pdata->irq_gpio);

	ret = request_threaded_irq(client->irq, touch_irq_handler,
			NULL,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT, client->name, ts);

	if (ret < 0) {
		TOUCH_ERR_MSG("request_irq failed. use polling mode\n");
		goto err_interrupt_failed;
	}

	/* Add i2c check routine for booting in no touch panel/ic case */
	for (i = 0; i < MAX_RETRY_COUNT; i++) {
		if (unlikely(touch_i2c_read(ts->client, FW_REVISION_REG,
				sizeof(i2c_test), &i2c_test) < 0)) {
			TOUCH_ERR_MSG("Touch I2C  read fail\n");
			if (i == MAX_RETRY_COUNT-1) {
				TOUCH_ERR_MSG("No Touch Panel \n");
				return -EIO;
			}
		} else {
			TOUCH_DEBUG_MSG("Touch I2C read success \n");
			break;
		}
	}

	/* Specific device initialization */
	touch_ic_init(ts);

	/* Firmware Upgrade Check - use thread for booting time reduction */
	if (ts->fw_info.fw_start != NULL)
		queue_work(synaptics_wq, &ts->work_fw_upgrade);

#if defined(CONFIG_HAS_EARLYSUSPEND)
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = touch_early_suspend;
	ts->early_suspend.resume = touch_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

	for (i = 0; i < ARRAY_SIZE(synaptics_device_attrs); i++) {
		ret = device_create_file(&client->dev,
						&synaptics_device_attrs[i]);
		if (ret)
			goto err_dev_create_file;
	}

	return ret;

err_dev_create_file:
	free_irq(ts->client->irq, ts);
err_interrupt_failed:
err_input_register_device_failed:
	input_free_device(ts->input_dev);
err_input_dev_alloc_failed:
	touch_power_cntl(ts, POWER_OFF);
err_power_failed:
err_assign_platform_data:
	kfree(ts);
	return ret;
err_alloc_platformdata_failed:
	kfree(ts->pdata);
err_alloc_data_failed:
	kfree(ts);

	return ret;
}

static int synaptics_ts_remove(struct i2c_client *client)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);

	/* Power off */
	touch_power_cntl(ts, POWER_OFF);
	unregister_early_suspend(&ts->early_suspend);
	free_irq(client->irq, ts);
	input_unregister_device(ts->input_dev);
	kfree(ts);

	return 0;
}

#if defined(CONFIG_HAS_EARLYSUSPEND)
static void touch_early_suspend(struct early_suspend *h)
{
	struct synaptics_ts_data *ts =
		container_of(h, struct synaptics_ts_data, early_suspend);

	if (unlikely(touch_debug_mask & DEBUG_TRACE))
		TOUCH_DEBUG_MSG("\n");

	ts->curr_resume_state = 0;

	if (ts->fw_info.fw_upgrade.is_downloading == UNDER_DOWNLOADING) {
		TOUCH_INFO_MSG("early_suspend is not executed\n");
		return;
	}

	disable_irq(ts->client->irq);
	cancel_work_sync(&ts->work);
	cancel_delayed_work_sync(&ts->work_init);
	release_all_ts_event(ts);
	touch_power_cntl(ts, POWER_OFF);
}

static void touch_late_resume(struct early_suspend *h)
{
	struct synaptics_ts_data *ts =
		container_of(h, struct synaptics_ts_data, early_suspend);

	if (unlikely(touch_debug_mask & DEBUG_TRACE))
		TOUCH_DEBUG_MSG("\n");

	ts->curr_resume_state = 1;

	if (ts->fw_info.fw_upgrade.is_downloading == UNDER_DOWNLOADING) {
		TOUCH_INFO_MSG("late_resume is not executed\n");
		return;
	}

	touch_power_cntl(ts, POWER_ON);
	enable_irq(ts->client->irq);
	queue_delayed_work(synaptics_wq, &ts->work_init, msecs_to_jiffies(BOOTING_DELAY));
}
#endif

#if defined(CONFIG_PM)
static int touch_suspend(struct device *device)
{
	return 0;
}

static int touch_resume(struct device *device)
{
	return 0;
}
#endif

static struct of_device_id synaptics_match_table[] = {
	{ .compatible = "synaptics,s7020",},
	{ },
};

static struct i2c_device_id synaptics_ts_id[] = {
	{ LGE_TOUCH_NAME, 0 },
};

#if defined(CONFIG_PM)
static struct dev_pm_ops touch_pm_ops = {
	.suspend 	= touch_suspend,
	.resume 	= touch_resume,
};
#endif

static struct i2c_driver synaptics_ts_driver = {
	.probe          = synaptics_ts_probe,
	.remove         = synaptics_ts_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend        = synaptics_ts_suspend,
	.resume         = synaptics_ts_resume,
#endif
	.id_table       = synaptics_ts_id,
	.driver = {
		.name   = LGE_TOUCH_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = synaptics_match_table,
#if defined(CONFIG_PM)
		.pm     = &touch_pm_ops,
#endif
	},
};

static int __devinit synaptics_ts_init(void)
{
	synaptics_wq = create_singlethread_workqueue("synaptics_wq");
	if (!synaptics_wq)
		return -ENOMEM;
	return i2c_add_driver(&synaptics_ts_driver);
}

static void __exit synaptics_ts_exit(void)
{
	i2c_del_driver(&synaptics_ts_driver);
	if (synaptics_wq)
		destroy_workqueue(synaptics_wq);
}

module_init(synaptics_ts_init);
module_exit(synaptics_ts_exit);

MODULE_DESCRIPTION("Synaptics Touchscreen Driver");
MODULE_LICENSE("GPL");