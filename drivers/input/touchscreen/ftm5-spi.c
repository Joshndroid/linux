// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics FingerTipS FTM5 touchscreen, SPI (4-wire)
 *
 * Minimal driver for the FTM5 controller used on the Pixel 8 Pro (husky,
 * D-chip ID 0x36 0x40).  The wire protocol follows ST's FTS v5.2.1 library
 * (drivers/input/touchscreen/fts_spi): HW register access with a 4-byte
 * address, a dummy byte in front of every SPI read, and 8-byte FIFO events.
 *
 * It relies on the firmware and calibration already stored in the
 * controller's flash: no firmware update, no self-test.
 *
 * Tap to wake: when the panel it is attached to ("panel" phandle) powers
 * down, the controller is put in its low-power scan with only the firmware's
 * double-tap gesture enabled, and a double tap is reported as KEY_WAKEUP.
 * The panel powering back up returns it to active multitouch scanning.
 */

#include <drm/drm_panel.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#define FTM5_CMD_HW_REG_W	0xFA
#define FTM5_CMD_HW_REG_R	0xFB
#define FTM5_CMD_FIFO_READ	0x87
#define FTM5_CMD_SCAN_MODE	0xA0
#define FTM5_CMD_FEATURE	0xA2

#define FTM5_ADDR_DCHIP_ID	0x20000000
#define FTM5_ADDR_FW_VER	0x20000004

#define FTM5_SCAN_ACTIVE	0x00
#define FTM5_SCAN_LOW_POWER	0x01
#define FTM5_FEAT_GESTURE	0x03
#define FTM5_GEST_ID_DBLTAP	0x05
#define EVT_TYPE_USER_GESTURE	0x02
#define FTM5_ACTIVE_MULTITOUCH	0x01

#define FTM5_EVENT_SIZE		8
#define FTM5_FIFO_DEPTH		32

#define EVT_ID_NOEVENT		0x00
#define EVT_ID_CONTROLLER_READY	0x03
#define EVT_ID_ENTER_POINT	0x13
#define EVT_ID_MOTION_POINT	0x23
#define EVT_ID_LEAVE_POINT	0x33
#define EVT_ID_STATUS_UPDATE	0x43
#define EVT_ID_USER_REPORT	0x53
#define EVT_ID_DEBUG		0xE3
#define EVT_ID_ERROR		0xF3

#define TOUCH_TYPE_INVALID	0x00
#define TOUCH_TYPE_FINGER	0x01
#define TOUCH_TYPE_GLOVE	0x02
#define TOUCH_TYPE_STYLUS	0x03
#define TOUCH_TYPE_PALM		0x04
#define TOUCH_TYPE_HOVER	0x05

#define FTM5_MAX_FINGERS	10

struct ftm5 {
	struct spi_device *spi;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];
	u8 chip_id[2];
	struct drm_panel_follower panel_follower;
	struct mutex lock;		/* scan mode changes */
	bool low_power;			/* panel off: gesture scan only */
};

static int ftm5_write(struct ftm5 *ts, const u8 *buf, size_t len)
{
	return spi_write(ts->spi, buf, len);
}

/* Every FTM5 SPI read returns one dummy byte before the data. */
static int ftm5_read(struct ftm5 *ts, const u8 *cmd, size_t cmd_len,
		     u8 *data, size_t len)
{
	u8 buf[FTM5_EVENT_SIZE + 1];
	int ret;

	if (len > FTM5_EVENT_SIZE)
		return -EINVAL;
	ret = spi_write_then_read(ts->spi, cmd, cmd_len, buf, len + 1);
	if (ret)
		return ret;
	memcpy(data, buf + 1, len);
	return 0;
}

static int ftm5_read_reg(struct ftm5 *ts, u32 addr, u8 *data, size_t len)
{
	const u8 cmd[] = { FTM5_CMD_HW_REG_R, addr >> 24, addr >> 16,
			   addr >> 8, addr };

	return ftm5_read(ts, cmd, sizeof(cmd), data, len);
}

static int ftm5_read_event(struct ftm5 *ts, u8 *event)
{
	const u8 cmd = FTM5_CMD_FIFO_READ;

	return ftm5_read(ts, &cmd, 1, event, FTM5_EVENT_SIZE);
}

static int ftm5_wait_ready(struct ftm5 *ts)
{
	u8 event[FTM5_EVENT_SIZE];
	int i, ret;

	for (i = 0; i < 100; i++) {
		ret = ftm5_read_event(ts, event);
		if (ret)
			return ret;
		if (event[0] == EVT_ID_CONTROLLER_READY)
			return 0;
		if (event[0] == EVT_ID_ERROR)
			dev_warn(&ts->spi->dev, "error event %8ph during reset\n",
				 event);
		usleep_range(10000, 11000);
	}
	return -ETIMEDOUT;
}

