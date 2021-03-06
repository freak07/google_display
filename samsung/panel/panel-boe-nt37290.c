// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based nt37290 AMOLED LCD panel driver.
 *
 * Copyright (c) 2021 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <trace/dpu_trace.h>
#include <video/mipi_display.h>

#include "panel-samsung-drv.h"

/* when refresh rate can go lower than this value (in auto mode), fixed TE2 should be enabled */
#define NT37290_TE2_MIN_RATE   30
#define NT37290_TE2_CHANGEABLE 0x02
#define NT37290_TE2_FIXED      0x22

/**
 * enum nt37290_panel_feature - features supported by this panel
 * @G10_FEAT_EARLY_EXIT: early exit from a long frame
 * @G10_FEAT_FRAME_AUTO: automatic (not manual) frame control
 * @G10_FEAT_MAX: placeholder, counter for number of features
 *
 * The following features are correlated, if one or more of them change, the others need
 * to be updated unconditionally.
 */
enum nt37290_panel_feature {
	G10_FEAT_EARLY_EXIT = 0,
	G10_FEAT_FRAME_AUTO,
	G10_FEAT_MAX,
};

/**
 * struct nt37290_panel - panel specific runtime info
 *
 * This struct maintains nt37290 panel specific runtime info, any fixed details about panel
 * should most likely go into struct exynos_panel_desc. The variables with the prefix hw_ keep
 * track of the features that were actually committed to hardware, and should be modified
 * after sending cmds to panel, i.e. updating hw state.
 */
struct nt37290_panel {
	/** @base: base panel struct */
	struct exynos_panel base;
	/** @feat: software/working correlated features, not guaranteed to be effective in panel */
	DECLARE_BITMAP(feat, G10_FEAT_MAX);
	/** @hw_feat: correlated states effective in panel */
	DECLARE_BITMAP(hw_feat, G10_FEAT_MAX);
	/** @hw_vrefresh: vrefresh rate effective in panel */
	int hw_vrefresh;
	/** @hw_idle_vrefresh: idle vrefresh rate effective in panel */
	int hw_idle_vrefresh;
	/**
	 * @auto_mode_vrefresh: indicates current minimum refresh rate while in auto mode,
	 *                      if 0 it means that auto mode is not enabled
	 */
	u32 auto_mode_vrefresh;
	/**
	 * @delayed_idle: indicates idle mode set is delayed due to idle_delay_ms,
	 *                we should avoid changing idle_mode when it's true
	 */
	bool delayed_idle;
};

#define to_spanel(ctx) container_of(ctx, struct nt37290_panel, base)

static const u8 display_off[] = { 0x28 };
static const u8 display_on[] = { 0x29 };
static const u8 cmd2_page0[] = { 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00 };
static const u8 stream_2c[] = { 0x2C };

static const struct exynos_dsi_cmd nt37290_lp_cmds[] = {
	/* enter AOD */
	EXYNOS_DSI_CMD_SEQ(0x39),
	/* manual mode (no frame skip) */
	EXYNOS_DSI_CMD_SEQ(0x2F, 0x00),
};
static DEFINE_EXYNOS_CMD_SET(nt37290_lp);

static const struct exynos_dsi_cmd nt37290_lp_off_cmds[] = {
	EXYNOS_DSI_CMD0(display_off),
};

static const struct exynos_dsi_cmd nt37290_lp_low_cmds[] = {
	/* 10 nit */
	EXYNOS_DSI_CMD_SEQ_DELAY(9, 0x51, 0x00, 0x00, 0x00, 0x00, 0x03, 0x33),
	/* 2Ch needs to be sent twice in next 2 vsync */
	EXYNOS_DSI_CMD(stream_2c, 9),
	EXYNOS_DSI_CMD0(stream_2c),
	EXYNOS_DSI_CMD0(display_on),
};

static const struct exynos_dsi_cmd nt37290_lp_high_cmds[] = {
	/* 50 nit */
	EXYNOS_DSI_CMD_SEQ_DELAY(9, 0x51, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFE),
	/* 2Ch needs to be sent twice in next 2 vsync */
	EXYNOS_DSI_CMD(stream_2c, 9),
	EXYNOS_DSI_CMD0(stream_2c),
	EXYNOS_DSI_CMD0(display_on),
};

static const struct exynos_binned_lp nt37290_binned_lp[] = {
	BINNED_LP_MODE("off", 0, nt37290_lp_off_cmds),
	/* rising = 0, falling = 48 */
	BINNED_LP_MODE_TIMING("low", 80, nt37290_lp_low_cmds, 0, 48),
	BINNED_LP_MODE_TIMING("high", 2047, nt37290_lp_high_cmds, 0, 48),
};

