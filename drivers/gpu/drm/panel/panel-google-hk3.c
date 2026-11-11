// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Pixel 8 Pro (husky) HK3 MIPI-DSI panel
 *
 * Command sequences, DSC configuration and timings are taken from Google's
 * downstream panel-google-hk3 driver (android-gs-shusky-6.1).  This driver
 * implements the WQHD (1344x2992) normal-mode panel at 120 Hz
 * (preferred) and 60 Hz in HS operation with manual frame insertion, plus DCS
 * brightness.  AOD/LP,
 * HBM, LHBM, NS mode, auto refresh and FHD are not implemented.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define HK3_HS_CLK_MBPS		1368
#define HK3_BRIGHTNESS_MAX	4095
#define HK3_BRIGHTNESS_DEFAULT	1353	/* ~140 nits */

struct hk3_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_connector *connector;
	struct gpio_desc *reset_gpio;
	struct regulator *vddi;
	struct regulator *vci;
	struct regulator *vddd;
	bool vddd_on;
};

/* WQHD PPS: DSC 1.1, two 672-pixel slices, 8 bpp. */
static const struct drm_dsc_config hk3_wqhd_dsc = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 672,
	.slice_height = 187,
	.slice_count = 2,
	.simple_422 = false,
	.pic_width = 1344,
	.pic_height = 2992,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 592,
	.block_pred_enable = true,
	.first_line_bpg_offset = 12,
	.initial_offset = 6144,
	.rc_buf_thresh = {
		14, 28, 42, 56, 70, 84, 98, 105,
		112, 119, 121, 123, 125, 126
	},
	.rc_range_params = {
		{ 0, 4, 2 }, { 0, 4, 0 }, { 1, 5, 0 }, { 1, 6, 62 },
		{ 3, 7, 60 }, { 3, 7, 58 }, { 3, 7, 56 }, { 3, 8, 56 },
		{ 3, 9, 56 }, { 3, 10, 54 }, { 5, 11, 54 }, { 5, 12, 52 },
		{ 5, 13, 52 }, { 7, 13, 52 }, { 13, 15, 52 }
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 9,
	.scale_increment_interval = 5177,
	.nfl_bpg_offset = 133,
	.slice_bpg_offset = 112,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 672,
	.dsc_version_minor = 1,
	.dsc_version_major = 1,
};

#define HK3_MODE(hz, hbp, clk, flags)					\
	{								\
		.clock = (clk),						\
		.hdisplay = 1344,					\
		.hsync_start = 1344 + 80,				\
		.hsync_end = 1344 + 80 + 24,				\
		.htotal = 1344 + 80 + 24 + (hbp),			\
		.vdisplay = 2992,					\
		.vsync_start = 2992 + 12,				\
		.vsync_end = 2992 + 12 + 4,				\
		.vtotal = 2992 + 12 + 4 + 22,				\
		.width_mm = 70,						\
		.height_mm = 155,					\
		.type = DRM_MODE_TYPE_DRIVER | (flags),			\
	}

/*
 * 120 Hz is preferred, so compositors default to it. The bootloader leaves
 * the panel at 60 Hz, but hk3_enable() always resets and reinitialises the
 * panel at the rate of the mode being set. Measured on hardware from the TE
 * interrupt: 120.4 Hz and 60.2 Hz.
 */
static const struct drm_display_mode hk3_modes[] = {
	HK3_MODE(120, 42, 541764, DRM_MODE_TYPE_PREFERRED),
	HK3_MODE(60, 42, 270882, 0),
};

static inline struct hk3_panel *to_hk3(struct drm_panel *panel)
{
	return container_of(panel, struct hk3_panel, panel);
}

static int hk3_vrefresh(struct hk3_panel *ctx)
{
	struct drm_connector *conn = ctx->connector;

	if (conn && conn->state && conn->state->crtc &&
	    conn->state->crtc->state)
		return drm_mode_vrefresh(&conn->state->crtc->state->adjusted_mode);
	return 60;
}

static int hk3_prepare(struct drm_panel *panel)
{
	struct hk3_panel *ctx = to_hk3(panel);
	int ret;

	/* reg_ctrl_enable: VDDI +1 ms, VCI +10 ms. */
	ret = regulator_enable(ctx->vddi);
	if (ret)
		return ret;
	usleep_range(1000, 1100);
	ret = regulator_enable(ctx->vci);
	if (ret) {
		regulator_disable(ctx->vddi);
		return ret;
	}
	usleep_range(10000, 11000);
	return 0;
}

