/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifndef __HDMI_CONNECTOR_H__
#define __HDMI_CONNECTOR_H__

#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/hdmi.h>

#include <drm/drm_bridge.h>

#include "msm_drv.h"
#include "hdmi.xml.h"

struct hdmi_phy;
struct hdmi_platform_config;

struct hdmi_audio {
	bool enabled;
	int rate;
	int channels;
};

struct hdmi_hdcp_ctrl;

struct hdmi {
	struct drm_device *dev;
	struct platform_device *pdev;

	const struct hdmi_platform_config *config;

	/* audio state: */
	struct hdmi_audio audio;

	/* video state: */
	bool power_on;
	bool hpd_enabled;
	struct mutex state_mutex; /* protects two booleans */
	unsigned long int pixclock;

	void __iomem *mmio;
	void __iomem *qfprom_mmio;
	phys_addr_t mmio_phy_addr;

	struct regulator_bulk_data *pwr_regs;
	struct clk_bulk_data *pwr_clks;
	struct clk *extp_clk;

	struct gpio_desc *hpd_gpiod;

	struct hdmi_phy *phy;
	struct device *phy_dev;

	struct i2c_adapter *i2c;
	struct drm_connector *connector;
	struct drm_bridge *bridge;

	struct drm_bridge *next_bridge;

	/* the encoder we are hooked to (outside of hdmi block) */
	struct drm_encoder *encoder;

	int irq;
	struct workqueue_struct *workq;

	struct hdmi_hdcp_ctrl *hdcp_ctrl;

	/*
	* spinlock to protect registers shared by different execution
	* REG_HDMI_CTRL
	* REG_HDMI_DDC_ARBITRATION
	* REG_HDMI_HDCP_INT_CTRL
	* REG_HDMI_HPD_CTRL
	*/
	spinlock_t reg_lock;
};

/* platform config data (ie. from DT, or pdata) */
struct hdmi_platform_config {
	/* regulators that need to be on for screen pwr: */
	const char * const *pwr_reg_names;
	int pwr_reg_cnt;

	/* clks that need to be on: */
	const char * const *pwr_clk_names;
	int pwr_clk_cnt;
};

struct hdmi_bridge {
	struct drm_bridge base;
	struct hdmi *hdmi;
	struct work_struct hpd_work;
};
#define to_hdmi_bridge(x) container_of(x, struct hdmi_bridge, base)

void msm_hdmi_set_mode(struct hdmi *hdmi, bool power_on);

static inline void hdmi_write(struct hdmi *hdmi, u32 reg, u32 data)
{
	writel(data, hdmi->mmio + reg);
}

static inline u32 hdmi_read(struct hdmi *hdmi, u32 reg)
{
	return readl(hdmi->mmio + reg);
}

static inline u32 hdmi_qfprom_read(struct hdmi *hdmi, u32 reg)
{
	return readl(hdmi->qfprom_mmio + reg);
}

/*
 * hdmi phy:
 */

enum hdmi_phy_type {
	MSM_HDMI_PHY_8x60,
	MSM_HDMI_PHY_8960,
	MSM_HDMI_PHY_8x74,
	MSM_HDMI_PHY_8996,
	MSM_HDMI_PHY_8998,
	MSM_HDMI_PHY_MAX,
};

struct hdmi_phy_cfg {
	enum hdmi_phy_type type;
	void (*powerup)(struct hdmi_phy *phy, unsigned long int pixclock);
	void (*powerdown)(struct hdmi_phy *phy);
	const char * const *reg_names;
	int num_regs;
	const char * const *clk_names;
	int num_clks;
};

extern const struct hdmi_phy_cfg msm_hdmi_phy_8x60_cfg;
extern const struct hdmi_phy_cfg msm_hdmi_phy_8960_cfg;
extern const struct hdmi_phy_cfg msm_hdmi_phy_8x74_cfg;
extern const struct hdmi_phy_cfg msm_hdmi_phy_8996_cfg;
extern const struct hdmi_phy_cfg msm_hdmi_phy_8998_cfg;