static const struct exynos_dsi_cmd nt37290_off_cmds[] = {
	EXYNOS_DSI_CMD(display_off, 100),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, 0x10),
};
static DEFINE_EXYNOS_CMD_SET(nt37290_off);

static const struct exynos_dsi_cmd nt37290_lhbm_on_setting_cmds[] = {
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x07),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xC0, 0xB1),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x08),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xC0, 0x55),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xD5, 0x21, 0x00, 0x39, 0x31, 0x39,
				0x31, 0x00, 0x00, 0x3F, 0xC9, 0xEF, 0xAE, 0x3F, 0xC9, 0xEF, 0xAE,
				0x00, 0x0C, 0xC6, 0xDB, 0x61, 0x23, 0x00, 0x00, 0x79, 0x00, 0x00,
				0x79, 0x33, 0xF0, 0x87, 0x87, 0x39, 0x31, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xD6, 0x27, 0x00, 0x39, 0x31, 0x39,
				0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xC9, 0xEF, 0xAE,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x7A, 0xF3, 0x00, 0x00,
				0x79, 0x33, 0x30, 0x79, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xD7, 0x2B, 0x00, 0x39, 0x31, 0x39,
				0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x7F, 0xF3, 0x39, 0x24, 0x9F, 0x55, 0x00, 0x7A, 0xF3, 0x00, 0x7A,
				0xF3, 0x33, 0x0F, 0x79, 0x79, 0xC6, 0xCF, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xD8, 0x2D, 0x00, 0x39, 0x31, 0x39,
				0x31, 0x00, 0x00, 0x3F, 0xC9, 0xEF, 0xAE, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x79, 0x00, 0x7A,
				0xF3, 0x33, 0xC0, 0x87, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD0_REV(cmd2_page0, PANEL_REV_GE(PANEL_REV_EVT1)),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x05),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x02),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x13),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00, 0x7A, 0x00, 0x7A),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x1B),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x1F),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00, 0xF3, 0x00, 0xF3),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x2B),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x3F, 0xFF, 0x3F, 0xFF, 0x3F,
				0xFF),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x31),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x22),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x32),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x2A),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x33),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x2A),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x34),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x16),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x35),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x36),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x02),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x37),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x38),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x0C, 0x38),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x3A),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x01, 0x1F, 0x00, 0x61, 0x00,
				0x93),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x40),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00, 0xF8, 0x01, 0x07, 0x00,
				0x2E),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x46),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x00, 0x99, 0x00, 0x29, 0x00,
				0x88),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x4C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x1F, 0xFC, 0x1F, 0xFC, 0x1F,
				0xFC),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x52),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x0A, 0x99, 0x22, 0xDA, 0x3E,
				0xB5),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x58),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x3D, 0xDC, 0x28, 0xD5, 0x1D,
				0x52),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x5E),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x13, 0x51, 0x13, 0xCD, 0x0D,
				0x4E),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x64),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x3B, 0x3F, 0x2E, 0x39, 0x35,
				0xF2),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x6A),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x25, 0x35, 0x18, 0x3C, 0x30,
				0xCF),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x70),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x3E, 0xD6, 0x03, 0xE4, 0x3F,
				0xF5),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x76),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x23, 0x19, 0x1C, 0x89, 0x37,
				0x4B),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x7C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x3F, 0x69, 0x0A, 0xC7, 0x3C,
				0xB5),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x82),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x13, 0x61, 0x1E, 0x2E, 0x03,
				0xA9),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x88),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xDF, 0x40),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x87, 0x07, 0x5E),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x03),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x87, 0x07, 0x5E),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x6F, 0x05),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x87, 0x07, 0x5E, 0x07, 0x5E, 0x07,
				0x5E, 0x07, 0x5E, 0x07, 0x5E, 0x07, 0x5E, 0x07, 0x5E, 0x07, 0x5E),

	EXYNOS_DSI_CMD_SEQ(0x88, 0x01), /* enable */
	/* circle center: x=720, y=2361 */
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x01),
	EXYNOS_DSI_CMD_SEQ(0x88, 0x02, 0xD0, 0x09, 0x39),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x15),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x0A, 0x86),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x17),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x0F, 0xFF),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x19),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x01, 0x4F, 0x06, 0x45, 0x0B, 0x98, 0x01, 0x96, 0x08, 0x19, 0x0A,
					 0xFD, 0x01, 0x55, 0x05, 0x84),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x3D),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x01, 0x4A),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x3F),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x08, 0xBB),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x41),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x08, 0xF4, 0x0C, 0xAB, 0x00, 0xD4, 0x08, 0x80, 0x09, 0x91, 0x0A,
					 0x87, 0x04, 0x1D, 0x0B, 0x9C),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x65),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x07, 0x68),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x67),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x01, 0x1C),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x69),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x0B, 0x3C, 0x0D, 0x16, 0x04, 0x32, 0x07, 0x83, 0x0D, 0x92, 0x0C,
					 0x87, 0x07, 0x4B, 0x07, 0x18),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x29),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x09, 0xBE),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x2B),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x0D, 0x95),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x2D),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x0E, 0x45, 0x07, 0xCE, 0x04, 0x18, 0x03, 0x47, 0x0B, 0x52, 0x00,
					 0x7C, 0x0D, 0x90, 0x0A, 0x8B),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x51),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x02, 0x10),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x53),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x07, 0x9D),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x55),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x01, 0x11, 0x04, 0x28, 0x00, 0xF0, 0x0B, 0x8C, 0x0C, 0xC0, 0x04,
					 0x0F, 0x05, 0x1F, 0x0E, 0x89),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x79),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x07, 0x8C),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x7B),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x0C, 0xE2),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x7D),
	EXYNOS_DSI_CMD_SEQ(0x87, 0x09, 0x08, 0x02, 0xF9, 0x01, 0x08, 0x0D, 0x17, 0x04, 0x6B, 0x00,
					 0xD0, 0x04, 0x77, 0x05, 0x7D),

	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x51, 0x3F, 0xFF),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x53, 0x20),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xFF, 0xAA, 0x55, 0xA5, 0x84),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x7C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xF3, 0x01),
};
static DEFINE_EXYNOS_CMD_SET(nt37290_lhbm_on_setting);