static void ftm5_report(struct ftm5 *ts, const u8 *ev)
{
	unsigned int slot = ev[1] >> 4;
	unsigned int type = ev[1] & 0x0f;
	unsigned int x, y, major, minor;

	if (slot >= FTM5_MAX_FINGERS)
		return;

	input_mt_slot(ts->input, slot);

	if (ev[0] == EVT_ID_LEAVE_POINT || type == TOUCH_TYPE_HOVER ||
	    type == TOUCH_TYPE_INVALID) {
		input_mt_report_slot_inactive(ts->input);
		return;
	}

	x = ((ev[3] & 0x0f) << 8) | ev[2];
	y = (ev[4] << 4) | (ev[3] >> 4);
	major = ((ev[0] & 0x0c) << 2) | (ev[6] >> 4);
	minor = ((ev[7] & 0xc0) >> 2) | (ev[6] & 0x0f);

	input_mt_report_slot_state(ts->input,
				   type == TOUCH_TYPE_STYLUS ? MT_TOOL_PEN :
				   type == TOUCH_TYPE_PALM ? MT_TOOL_PALM :
				   MT_TOOL_FINGER, true);
	touchscreen_report_pos(ts->input, &ts->prop, x, y, true);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, major);
	input_report_abs(ts->input, ABS_MT_TOUCH_MINOR, minor);
}

static irqreturn_t ftm5_irq(int irq, void *data)
{
	struct ftm5 *ts = data;
	u8 ev[FTM5_EVENT_SIZE];
	bool touched = false;
	int i;

	for (i = 0; i < FTM5_FIFO_DEPTH; i++) {
		if (ftm5_read_event(ts, ev))
			break;
		if (ev[0] == EVT_ID_NOEVENT)
			break;

		switch (ev[0]) {
		case EVT_ID_ENTER_POINT:
		case EVT_ID_MOTION_POINT:
		case EVT_ID_LEAVE_POINT:
			if (READ_ONCE(ts->low_power))
				break;	/* stale points from before the switch */
			ftm5_report(ts, ev);
			touched = true;
			break;
		case EVT_ID_USER_REPORT:
			if (ev[1] == EVT_TYPE_USER_GESTURE &&
			    ev[2] == FTM5_GEST_ID_DBLTAP) {
				dev_dbg(&ts->spi->dev, "double tap\n");
				pm_wakeup_event(&ts->spi->dev, 0);
				input_report_key(ts->input, KEY_WAKEUP, 1);
				input_sync(ts->input);
				input_report_key(ts->input, KEY_WAKEUP, 0);
				input_sync(ts->input);
			} else {
				dev_dbg(&ts->spi->dev, "user report %8ph\n", ev);
			}
			break;
		case EVT_ID_ERROR:
			dev_warn_ratelimited(&ts->spi->dev, "error event %8ph\n", ev);
			break;
		case EVT_ID_CONTROLLER_READY:
			dev_warn(&ts->spi->dev, "controller reset itself\n");
			break;
		default:
			dev_dbg(&ts->spi->dev, "event %8ph\n", ev);
			break;
		}
	}

	if (touched) {
		input_mt_sync_frame(ts->input);
		input_sync(ts->input);
	}
	return IRQ_HANDLED;
}

/*
 * ST's library writes zero to the controller's system register block before
 * every scan-mode command (setScanMode()); do the same.
 */
static int ftm5_set_scan(struct ftm5 *ts, const u8 *cmd, size_t len)
{
	static const u8 sys[] = { FTM5_CMD_HW_REG_W, 0x20, 0x00, 0x00, 0x00,
				  0x00, 0x00 };
	int ret;

	ret = ftm5_write(ts, sys, sizeof(sys));
	if (!ret)
		ret = ftm5_write(ts, cmd, len);
	return ret;
}