static int hk3_unprepare(struct drm_panel *panel)
{
	struct hk3_panel *ctx = to_hk3(panel);

	if (ctx->vddd_on) {
		regulator_disable(ctx->vddd);
		ctx->vddd_on = false;
		usleep_range(1000, 1100);
	}
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	regulator_disable(ctx->vci);
	usleep_range(1000, 1100);
	regulator_disable(ctx->vddi);
	return 0;
}

#define hk3_cmd(dsi, seq...) do {					\
	ret = mipi_dsi_dcs_write_buffer((dsi), (const u8[]){ seq },	\
					sizeof((const u8[]){ seq }));	\
	if (ret < 0)							\
		goto fail;						\
} while (0)

#define HK3_UNLOCK(dsi)	hk3_cmd(dsi, 0xF0, 0x5A, 0x5A)
#define HK3_LOCK(dsi)	hk3_cmd(dsi, 0xF0, 0xA5, 0xA5)
#define HK3_FREQ_UPDATE(dsi) hk3_cmd(dsi, 0xF7, 0x0F)

static int hk3_enable(struct drm_panel *panel)
{
	struct hk3_panel *ctx = to_hk3(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct drm_dsc_picture_parameter_set pps;
	int vrefresh = hk3_vrefresh(ctx);
	u8 power = 0;
	int ret;

	if (ctx->vddd_on)
		return -EBUSY;

	/* reset_timing_ms = {1, 1, 5}, then reg_ctrl_post_enable: VDDD +1 ms */
	gpiod_direction_output(ctx->reset_gpio, 1);
	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	ret = regulator_enable(ctx->vddd);
	if (ret)
		goto fail_reset;
	ctx->vddd_on = true;
	usleep_range(1000, 1100);

	/* DSC: compression on, then the PPS. */
	hk3_cmd(dsi, 0x9D, 0x01);
	drm_dsc_pps_payload_pack(&pps, &hk3_wqhd_dsc);
	ret = mipi_dsi_picture_parameter_set(dsi, &pps);
	if (ret < 0)
		goto fail;

	/* hk3_init_cmds (MP panel revision) */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		goto fail;
	usleep_range(10000, 11000);

	HK3_UNLOCK(dsi);
	hk3_cmd(dsi, 0xB0, 0x00, 0x4F, 0xF4);	/* AMP type change */
	hk3_cmd(dsi, 0xF4, 0x50);
	hk3_cmd(dsi, 0xB0, 0x00, 0x31, 0xF4);	/* VREG 4.5 V */
	hk3_cmd(dsi, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x00);
	HK3_LOCK(dsi);
	msleep(110);
	hk3_cmd(dsi, MIPI_DCS_SET_TEAR_ON);

	HK3_UNLOCK(dsi);
	hk3_cmd(dsi, 0xB0, 0x00, 0x3C, 0xB9);	/* TSP sync enable */
	hk3_cmd(dsi, 0xB9, 0x19, 0x09);
	hk3_cmd(dsi, 0xB0, 0x00, 0x36, 0xC5);	/* FFC off, 1368 Mbps */
	hk3_cmd(dsi, 0xC5, 0x10, 0x10, 0x50, 0x05, 0x4D, 0x31, 0x40, 0x00,
		0x40, 0x00, 0x40, 0x00, 0x4D, 0x31, 0x40, 0x00,
		0x40, 0x00, 0x40, 0x00, 0x4D, 0x31, 0x40, 0x00,
		0x40, 0x00, 0x40, 0x00, 0x4D, 0x31, 0x40, 0x00,
		0x40, 0x00, 0x40, 0x00);
	hk3_cmd(dsi, 0xB0, 0x00, 0x04, 0xB9);	/* TE width */
	hk3_cmd(dsi, 0xB9, 0x0B, 0xBB, 0x00, 0x2F,
		0x0B, 0xBB, 0x00, 0x2F, 0x0B, 0xBB, 0x00, 0x2F);
	hk3_cmd(dsi, 0xB0, 0x00, 0x18, 0xB1);	/* PMIC fast discharge off */
	hk3_cmd(dsi, 0xB1, 0x55, 0x01);
	hk3_cmd(dsi, 0xB0, 0x00, 0x13, 0xB1);
	hk3_cmd(dsi, 0xB1, 0x80);
	HK3_FREQ_UPDATE(dsi);
	HK3_LOCK(dsi);
	hk3_cmd(dsi, MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x05, 0x3F);
	hk3_cmd(dsi, MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x0B, 0xAF);

	/* hk3_negative_field_setting (DVT1 and later) */
	HK3_UNLOCK(dsi);
	hk3_cmd(dsi, 0xB0, 0x00, 0x21, 0xF4);
	hk3_cmd(dsi, 0xF4, 0x1E);
	hk3_cmd(dsi, 0xB0, 0x00, 0x69, 0xF4);
	hk3_cmd(dsi, 0xF4, 0x78);
	hk3_cmd(dsi, 0xB0, 0x00, 0x17, 0xF4);
	hk3_cmd(dsi, 0xF4, 0x1E);
	HK3_FREQ_UPDATE(dsi);
	HK3_LOCK(dsi);

	/* WQHD, 8-bit */
	HK3_UNLOCK(dsi);
	hk3_cmd(dsi, 0xC3, 0x0C);
	hk3_cmd(dsi, 0xB0, 0x00, 0x01, 0xF2);
	hk3_cmd(dsi, 0xF2, 0x01);
	HK3_LOCK(dsi);

	/*
	 * hk3_set_panel_feat(enforce) for HS, early-exit off, HBM off, manual
	 * frame insertion: changeable TE, HS op mode, then the fixed rate.
	 */
	HK3_UNLOCK(dsi);
	hk3_cmd(dsi, 0xB9, 0x04);
	hk3_cmd(dsi, 0xB0, 0x00, 0x04, 0xB9);
	hk3_cmd(dsi, 0xB9, 0x0B, 0xBB, 0x00, 0x2F);
	hk3_cmd(dsi, 0xF2, 0x01);
	hk3_cmd(dsi, 0x60, 0x00);
	hk3_cmd(dsi, 0xBD, 0x21, 0x81, 0x83, 0x03, 0x03);
	hk3_cmd(dsi, 0xB0, 0x00, 0x10, 0xBD);
	hk3_cmd(dsi, 0xBD, 0x00);
	hk3_cmd(dsi, 0xB0, 0x00, 0x82, 0xBD);
	hk3_cmd(dsi, 0xBD, 0x00, 0x00, 0x00, 0x00);
	hk3_cmd(dsi, 0xB0, 0x00, 0x1E, 0xBD);
	hk3_cmd(dsi, 0xBD, 0x00, 0x00, 0x00, 0x02, 0x00, 0x06, 0x00, 0x16,
		0x00, 0x2E, 0x00, 0xEE);
	hk3_cmd(dsi, 0xBD, 0x21);
	hk3_cmd(dsi, 0x60, vrefresh == 120 ? 0x00 : 0x01);
	HK3_FREQ_UPDATE(dsi);
	HK3_LOCK(dsi);

	/* WRCTRLD: brightness control on */
	hk3_cmd(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	/* hk3_display_on_cmds (DVT1+: Vreg 7.1 V) */
	msleep(20);
	HK3_UNLOCK(dsi);
	hk3_cmd(dsi, 0xE4, 0x00, 0x2C, 0x2C, 0xA2, 0x00, 0x00);
	hk3_cmd(dsi, 0xB0, 0x00, 0x4F, 0xF4);
	hk3_cmd(dsi, 0xF4, 0x70);
	hk3_cmd(dsi, 0xB0, 0x00, 0x31, 0xF4);
	hk3_cmd(dsi, 0xF4, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A);
	hk3_cmd(dsi, 0xE4, 0x00, 0x2C, 0x2C, 0x82, 0x00, 0x00);
	HK3_LOCK(dsi);
	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		goto fail;

	if (mipi_dsi_dcs_get_power_mode(dsi, &power) >= 0)
		dev_info(&dsi->dev, "HK3 on at %d Hz, power mode %#x\n",
			 vrefresh, power);
	return 0;

fail:
	dev_err(&dsi->dev, "HK3 init failed: %d\n", ret);
	regulator_disable(ctx->vddd);
	ctx->vddd_on = false;
fail_reset:
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	return ret;
}

static int hk3_disable(struct drm_panel *panel)
{
	struct hk3_panel *ctx = to_hk3(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		goto fail;
	/* hk3_display_off_cmds: back to Vreg 4.5 V */
	HK3_UNLOCK(dsi);
	hk3_cmd(dsi, 0xE4, 0x00, 0x2C, 0x2C, 0xA2, 0x00, 0x00);
	hk3_cmd(dsi, 0xB0, 0x00, 0x4F, 0xF4);
	hk3_cmd(dsi, 0xF4, 0x50);
	hk3_cmd(dsi, 0xB0, 0x00, 0x31, 0xF4);
	hk3_cmd(dsi, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x00);
	hk3_cmd(dsi, 0xE4, 0x00, 0x2C, 0x2C, 0x82, 0x00, 0x00);
	HK3_LOCK(dsi);
	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	msleep(120);
fail:
	/* unprepare removes the panel's power either way; report a lost command */
	if (ret < 0)
		dev_warn(&dsi->dev, "HK3 display-off sequence failed: %d\n", ret);
	else
		dev_info(&dsi->dev, "HK3 off\n");
	return ret < 0 ? ret : 0;
}

static int hk3_get_modes(struct drm_panel *panel,
			 struct drm_connector *connector)
{
	struct hk3_panel *ctx = to_hk3(panel);
	unsigned int i;

	ctx->connector = connector;
	for (i = 0; i < ARRAY_SIZE(hk3_modes); i++) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, &hk3_modes[i]);
		if (!mode)
			return i ? i : -ENOMEM;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}
	connector->display_info.width_mm = hk3_modes[0].width_mm;
	connector->display_info.height_mm = hk3_modes[0].height_mm;
	return ARRAY_SIZE(hk3_modes);
}

static const struct drm_panel_funcs hk3_panel_funcs = {
	.prepare = hk3_prepare,
	.unprepare = hk3_unprepare,
	.enable = hk3_enable,
	.disable = hk3_disable,
	.get_modes = hk3_get_modes,
};

static int hk3_bl_update_status(struct backlight_device *bl)
{
	struct hk3_panel *ctx = bl_get_data(bl);
	u16 br = backlight_get_brightness(bl);

	if (!ctx->panel.enabled)
		return 0;
	return mipi_dsi_dcs_set_display_brightness_large(ctx->dsi, br);
}

static const struct backlight_ops hk3_bl_ops = {
	.update_status = hk3_bl_update_status,
};

static int hk3_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = HK3_BRIGHTNESS_DEFAULT,
		.max_brightness = HK3_BRIGHTNESS_MAX,
	};
	struct backlight_device *bl;
	struct hk3_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct hk3_panel, panel,
				   &hk3_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "reset gpio\n");
	ctx->vddi = devm_regulator_get(dev, "vddi");
	if (IS_ERR(ctx->vddi))
		return dev_err_probe(dev, PTR_ERR(ctx->vddi), "VDDI\n");
	ctx->vci = devm_regulator_get(dev, "vci");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci), "VCI\n");
	ctx->vddd = devm_regulator_get(dev, "vddd");
	if (IS_ERR(ctx->vddd))
		return dev_err_probe(dev, PTR_ERR(ctx->vddd), "VDDD\n");

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->hs_rate = HK3_HS_CLK_MBPS * 1000000UL;
	dsi->dsc = (struct drm_dsc_config *)&hk3_wqhd_dsc;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;
	ctx->panel.prepare_prev_first = true;

	bl = devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					    &hk3_bl_ops, &props);
	if (IS_ERR(bl))
		return dev_err_probe(dev, PTR_ERR(bl), "backlight\n");
	ctx->panel.backlight = bl;

	drm_panel_add(&ctx->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "DSI attach\n");
	}
	return 0;
}

static void hk3_remove(struct mipi_dsi_device *dsi)
{
	struct hk3_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id hk3_of_match[] = {
	{ .compatible = "google,hk3" },
	{ }
};
MODULE_DEVICE_TABLE(of, hk3_of_match);

static struct mipi_dsi_driver hk3_driver = {
	.probe = hk3_probe,
	.remove = hk3_remove,
	.driver = {
		.name = "panel-google-hk3",
		.of_match_table = hk3_of_match,
	},
};
module_mipi_dsi_driver(hk3_driver);

MODULE_DESCRIPTION("Google Pixel 8 Pro HK3 panel driver");
MODULE_LICENSE("GPL");