static const struct exynos_dsi_cmd nt37290_init_cmds[] = {
	/* CMD1 */
	/* set for higher MIPI speed: 1346Mbps */
	EXYNOS_DSI_CMD_SEQ(0x1F, 0xF0),
	/* gamma curve */
	EXYNOS_DSI_CMD_SEQ(0x26, 0x00),
	/* row address */
	EXYNOS_DSI_CMD_SEQ(0x2B, 0x00, 0x00, 0x0C, 0x2F),
	/* TE output line */
	EXYNOS_DSI_CMD_SEQ(0x35),
	/* select brightness value */
	EXYNOS_DSI_CMD_SEQ(0x51, 0x03, 0xF8, 0x03, 0xF8, 0x0F, 0xFE),
	/* control brightness */
	EXYNOS_DSI_CMD_SEQ(0x53, 0x20),
	EXYNOS_DSI_CMD_SEQ(0x5A, 0x01),
	/* DSC: slice 24, 2 decoder */
	EXYNOS_DSI_CMD_SEQ(0x90, 0x03, 0x03),
	EXYNOS_DSI_CMD_SEQ(0x91, 0x89, 0x28, 0x00, 0x18, 0xD2, 0x00, 0x02,
			   0x86, 0x02, 0x83, 0x00, 0x0A, 0x04, 0x86, 0x03,
			   0x2E, 0x10, 0xF0),
	/* change refresh frame to 1 after 2Ch command in skip mode */
	EXYNOS_DSI_CMD0(cmd2_page0),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00),

	/* CMD2 Page 1 */
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	EXYNOS_DSI_CMD_SEQ(0xC5, 0x00, 0x0B, 0x0B, 0x0B),

	/* CMD3 Page 0 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x80),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x1B),
	EXYNOS_DSI_CMD_SEQ(0xF4, 0x55),
	/* CMD3 Page 1 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x12),
	EXYNOS_DSI_CMD_SEQ(0xF5, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x09),
	EXYNOS_DSI_CMD_SEQ(0xF9, 0x10),
	/* CMD3 Page 3 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x83),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x14),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x0D),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x01),
	EXYNOS_DSI_CMD_SEQ(0xF9, 0x06),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x01),
	EXYNOS_DSI_CMD_SEQ(0xFA, 0x06),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x01),
	EXYNOS_DSI_CMD_SEQ(0xFB, 0x06),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x01),
	EXYNOS_DSI_CMD_SEQ(0xFC, 0x06),
	/* CMD3 Page 4 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x84),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x1C),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x3A),

	EXYNOS_DSI_CMD_SEQ_DELAY(120, 0x11),
};
static DEFINE_EXYNOS_CMD_SET(nt37290_init);

static u8 nt37290_get_te2_option(struct exynos_panel *ctx)
{
	struct nt37290_panel *spanel = to_spanel(ctx);

	if (!ctx || !ctx->current_mode)
		return NT37290_TE2_CHANGEABLE;

	/* AOD mode only supports fixed TE2 */
	if (ctx->current_mode->exynos_mode.is_lp_mode ||
	    (spanel->hw_idle_vrefresh > 0 && spanel->hw_idle_vrefresh < NT37290_TE2_MIN_RATE))
		return NT37290_TE2_FIXED;

	return NT37290_TE2_CHANGEABLE;
}