struct hdmi_phy {
	struct platform_device *pdev;
	void __iomem *mmio;
	struct hdmi_phy_cfg *cfg;
	const struct hdmi_phy_funcs *funcs;
	struct regulator_bulk_data *regs;
	struct clk **clks;
};

static inline void hdmi_phy_write(struct hdmi_phy *phy, u32 reg, u32 data)
{
	writel(data, phy->mmio + reg);
}

static inline u32 hdmi_phy_read(struct hdmi_phy *phy, u32 reg)
{
	return readl(phy->mmio + reg);
}

int msm_hdmi_phy_resource_enable(struct hdmi_phy *phy);
void msm_hdmi_phy_resource_disable(struct hdmi_phy *phy);
void msm_hdmi_phy_powerup(struct hdmi_phy *phy, unsigned long int pixclock);
void msm_hdmi_phy_powerdown(struct hdmi_phy *phy);
void __init msm_hdmi_phy_driver_register(void);
void __exit msm_hdmi_phy_driver_unregister(void);

#ifdef CONFIG_COMMON_CLK
int msm_hdmi_pll_8960_init(struct platform_device *pdev);
int msm_hdmi_pll_8996_init(struct platform_device *pdev);
int msm_hdmi_pll_8998_init(struct platform_device *pdev);
#else
static inline int msm_hdmi_pll_8960_init(struct platform_device *pdev)
{
	return -ENODEV;
}

static inline int msm_hdmi_pll_8996_init(struct platform_device *pdev)
{
	return -ENODEV;
}

static inline int msm_hdmi_pll_8998_init(struct platform_device *pdev)
{
	return -ENODEV;
}
#endif

/*
 * audio:
 */
struct hdmi_codec_daifmt;
struct hdmi_codec_params;

int msm_hdmi_audio_update(struct hdmi *hdmi);
int msm_hdmi_bridge_audio_prepare(struct drm_bridge *bridge,
				  struct drm_connector *connector,
				  struct hdmi_codec_daifmt *daifmt,
				  struct hdmi_codec_params *params);
void msm_hdmi_bridge_audio_shutdown(struct drm_bridge *bridge,
				    struct drm_connector *connector);

/*
 * hdmi bridge:
 */

int msm_hdmi_bridge_init(struct hdmi *hdmi);

void msm_hdmi_hpd_irq(struct drm_bridge *bridge);
enum drm_connector_status msm_hdmi_bridge_detect(
		struct drm_bridge *bridge, struct drm_connector *connector);
void msm_hdmi_hpd_enable(struct drm_bridge *bridge);
void msm_hdmi_hpd_disable(struct drm_bridge *bridge);

/*
 * i2c adapter for ddc:
 */

void msm_hdmi_i2c_irq(struct i2c_adapter *i2c);
void msm_hdmi_i2c_destroy(struct i2c_adapter *i2c);
struct i2c_adapter *msm_hdmi_i2c_init(struct hdmi *hdmi);

/*
 * hdcp
 */
#ifdef CONFIG_DRM_MSM_HDMI_HDCP
struct hdmi_hdcp_ctrl *msm_hdmi_hdcp_init(struct hdmi *hdmi);
void msm_hdmi_hdcp_destroy(struct hdmi *hdmi);
void msm_hdmi_hdcp_on(struct hdmi_hdcp_ctrl *hdcp_ctrl);
void msm_hdmi_hdcp_off(struct hdmi_hdcp_ctrl *hdcp_ctrl);
void msm_hdmi_hdcp_irq(struct hdmi_hdcp_ctrl *hdcp_ctrl);
#else
static inline struct hdmi_hdcp_ctrl *msm_hdmi_hdcp_init(struct hdmi *hdmi)
{
	return ERR_PTR(-ENXIO);
}
static inline void msm_hdmi_hdcp_destroy(struct hdmi *hdmi) {}
static inline void msm_hdmi_hdcp_on(struct hdmi_hdcp_ctrl *hdcp_ctrl) {}
static inline void msm_hdmi_hdcp_off(struct hdmi_hdcp_ctrl *hdcp_ctrl) {}
static inline void msm_hdmi_hdcp_irq(struct hdmi_hdcp_ctrl *hdcp_ctrl) {}
#endif

#endif /* __HDMI_CONNECTOR_H__ */
