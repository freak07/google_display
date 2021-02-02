/* SPDX-License-Identifier: GPL-2.0-only
 *
 * linux/drivers/gpu/drm/samsung/exynos_drm_dsim.h
 *
 * Copyright (c) 2018 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Headef file for Samsung MIPI DSI Master driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __EXYNOS_DRM_DSI_H__
#define __EXYNOS_DRM_DSI_H__

/* Add header */
#include <drm/drm_encoder.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_property.h>
#include <drm/drm_panel.h>
#include <video/videomode.h>

#include <dsim_cal.h>

#include "exynos_drm_drv.h"

enum dsim_state {
	DSIM_STATE_HSCLKEN,
	DSIM_STATE_ULPS,
	DSIM_STATE_SUSPEND
};

struct dsim_pll_features {
	u64 finput;
	u64 foptimum;
	u64 fout_min, fout_max;
	u64 fvco_min, fvco_max;
	u32 p_min, p_max;
	u32 m_min, m_max;
	u32 s_min, s_max;
	u32 k_bits;
};

struct dsim_pll_params {
	unsigned int num_modes;
	struct dsim_pll_param **params;
	struct dsim_pll_features *features;
};

struct dsim_resources {
	void __iomem *regs;
	void __iomem *phy_regs;
	void __iomem *phy_regs_ex;
	void __iomem *ss_reg_base;
	struct phy *phy;
	struct phy *phy_ex;
};

struct dsim_device {
	struct drm_encoder encoder;
	struct mipi_dsi_host dsi_host;
	struct device *dev;
	struct drm_bridge *panel_bridge;
	struct mipi_dsi_device *dsi_device;

	enum exynos_drm_output_type output_type;
	int te_from;
	int te_gpio;
	struct pinctrl *pinctrl;
	struct pinctrl_state *te_on;
	struct pinctrl_state *te_off;
	bool hw_trigger;

	struct dsim_resources res;
	struct clk **clks;
	struct dsim_pll_params *pll_params;

#ifdef CONFIG_DEBUG_FS
        struct dentry *debugfs_entry;
#endif

	int irq;
	int id;
	spinlock_t slock;
	struct mutex cmd_lock;
	struct mutex state_lock;
	struct completion ph_wr_comp;
	struct completion pl_wr_comp;
	struct completion rd_comp;

	enum dsim_state state;

	/* set bist mode by sysfs */
	unsigned int bist_mode;

	/* FIXME: dsim cal structure */
	struct dsim_reg_config config;
	struct dsim_clks clk_param;

	struct dsim_pll_param *current_pll_param;

	int idle_ip_index;
};

extern struct dsim_device *dsim_drvdata[MAX_DSI_CNT];

#define encoder_to_dsim(e) container_of(e, struct dsim_device, encoder)

#define MIPI_WR_TIMEOUT				msecs_to_jiffies(50)
#define MIPI_RD_TIMEOUT				msecs_to_jiffies(100)

struct decon_device;

static inline const struct decon_device *
dsim_get_decon(const struct dsim_device *dsim)
{
	const struct drm_crtc *crtc = dsim->encoder.crtc;

	if (!crtc)
		return NULL;

	return to_exynos_crtc(crtc)->ctx;
}

void dsim_enter_ulps(struct dsim_device *dsim);
void dsim_exit_ulps(struct dsim_device *dsim);

#ifdef CONFIG_DEBUG_FS
void dsim_diag_create_debugfs(struct dsim_device *dsim);
void dsim_diag_remove_debugfs(struct dsim_device *dsim);

int dsim_dphy_diag_get_reg(struct dsim_device *dsim,
                           struct dsim_dphy_diag *diag, uint32_t *vals);
int dsim_dphy_diag_set_reg(struct dsim_device *dsim,
                           struct dsim_dphy_diag *diag, uint32_t val);
#endif

#endif /* __EXYNOS_DRM_DSI_H__ */