static void nt37290_update_te2(struct exynos_panel *ctx)
{
	struct nt37290_panel *spanel = to_spanel(ctx);
	struct exynos_panel_te2_timing timing;
	/* default timing */
	u8 rising = 0, falling = 0x30;
	u8 option = nt37290_get_te2_option(ctx);
	int ret;

	if (!ctx)
		return;

	ret = exynos_panel_get_current_mode_te2(ctx, &timing);
	if (!ret) {
		rising = timing.rising_edge & 0xFF;
		falling = timing.falling_edge & 0xFF;
	} else if (ret == -EAGAIN) {
		dev_dbg(ctx->dev, "Panel is not ready, use default timing\n");
	} else {
		dev_warn(ctx->dev, "Failed to get current timing\n");
		return;
	}

	/* option */
	EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x03);
	EXYNOS_DCS_BUF_ADD(ctx, 0xC3, option);
	EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x04);
	EXYNOS_DCS_BUF_ADD(ctx, 0xC3, option);
	/* timing */
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xC4, 0x00, 0x00, 0x00, 0x00,
				     0x00, rising, 0x10, falling);

	dev_dbg(ctx->dev,
		"TE2 updated: option %s, idle mode %s, rising 0x%x, falling 0x%x\n",
		(option == NT37290_TE2_CHANGEABLE) ? "changeable" : "fixed",
		spanel->hw_idle_vrefresh ? "enabled" : "disabled",
		rising, falling);
}

static inline bool is_auto_mode_allowed(struct exynos_panel *ctx)
{
	/* don't want to enable auto mode/early exit during hbm or dimming on */
	if (IS_HBM_ON(ctx->hbm_mode) || ctx->dimming_on)
		return false;

	return ctx->panel_idle_enabled;
}

static void nt37290_update_min_idle_vrefresh(struct exynos_panel *ctx,
					     const struct exynos_panel_mode *pmode)
{
	struct nt37290_panel *spanel = to_spanel(ctx);
	const int vrefresh = drm_mode_vrefresh(&pmode->mode);
	int idle_vrefresh = ctx->min_vrefresh;

	if (!idle_vrefresh || !is_auto_mode_allowed(ctx) ||
	    pmode->idle_mode == IDLE_MODE_UNSUPPORTED)
		idle_vrefresh = 0;
	else if (idle_vrefresh <= 10)
		idle_vrefresh = 10;
	else if (idle_vrefresh <= 30)
		idle_vrefresh = 30;
	else if (idle_vrefresh <= 60)
		idle_vrefresh = 60;
	else /* 120hz: no idle available */
		idle_vrefresh = 0;

	if (idle_vrefresh >= vrefresh) {
		dev_dbg(ctx->dev, "idle vrefresh (%d) higher than target (%d)\n",
			idle_vrefresh, vrefresh);
		idle_vrefresh = 0;
	}

	if (idle_vrefresh && ctx->idle_delay_ms &&
	    (panel_get_idle_time_delta(ctx) < ctx->idle_delay_ms)) {
		spanel->delayed_idle = true;
		idle_vrefresh = 0;
	} else {
		spanel->delayed_idle = false;
	}

	spanel->auto_mode_vrefresh = idle_vrefresh;
}