static void ftm5_release_all(struct ftm5 *ts)
{
	int i;

	for (i = 0; i < FTM5_MAX_FINGERS; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_inactive(ts->input);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static int ftm5_enter_low_power(struct ftm5 *ts)
{
	/* gesture bitmask: bit (id % 8) of byte (id / 8), 4 bytes */
	const u8 feat[] = { FTM5_CMD_FEATURE, FTM5_FEAT_GESTURE,
			    BIT(FTM5_GEST_ID_DBLTAP), 0, 0, 0 };
	const u8 scan[] = { FTM5_CMD_SCAN_MODE, FTM5_SCAN_LOW_POWER };
	int ret;

	guard(mutex)(&ts->lock);
	if (ts->low_power)
		return 0;
	disable_irq(ts->spi->irq);
	WRITE_ONCE(ts->low_power, true);
	ftm5_release_all(ts);
	ret = ftm5_write(ts, feat, sizeof(feat));
	if (!ret)
		ret = ftm5_set_scan(ts, scan, sizeof(scan));
	enable_irq(ts->spi->irq);
	if (ret) {
		dev_err(&ts->spi->dev, "cannot enter gesture mode: %d\n", ret);
		return ret;
	}
	dev_dbg(&ts->spi->dev, "gesture mode (tap to wake)\n");
	return 0;
}

static int ftm5_enter_active(struct ftm5 *ts)
{
	const u8 scan[] = { FTM5_CMD_SCAN_MODE, FTM5_SCAN_ACTIVE,
			    FTM5_ACTIVE_MULTITOUCH };
	int ret;

	guard(mutex)(&ts->lock);
	if (!ts->low_power)
		return 0;
	ret = ftm5_set_scan(ts, scan, sizeof(scan));
	if (ret) {
		dev_err(&ts->spi->dev, "cannot resume active scan: %d\n", ret);
		return ret;
	}
	WRITE_ONCE(ts->low_power, false);
	return 0;
}

static int ftm5_panel_prepared(struct drm_panel_follower *follower)
{
	struct ftm5 *ts = container_of(follower, struct ftm5, panel_follower);

	return ftm5_enter_active(ts);
}

static int ftm5_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct ftm5 *ts = container_of(follower, struct ftm5, panel_follower);

	/* Failing here would only leave touch in active scan; never block. */
	ftm5_enter_low_power(ts);
	return 0;
}

static const struct drm_panel_follower_funcs ftm5_panel_follower_funcs = {
	.panel_prepared = ftm5_panel_prepared,
	.panel_unpreparing = ftm5_panel_unpreparing,
};

static int ftm5_start(struct ftm5 *ts)
{
	struct device *dev = &ts->spi->dev;
	const u8 scan[] = { FTM5_CMD_SCAN_MODE, FTM5_SCAN_ACTIVE,
			    FTM5_ACTIVE_MULTITOUCH };
	u8 fw[2];
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ts->supplies), ts->supplies);
	if (ret)
		return ret;
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);

	ret = ftm5_wait_ready(ts);
	if (ret) {
		dev_err(dev, "no CONTROLLER_READY after reset: %d\n", ret);
		goto power_off;
	}

	ret = ftm5_read_reg(ts, FTM5_ADDR_DCHIP_ID, ts->chip_id, 2);
	if (!ret)
		ret = ftm5_read_reg(ts, FTM5_ADDR_FW_VER, fw, 2);
	if (ret)
		goto power_off;
	dev_info(dev, "FTM5 chip %02x%02x, firmware %02x%02x\n",
		 ts->chip_id[0], ts->chip_id[1], fw[1], fw[0]);

	ret = ftm5_write(ts, scan, sizeof(scan));
	if (ret)
		goto power_off;
	return 0;

power_off:
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
	return ret;
}

static int ftm5_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ftm5 *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->spi = spi;
	spi_set_drvdata(spi, ts);
	mutex_init(&ts->lock);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	ts->supplies[0].supply = "vdd";
	ts->supplies[1].supply = "avdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ts->supplies),
				      ts->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	ts->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio), "reset gpio\n");

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "FTM5 touchscreen";
	ts->input->id.bustype = BUS_SPI;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 4095, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 4095, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);
	input_set_capability(ts->input, EV_KEY, KEY_WAKEUP);
	touchscreen_parse_properties(ts->input, true, &ts->prop);
	ret = input_mt_init_slots(ts->input, FTM5_MAX_FINGERS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = ftm5_start(ts);
	if (ret)
		return dev_err_probe(dev, ret, "controller initialisation failed\n");

	ret = devm_request_threaded_irq(dev, spi->irq, NULL, ftm5_irq,
					IRQF_ONESHOT, "ftm5", ts);
	if (ret)
		return dev_err_probe(dev, ret, "irq %d\n", spi->irq);

	ret = input_register_device(ts->input);
	if (ret)
		return ret;

	device_init_wakeup(dev, true);

	/* Tap to wake follows the panel's power; optional. */
	if (drm_is_panel_follower(dev)) {
		ts->panel_follower.funcs = &ftm5_panel_follower_funcs;
		ret = devm_drm_panel_add_follower(dev, &ts->panel_follower);
		if (ret)
			return dev_err_probe(dev, ret, "panel follower\n");
	}
	return 0;
}

static void ftm5_remove(struct spi_device *spi)
{
	struct ftm5 *ts = spi_get_drvdata(spi);

	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ts->supplies), ts->supplies);
}

static const struct of_device_id ftm5_of_match[] = {
	{ .compatible = "st,ftm5-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, ftm5_of_match);

static const struct spi_device_id ftm5_spi_ids[] = {
	{ "ftm5-spi" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ftm5_spi_ids);

static struct spi_driver ftm5_driver = {
	.driver = {
		.name = "ftm5-spi",
		.of_match_table = ftm5_of_match,
	},
	.id_table = ftm5_spi_ids,
	.probe = ftm5_probe,
	.remove = ftm5_remove,
};
module_spi_driver(ftm5_driver);

MODULE_DESCRIPTION("ST FingerTipS FTM5 SPI touchscreen");
MODULE_LICENSE("GPL");