static bool nt37290_update_panel_feat(struct exynos_panel *ctx,
				      const struct exynos_panel_mode *pmode, bool enforce)
{
	struct nt37290_panel *spanel = to_spanel(ctx);
	int vrefresh;
	int idle_vrefresh = spanel->auto_mode_vrefresh;
	DECLARE_BITMAP(changed_feat, G10_FEAT_MAX);
	bool ee, fi;

	if (pmode)
		vrefresh = drm_mode_vrefresh(&pmode->mode);
	else
		vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);

	/* when panel feat func is called, idle effect should be disabled */
	ctx->panel_idle_vrefresh = 0;

	if (enforce) {
		bitmap_fill(changed_feat, G10_FEAT_MAX);
	} else {
		bitmap_xor(changed_feat, spanel->feat, spanel->hw_feat, G10_FEAT_MAX);
		if (bitmap_empty(changed_feat, G10_FEAT_MAX) &&
		    vrefresh == spanel->hw_vrefresh &&
		    idle_vrefresh == spanel->hw_idle_vrefresh)
			return false;
	}

	spanel->hw_vrefresh = vrefresh;
	spanel->hw_idle_vrefresh = idle_vrefresh;
	bitmap_copy(spanel->hw_feat, spanel->feat, G10_FEAT_MAX);
	ee = test_bit(G10_FEAT_EARLY_EXIT, spanel->feat);
	fi = test_bit(G10_FEAT_FRAME_AUTO, spanel->feat);

	dev_dbg(ctx->dev, "ee=%s fi=%s vrefresh=%d idle_vrefresh=%d\n",
		ee ? "on" : "off", fi ? "auto" : "manual", vrefresh, idle_vrefresh);

	DPU_ATRACE_BEGIN(__func__);

	if (vrefresh == 120 && !fi) {
		/* freq_mode_hs */
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
		/* restore TE timing (no shift) */
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x44, 0x00, 0x00);
	} else {
		/* freq_mode_hs */
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
		/* freq_ctrl_hs */
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
		/* early exit */
		EXYNOS_DCS_BUF_ADD(ctx, 0x5A, !ee);

		/* set auto frame insertion */
		EXYNOS_DCS_BUF_ADD_SET(ctx, cmd2_page0);
		EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x1C);
		if (!fi) {
			/* auto frame insertion off (manual) */
			if (vrefresh == 60)
				EXYNOS_DCS_BUF_ADD(ctx,
					0xBA, 0x91, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00);
			else
				dev_warn(ctx->dev,
					 "Unsupported vrefresh %dHz for manual mode\n", vrefresh);
		} else {
			/* auto frame insertion on */
			if (idle_vrefresh == 10)
				EXYNOS_DCS_BUF_ADD(ctx,
					0xBA, 0x93, 0x09, 0x03, 0x00, 0x11, 0x0B, 0x0B,
					0x00, 0x06);
			else if (idle_vrefresh == 30)
				EXYNOS_DCS_BUF_ADD(ctx,
					0xBA, 0x93, 0x03, 0x02, 0x00, 0x11, 0x03, 0x03,
					0x00, 0x04);
			else if (idle_vrefresh == 60)
				EXYNOS_DCS_BUF_ADD(ctx,
					0xBA, 0x93, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
					0x00, 0x00);
			else
				dev_warn(ctx->dev,
					 "Unsupported idle_vrefresh %dHz for auto mode\n",
					 idle_vrefresh);
		}

		EXYNOS_DCS_BUF_ADD(ctx, 0x2C);

		if (vrefresh == 120)
			/* restore TE timing (no shift) */
			EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x44, 0x00, 0x00);
		else
			/* TE shift 8.2ms */
			EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x44, 0x00, 0x01);
	}

	DPU_ATRACE_END(__func__);

	return true;
}

static bool nt37290_change_frequency(struct exynos_panel *ctx,
				     const struct exynos_panel_mode *pmode)
{
	struct nt37290_panel *spanel = to_spanel(ctx);
	int vrefresh = drm_mode_vrefresh(&pmode->mode);
	bool idle_active = false;
	bool was_lp_mode = ctx->current_mode->exynos_mode.is_lp_mode;
	bool updated;

	nt37290_update_min_idle_vrefresh(ctx, pmode);

	if (spanel->auto_mode_vrefresh &&
	    (pmode->idle_mode == IDLE_MODE_ON_INACTIVITY ||
	    (pmode->idle_mode == IDLE_MODE_ON_SELF_REFRESH && ctx->self_refresh_active)))
		idle_active = true;

	if (idle_active) {
		set_bit(G10_FEAT_EARLY_EXIT, spanel->feat);
		set_bit(G10_FEAT_FRAME_AUTO, spanel->feat);
	} else {
		clear_bit(G10_FEAT_EARLY_EXIT, spanel->feat);
		clear_bit(G10_FEAT_FRAME_AUTO, spanel->feat);
	}

	/* need to send 2Fh command while exiting AOD */
	updated = nt37290_update_panel_feat(ctx, pmode, was_lp_mode);

	ctx->panel_idle_vrefresh = ctx->self_refresh_active ? spanel->hw_idle_vrefresh : 0;

	if (updated) {
		backlight_state_changed(ctx->bl);
		dev_dbg(ctx->dev, "change to %dHz, idle %s, was_lp_mode %d\n",
			vrefresh, idle_active ? "active" : "deactive", was_lp_mode);
	}

	return updated;
}

static bool nt37290_set_self_refresh(struct exynos_panel *ctx, bool enable)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	bool updated;

	if (unlikely(!pmode))
		return false;

	/* self refresh is not supported in lp mode since that always makes use of early exit */
	if (pmode->exynos_mode.is_lp_mode)
		return false;

	DPU_ATRACE_BEGIN(__func__);

	updated = nt37290_change_frequency(ctx, pmode);

	if (pmode->idle_mode == IDLE_MODE_ON_SELF_REFRESH) {
		dev_dbg(ctx->dev, "%s: %s idle (%dHz) for mode %s\n",
			__func__, enable ? "enter" : "exit",
			ctx->panel_idle_vrefresh ? : drm_mode_vrefresh(&pmode->mode),
			pmode->mode.name);
	}

	DPU_ATRACE_END(__func__);

	return updated;
}

/**
 * 120hz auto mode takes at least 2 frames to start lowering refresh rate in addition to
 * time to next vblank. Use just over 2 frames time to consider worst case scenario
 */
#define EARLY_EXIT_THRESHOLD_US 17000
/**
 * Use a threshold to avoid disabling idle auto mode too frequently while continuously
 * updating frames. Considering the hibernation time for this scenario.
 */
#define IDLE_DELAY_THRESHOLD_US 50000

/**
 * nt37290_trigger_early_exit - trigger early exit command to panel
 * @ctx: panel struct
 *
 * Sends a command to panel to indicate a frame is about to come in case its been a while since
 * the last frame update and auto mode may have started to take effect and lowering refresh rate
 */
static void nt37290_trigger_early_exit(struct exynos_panel *ctx)
{
	const ktime_t delta = ktime_sub(ktime_get(), ctx->last_commit_ts);
	const s64 delta_us = ktime_to_us(delta);

	if (delta_us < EARLY_EXIT_THRESHOLD_US) {
		dev_dbg(ctx->dev, "skip early exit. %lldus since last commit\n",
			delta_us);
		return;
	}

	/* triggering early exit causes a switch to 120hz */
	ctx->last_mode_set_ts = ktime_get();

	DPU_ATRACE_BEGIN(__func__);

	if (ctx->idle_delay_ms && delta_us > IDLE_DELAY_THRESHOLD_US) {
		const struct exynos_panel_mode *pmode = ctx->current_mode;

		dev_dbg(ctx->dev, "%s: disable auto idle mode for %s\n",
			 __func__, pmode->mode.name);
		nt37290_change_frequency(ctx, pmode);
	} else {
		EXYNOS_DCS_WRITE_TABLE(ctx, stream_2c);
	}

	DPU_ATRACE_END(__func__);
}

static void nt37290_commit_done(struct exynos_panel *ctx)
{
	struct nt37290_panel *spanel = to_spanel(ctx);
	const struct exynos_panel_mode *pmode = ctx->current_mode;

	if (!is_panel_active(ctx) || !pmode)
		return;

	if (test_bit(G10_FEAT_EARLY_EXIT, spanel->feat))
		nt37290_trigger_early_exit(ctx);
	/**
	 * For IDLE_MODE_ON_INACTIVITY, we should go back to auto mode again
	 * after the delay time has elapsed.
	 */
	else if (pmode->idle_mode == IDLE_MODE_ON_INACTIVITY &&
		 spanel->delayed_idle)
		nt37290_change_frequency(ctx, pmode);
}

static void nt37290_set_nolp_mode(struct exynos_panel *ctx,
				  const struct exynos_panel_mode *pmode)
{
	if (!is_panel_active(ctx))
		return;

	/* exit AOD */
	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 34, 0x38);

	nt37290_change_frequency(ctx, pmode);

	/* 2Ch needs to be sent twice in next 2 vsync */
	EXYNOS_DCS_WRITE_TABLE_DELAY(ctx, 34, stream_2c);
	EXYNOS_DCS_WRITE_TABLE(ctx, stream_2c);
	EXYNOS_DCS_WRITE_TABLE(ctx, display_on);

	dev_info(ctx->dev, "exit LP mode\n");
}

static int nt37290_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(ctx->dev, "%s\n", __func__);

	exynos_panel_reset(ctx);
	exynos_panel_send_cmd_set(ctx, &nt37290_init_cmd_set);
	exynos_panel_send_cmd_set(ctx, &nt37290_lhbm_on_setting_cmd_set);

	nt37290_update_panel_feat(ctx, pmode, true);

	if (!pmode->exynos_mode.is_lp_mode)
		EXYNOS_DCS_WRITE_TABLE(ctx, display_on);
	else
		exynos_panel_set_lp_mode(ctx, pmode);

	return 0;
}

static int nt37290_disable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	struct nt37290_panel *spanel = to_spanel(ctx);

	/* panel register state gets reset after disabling hardware */
	bitmap_clear(spanel->hw_feat, 0, G10_FEAT_MAX);
	spanel->hw_vrefresh = 60;
	spanel->hw_idle_vrefresh = 0;

	return exynos_panel_disable(panel);
}

static int nt37290_set_brightness(struct exynos_panel *ctx, u16 br)
{
	if ((ctx->panel_rev >= PANEL_REV_EVT1) && ctx->hbm.local_hbm.enabled) {
		u16 level = br * 4;
		u8 val1 = level >> 8;
		u8 val2 = level & 0xff;

		/* LHBM DBV value write */
		EXYNOS_DCS_BUF_ADD_SET(ctx, cmd2_page0);
		EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x4C);
		EXYNOS_DCS_BUF_ADD(ctx, 0xDF, val1, val2, val1, val2, val1, val2);
	}
	return exynos_panel_set_brightness(ctx, br);
}

static void nt37290_set_local_hbm_mode(struct exynos_panel *ctx,
				       bool local_hbm_en)
{
	if (ctx->hbm.local_hbm.enabled == local_hbm_en)
		return;

	ctx->hbm.local_hbm.enabled = local_hbm_en;

	if (local_hbm_en) {
		if (ctx->panel_rev >= PANEL_REV_EVT1) {
			u16 level = ctx->bl->props.brightness * 4;
			u8 val1 = level >> 8;
			u8 val2 = level & 0xff;

			/* LHBM DBV value write */
			EXYNOS_DCS_BUF_ADD_SET(ctx, cmd2_page0);
			EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x4C);
			EXYNOS_DCS_BUF_ADD(ctx, 0xDF, val1, val2, val1, val2, val1, val2);
			/* FPS gamma timing */
			EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x02);
			/* Enter FPS mode */
			EXYNOS_DCS_BUF_ADD(ctx, 0x87, 0x01);
		} else {
			EXYNOS_DCS_BUF_ADD(ctx, 0x87, 0x21);
		}
		/* LHBM on */
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x85);
	} else {
		/* LHBM off */
		EXYNOS_DCS_BUF_ADD(ctx, 0x86);
		if (ctx->panel_rev >= PANEL_REV_EVT1) {
			/* Exit FPS mode */
			EXYNOS_DCS_BUF_ADD(ctx, 0x87, 0x00);
			/* normal gamma timing */
			EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x2F, 0x00);
		} else {
			EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x87, 0x20);
		}
	}
}

static void nt37290_mode_set(struct exynos_panel *ctx,
			     const struct exynos_panel_mode *pmode)
{
	if (!is_panel_active(ctx))
		return;

	nt37290_change_frequency(ctx, pmode);
}

static bool nt37290_is_mode_seamless(const struct exynos_panel *ctx,
				     const struct exynos_panel_mode *pmode)
{
	const struct drm_display_mode *c = &ctx->current_mode->mode;
	const struct drm_display_mode *n = &pmode->mode;

	/* seamless mode set can happen if active region resolution is same */
	return (c->vdisplay == n->vdisplay) && (c->hdisplay == n->hdisplay) &&
	       (c->flags == n->flags);
}

static void nt37290_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 rev = ((build_code & 0xE0) >> 3) | (build_code & 0x03);

	exynos_panel_get_panel_rev(ctx, rev);
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

static const u32 nt37290_bl_range[] = {
	94, 180, 270, 360, 2047
};

/* Truncate 8-bit signed value to 6-bit signed value */
#define TO_6BIT_SIGNED(v) (v & 0x3F)

static const struct drm_dsc_config nt37290_dsc_cfg = {
	.first_line_bpg_offset = 13,
	.rc_range_params = {
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{4, 10, TO_6BIT_SIGNED(-10)},
		{5, 10, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{8, 12, TO_6BIT_SIGNED(-12)},
		{12, 13, TO_6BIT_SIGNED(-12)},
	},
};

#define NT37290_DSC_CONFIG \
	.dsc = { \
		.enabled = true, \
		.dsc_count = 2, \
		.slice_count = 2, \
		.slice_height = 24, \
		.cfg = &nt37290_dsc_cfg, \
	}

static const struct exynos_panel_mode nt37290_modes[] = {
	{
		/* 1440x3120 @ 60Hz */
		.mode = {
			.name = "1440x3120x60",
			.clock = 298620,
			.hdisplay = 1440,
			.hsync_start = 1440 + 80, // add hfp
			.hsync_end = 1440 + 80 + 24, // add hsa
			.htotal = 1440 + 80 + 24 + 36, // add hbp
			.vdisplay = 3120,
			.vsync_start = 3120 + 12, // add vfp
			.vsync_end = 3120 + 12 + 4, // add vsa
			.vtotal = 3120 + 12 + 4 + 14, // add vbp
			.flags = 0,
			.width_mm = 71,
			.height_mm = 155,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			NT37290_DSC_CONFIG,
			.underrun_param = &underrun_param,
		},
		.te2_timing = {
			.rising_edge = 0,
			.falling_edge = 48,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		/* 1440x3120 @ 120Hz */
		.mode = {
			.name = "1440x3120x120",
			.clock = 597240,
			.hdisplay = 1440,
			.hsync_start = 1440 + 80, // add hfp
			.hsync_end = 1440 + 80 + 24, // add hsa
			.htotal = 1440 + 80 + 24 + 36, // add hbp
			.vdisplay = 3120,
			.vsync_start = 3120 + 12, // add vfp
			.vsync_end = 3120 + 12 + 4, // add vsa
			.vtotal = 3120 + 12 + 4 + 14, // add vbp
			.flags = 0,
			.width_mm = 71,
			.height_mm = 155,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			NT37290_DSC_CONFIG,
			.underrun_param = &underrun_param,
		},
		.te2_timing = {
			.rising_edge = 0,
			.falling_edge = 48,
		},
		.idle_mode = IDLE_MODE_ON_SELF_REFRESH,
	},
};

static const struct exynos_panel_mode nt37290_lp_mode = {
	.mode = {
		/* 1440x3120 @ 30Hz */
		.name = "1440x3120x30",
		.clock = 149310,
		.hdisplay = 1440,
		.hsync_start = 1440 + 80, // add hfp
		.hsync_end = 1440 + 80 + 24, // add hsa
		.htotal = 1440 + 80 + 24 + 36, // add hbp
		.vdisplay = 3120,
		.vsync_start = 3120 + 12, // add vfp
		.vsync_end = 3120 + 12 + 4, // add vsa
		.vtotal = 3120 + 12 + 4 + 14, // add vbp
		.flags = 0,
		.width_mm = 71,
		.height_mm = 155,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.bpc = 8,
		NT37290_DSC_CONFIG,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	},
};

static void nt37290_panel_init(struct exynos_panel *ctx)
{
	struct dentry *csroot = ctx->debugfs_cmdset_entry;

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &nt37290_init_cmd_set, "init");
	exynos_panel_send_cmd_set(ctx, &nt37290_lhbm_on_setting_cmd_set);
}

static int nt37290_panel_probe(struct mipi_dsi_device *dsi)
{
	struct nt37290_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->hw_vrefresh = 60;
	spanel->hw_idle_vrefresh = 0;
	spanel->auto_mode_vrefresh = 0;
	spanel->delayed_idle = false;

	return exynos_panel_common_init(dsi, &spanel->base);
}

static const struct drm_panel_funcs nt37290_drm_funcs = {
	.disable = nt37290_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = nt37290_enable,
	.get_modes = exynos_panel_get_modes,
};

static const struct exynos_panel_funcs nt37290_exynos_funcs = {
	.set_brightness = nt37290_set_brightness,
	.set_lp_mode = exynos_panel_set_lp_mode,
	.set_nolp_mode = nt37290_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_local_hbm_mode = nt37290_set_local_hbm_mode,
	.is_mode_seamless = nt37290_is_mode_seamless,
	.mode_set = nt37290_mode_set,
	.panel_init = nt37290_panel_init,
	.get_panel_rev = nt37290_get_panel_rev,
	.get_te2_edges = exynos_panel_get_te2_edges,
	.configure_te2_edges = exynos_panel_configure_te2_edges,
	.update_te2 = nt37290_update_te2,
	.set_self_refresh = nt37290_set_self_refresh,
	.commit_done = nt37290_commit_done,
};

const struct brightness_capability nt37290_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 500,
		},
		.level = {
			.min = 3,
			.max = 2047,
		},
		.percentage = {
			.min = 0,
			.max = 50,
		},
	},
	.hbm = {
		.nits = {
			.min = 550,
			.max = 1000,
		},
		.level = {
			.min = 2048,
			.max = 4094,
		},
		.percentage = {
			.min = 50,
			.max = 100,
		},
	},
};

const struct exynos_panel_desc boe_nt37290 = {
	.panel_id_reg = 0xAC,
	.data_lane_cnt = 4,
	.max_brightness = 4094,
	.min_brightness = 3,
	.dft_brightness = 1023,
	.brt_capability = &nt37290_brightness_capability,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.bl_range = nt37290_bl_range,
	.bl_num_ranges = ARRAY_SIZE(nt37290_bl_range),
	.modes = nt37290_modes,
	.num_modes = ARRAY_SIZE(nt37290_modes),
	.off_cmd_set = &nt37290_off_cmd_set,
	.lp_mode = &nt37290_lp_mode,
	.lp_cmd_set = &nt37290_lp_cmd_set,
	.binned_lp = nt37290_binned_lp,
	.num_binned_lp = ARRAY_SIZE(nt37290_binned_lp),
	.panel_func = &nt37290_drm_funcs,
	.exynos_panel_func = &nt37290_exynos_funcs,
};

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "boe,nt37290", .data = &boe_nt37290 },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = nt37290_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-boe-nt37290",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Chris Lu <luchris@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based BOE nt37290 panel driver");
MODULE_LICENSE("GPL");
