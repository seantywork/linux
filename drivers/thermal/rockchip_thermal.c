// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2016, Fuzhou Rockchip Electronics Co., Ltd
 * Caesar Wang <wxt@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <linux/mfd/syscon.h>
#include <linux/pinctrl/consumer.h>

/*
 * If the temperature over a period of time High,
 * the resulting TSHUT gave CRU module,let it reset the entire chip,
 * or via GPIO give PMIC.
 */
enum tshut_mode {
	TSHUT_MODE_CRU = 0,
	TSHUT_MODE_GPIO,
};

/*
 * The system Temperature Sensors tshut(tshut) polarity
 * the bit 8 is tshut polarity.
 * 0: low active, 1: high active
 */
enum tshut_polarity {
	TSHUT_LOW_ACTIVE = 0,
	TSHUT_HIGH_ACTIVE,
};

/*
 * The conversion table has the adc value and temperature.
 * ADC_DECREMENT: the adc value is of diminishing.(e.g. rk3288_code_table)
 * ADC_INCREMENT: the adc value is incremental.(e.g. rk3368_code_table)
 */
enum adc_sort_mode {
	ADC_DECREMENT = 0,
	ADC_INCREMENT,
};

#include "thermal_hwmon.h"

/**
 * struct chip_tsadc_table - hold information about chip-specific differences
 * @id: conversion table
 * @length: size of conversion table
 * @data_mask: mask to apply on data inputs
 * @mode: sort mode of this adc variant (incrementing or decrementing)
 */
struct chip_tsadc_table {
	const struct tsadc_table *id;
	unsigned int length;
	u32 data_mask;
	enum adc_sort_mode mode;
};

/**
 * struct rockchip_tsadc_chip - hold the private data of tsadc chip
 * @chn_offset: the channel offset of the first channel
 * @chn_num: the channel number of tsadc chip
 * @trim_slope: used to convert the trim code to a temperature in millicelsius
 * @tshut_temp: the hardware-controlled shutdown temperature value, with no trim
 * @tshut_mode: the hardware-controlled shutdown mode (0:CRU 1:GPIO)
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 * @initialize: SoC special initialize tsadc controller method
 * @irq_ack: clear the interrupt
 * @control: enable/disable method for the tsadc controller
 * @get_temp: get the raw temperature, unadjusted by trim
 * @set_alarm_temp: set the high temperature interrupt
 * @set_tshut_temp: set the hardware-controlled shutdown temperature
 * @set_tshut_mode: set the hardware-controlled shutdown mode
 * @get_trim_code: convert a hardware temperature code to one adjusted for by trim
 * @table: the chip-specific conversion table
 */
struct rockchip_tsadc_chip {
	/* The sensor id of chip correspond to the ADC channel */
	int chn_offset;
	int chn_num;

	/* Used to convert trim code to trim temp */
	int trim_slope;

	/* The hardware-controlled tshut property */
	int tshut_temp;
	enum tshut_mode tshut_mode;
	enum tshut_polarity tshut_polarity;

	/* Chip-wide methods */
	void (*initialize)(struct regmap *grf,
			   void __iomem *reg, enum tshut_polarity p);
	void (*irq_ack)(void __iomem *reg);
	void (*control)(void __iomem *reg, bool on);

	/* Per-sensor methods */
	int (*get_temp)(const struct chip_tsadc_table *table,
			int chn, void __iomem *reg, int *temp);
	int (*set_alarm_temp)(const struct chip_tsadc_table *table,
			      int chn, void __iomem *reg, int temp);
	int (*set_tshut_temp)(const struct chip_tsadc_table *table,
			      int chn, void __iomem *reg, int temp);
	void (*set_tshut_mode)(int chn, void __iomem *reg, enum tshut_mode m);
	int (*get_trim_code)(const struct chip_tsadc_table *table,
			     int code, int trim_base, int trim_base_frac);

	/* Per-table methods */
	struct chip_tsadc_table table;
};

/**
 * struct rockchip_thermal_sensor - hold the information of thermal sensor
 * @thermal:  pointer to the platform/configuration data
 * @tzd: pointer to a thermal zone
 * @of_node: pointer to the device_node representing this sensor, if any
 * @id: identifier of the thermal sensor
 * @trim_temp: per-sensor trim temperature value
 */
struct rockchip_thermal_sensor {
	struct rockchip_thermal_data *thermal;
	struct thermal_zone_device *tzd;
	struct device_node *of_node;
	int id;
	int trim_temp;
};

/**
 * struct rockchip_thermal_data - hold the private data of thermal driver
 * @chip: pointer to the platform/configuration data
 * @pdev: platform device of thermal
 * @reset: the reset controller of tsadc
 * @sensors: array of thermal sensors
 * @clk: the controller clock is divided by the exteral 24MHz
 * @pclk: the advanced peripherals bus clock
 * @grf: the general register file will be used to do static set by software
 * @regs: the base address of tsadc controller
 * @trim_base: major component of sensor trim value, in Celsius
 * @trim_base_frac: minor component of sensor trim value, in Decicelsius
 * @trim: fallback thermal trim value for each channel
 * @tshut_temp: the hardware-controlled shutdown temperature value, with no trim
 * @trim_temp: the fallback trim temperature for the whole sensor
 * @tshut_mode: the hardware-controlled shutdown mode (0:CRU 1:GPIO)
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 */
struct rockchip_thermal_data {
	const struct rockchip_tsadc_chip *chip;
	struct platform_device *pdev;
	struct reset_control *reset;

	struct rockchip_thermal_sensor *sensors;

	struct clk *clk;
	struct clk *pclk;

	struct regmap *grf;
	void __iomem *regs;

	int trim_base;
	int trim_base_frac;
	int trim;

	int tshut_temp;
	int trim_temp;
	enum tshut_mode tshut_mode;
	enum tshut_polarity tshut_polarity;
};

/*
 * TSADC Sensor Register description:
 *
 * TSADCV2_* are used for RK3288 SoCs, the other chips can reuse it.
 * TSADCV3_* are used for newer SoCs than RK3288. (e.g: RK3228, RK3399)
 *
 */
#define TSADCV2_USER_CON			0x00
#define TSADCV2_AUTO_CON			0x04
#define TSADCV2_INT_EN				0x08
#define TSADCV2_INT_PD				0x0c
#define TSADCV3_AUTO_SRC_CON			0x0c
#define TSADCV3_HT_INT_EN			0x14
#define TSADCV3_HSHUT_GPIO_INT_EN		0x18
#define TSADCV3_HSHUT_CRU_INT_EN		0x1c
#define TSADCV3_INT_PD				0x24
#define TSADCV3_HSHUT_PD			0x28
#define TSADCV2_DATA(chn)			(0x20 + (chn) * 0x04)
#define TSADCV2_COMP_INT(chn)		        (0x30 + (chn) * 0x04)
#define TSADCV2_COMP_SHUT(chn)		        (0x40 + (chn) * 0x04)
#define TSADCV3_DATA(chn)			(0x2c + (chn) * 0x04)
#define TSADCV3_COMP_INT(chn)		        (0x6c + (chn) * 0x04)
#define TSADCV3_COMP_SHUT(chn)		        (0x10c + (chn) * 0x04)
#define TSADCV2_HIGHT_INT_DEBOUNCE		0x60
#define TSADCV2_HIGHT_TSHUT_DEBOUNCE		0x64
#define TSADCV3_HIGHT_INT_DEBOUNCE		0x14c
#define TSADCV3_HIGHT_TSHUT_DEBOUNCE		0x150
#define TSADCV2_AUTO_PERIOD			0x68
#define TSADCV2_AUTO_PERIOD_HT			0x6c
#define TSADCV3_AUTO_PERIOD			0x154
#define TSADCV3_AUTO_PERIOD_HT			0x158

#define TSADCV2_AUTO_EN				BIT(0)
#define TSADCV2_AUTO_EN_MASK			BIT(16)
#define TSADCV2_AUTO_SRC_EN(chn)		BIT(4 + (chn))
#define TSADCV3_AUTO_SRC_EN(chn)		BIT(chn)
#define TSADCV3_AUTO_SRC_EN_MASK(chn)		BIT(16 + chn)
#define TSADCV2_AUTO_TSHUT_POLARITY_HIGH	BIT(8)
#define TSADCV2_AUTO_TSHUT_POLARITY_MASK	BIT(24)

#define TSADCV3_AUTO_Q_SEL_EN			BIT(1)

#define TSADCV2_INT_SRC_EN(chn)			BIT(chn)
#define TSADCV2_INT_SRC_EN_MASK(chn)		BIT(16 + (chn))
#define TSADCV2_SHUT_2GPIO_SRC_EN(chn)		BIT(4 + (chn))
#define TSADCV2_SHUT_2CRU_SRC_EN(chn)		BIT(8 + (chn))

#define TSADCV2_INT_PD_CLEAR_MASK		~BIT(8)
#define TSADCV3_INT_PD_CLEAR_MASK		~BIT(16)
#define TSADCV4_INT_PD_CLEAR_MASK		0xffffffff

#define TSADCV2_DATA_MASK			0xfff
#define TSADCV3_DATA_MASK			0x3ff
#define TSADCV4_DATA_MASK			0x1ff

#define TSADCV2_HIGHT_INT_DEBOUNCE_COUNT	4
#define TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT	4
#define TSADCV2_AUTO_PERIOD_TIME		250 /* 250ms */
#define TSADCV2_AUTO_PERIOD_HT_TIME		50  /* 50ms */
#define TSADCV3_AUTO_PERIOD_TIME		1875 /* 2.5ms */
#define TSADCV3_AUTO_PERIOD_HT_TIME		1875 /* 2.5ms */

#define TSADCV5_AUTO_PERIOD_TIME		1622 /* 2.5ms */
#define TSADCV5_AUTO_PERIOD_HT_TIME		1622 /* 2.5ms */
#define TSADCV6_AUTO_PERIOD_TIME		5000 /* 2.5ms */
#define TSADCV6_AUTO_PERIOD_HT_TIME		5000 /* 2.5ms */

#define TSADCV2_USER_INTER_PD_SOC		0x340 /* 13 clocks */
#define TSADCV5_USER_INTER_PD_SOC		0xfc0 /* 97us, at least 90us */

#define GRF_SARADC_TESTBIT			0x0e644
#define GRF_TSADC_TESTBIT_L			0x0e648
#define GRF_TSADC_TESTBIT_H			0x0e64c

#define PX30_GRF_SOC_CON2			0x0408

#define RK3568_GRF_TSADC_CON			0x0600
#define RK3568_GRF_TSADC_ANA_REG0		(0x10001 << 0)
#define RK3568_GRF_TSADC_ANA_REG1		(0x10001 << 1)
#define RK3568_GRF_TSADC_ANA_REG2		(0x10001 << 2)
#define RK3568_GRF_TSADC_TSEN			(0x10001 << 8)

#define RK3588_GRF0_TSADC_CON			0x0100

#define RK3588_GRF0_TSADC_TRM			(0xff0077 << 0)
#define RK3588_GRF0_TSADC_SHUT_2CRU		(0x30003 << 10)
#define RK3588_GRF0_TSADC_SHUT_2GPIO		(0x70007 << 12)

#define GRF_SARADC_TESTBIT_ON			(0x10001 << 2)
#define GRF_TSADC_TESTBIT_H_ON			(0x10001 << 2)
#define GRF_TSADC_VCM_EN_L			(0x10001 << 7)
#define GRF_TSADC_VCM_EN_H			(0x10001 << 7)

#define GRF_CON_TSADC_CH_INV			(0x10001 << 1)


#define RK_MAX_TEMP				(180000)

/**
 * struct tsadc_table - code to temperature conversion table
 * @code: the value of adc channel
 * @temp: the temperature
 * Note:
 * code to temperature mapping of the temperature sensor is a piece wise linear
 * curve.Any temperature, code faling between to 2 give temperatures can be
 * linearly interpolated.
 * Code to Temperature mapping should be updated based on manufacturer results.
 */
struct tsadc_table {
	u32 code;
	int temp;
};

static const struct tsadc_table rv1108_table[] = {
	{0, -40000},
	{374, -40000},
	{382, -35000},
	{389, -30000},
	{397, -25000},
	{405, -20000},
	{413, -15000},
	{421, -10000},
	{429, -5000},
	{436, 0},
	{444, 5000},
	{452, 10000},
	{460, 15000},
	{468, 20000},
	{476, 25000},
	{483, 30000},
	{491, 35000},
	{499, 40000},
	{507, 45000},
	{515, 50000},
	{523, 55000},
	{531, 60000},
	{539, 65000},
	{547, 70000},
	{555, 75000},
	{562, 80000},
	{570, 85000},
	{578, 90000},
	{586, 95000},
	{594, 100000},
	{602, 105000},
	{610, 110000},
	{618, 115000},
	{626, 120000},
	{634, 125000},
	{TSADCV2_DATA_MASK, 125000},
};

static const struct tsadc_table rk3228_code_table[] = {
	{0, -40000},
	{588, -40000},
	{593, -35000},
	{598, -30000},
	{603, -25000},
	{608, -20000},
	{613, -15000},
	{618, -10000},
	{623, -5000},
	{629, 0},
	{634, 5000},
	{639, 10000},
	{644, 15000},
	{649, 20000},
	{654, 25000},
	{660, 30000},
	{665, 35000},
	{670, 40000},
	{675, 45000},
	{681, 50000},
	{686, 55000},
	{691, 60000},
	{696, 65000},
	{702, 70000},
	{707, 75000},
	{712, 80000},
	{717, 85000},
	{723, 90000},
	{728, 95000},
	{733, 100000},
	{738, 105000},
	{744, 110000},
	{749, 115000},
	{754, 120000},
	{760, 125000},
	{TSADCV2_DATA_MASK, 125000},
};

static const struct tsadc_table rk3288_code_table[] = {
	{TSADCV2_DATA_MASK, -40000},
	{3800, -40000},
	{3792, -35000},
	{3783, -30000},
	{3774, -25000},
	{3765, -20000},
	{3756, -15000},
	{3747, -10000},
	{3737, -5000},
	{3728, 0},
	{3718, 5000},
	{3708, 10000},
	{3698, 15000},
	{3688, 20000},
	{3678, 25000},
	{3667, 30000},
	{3656, 35000},
	{3645, 40000},
	{3634, 45000},
	{3623, 50000},
	{3611, 55000},
	{3600, 60000},
	{3588, 65000},
	{3575, 70000},
	{3563, 75000},
	{3550, 80000},
	{3537, 85000},
	{3524, 90000},
	{3510, 95000},
	{3496, 100000},
	{3482, 105000},
	{3467, 110000},
	{3452, 115000},
	{3437, 120000},
	{3421, 125000},
	{0, 125000},
};

static const struct tsadc_table rk3328_code_table[] = {
	{0, -40000},
	{296, -40000},
	{304, -35000},
	{313, -30000},
	{322, -25000},
	{331, -20000},
	{340, -15000},
	{349, -10000},
	{359, -5000},
	{368, 0},
	{378, 5000},
	{388, 10000},
	{398, 15000},
	{408, 20000},
	{418, 25000},
	{429, 30000},
	{440, 35000},
	{451, 40000},
	{462, 45000},
	{473, 50000},
	{485, 55000},
	{496, 60000},
	{508, 65000},
	{521, 70000},
	{533, 75000},
	{546, 80000},
	{559, 85000},
	{572, 90000},
	{586, 95000},
	{600, 100000},
	{614, 105000},
	{629, 110000},
	{644, 115000},
	{659, 120000},
	{675, 125000},
	{TSADCV2_DATA_MASK, 125000},
};

static const struct tsadc_table rk3368_code_table[] = {
	{0, -40000},
	{106, -40000},
	{108, -35000},
	{110, -30000},
	{112, -25000},
	{114, -20000},
	{116, -15000},
	{118, -10000},
	{120, -5000},
	{122, 0},
	{124, 5000},
	{126, 10000},
	{128, 15000},
	{130, 20000},
	{132, 25000},
	{134, 30000},
	{136, 35000},
	{138, 40000},
	{140, 45000},
	{142, 50000},
	{144, 55000},
	{146, 60000},
	{148, 65000},
	{150, 70000},
	{152, 75000},
	{154, 80000},
	{156, 85000},
	{158, 90000},
	{160, 95000},
	{162, 100000},
	{163, 105000},
	{165, 110000},
	{167, 115000},
	{169, 120000},
	{171, 125000},
	{TSADCV3_DATA_MASK, 125000},
};

static const struct tsadc_table rk3399_code_table[] = {
	{0, -40000},
	{402, -40000},
	{410, -35000},
	{419, -30000},
	{427, -25000},
	{436, -20000},
	{444, -15000},
	{453, -10000},
	{461, -5000},
	{470, 0},
	{478, 5000},
	{487, 10000},
	{496, 15000},
	{504, 20000},
	{513, 25000},
	{521, 30000},
	{530, 35000},
	{538, 40000},
	{547, 45000},
	{555, 50000},
	{564, 55000},
	{573, 60000},
	{581, 65000},
	{590, 70000},
	{599, 75000},
	{607, 80000},
	{616, 85000},
	{624, 90000},
	{633, 95000},
	{642, 100000},
	{650, 105000},
	{659, 110000},
	{668, 115000},
	{677, 120000},
	{685, 125000},
	{TSADCV3_DATA_MASK, 125000},
};

static const struct tsadc_table rk3568_code_table[] = {
	{0, -40000},
	{1584, -40000},
	{1620, -35000},
	{1652, -30000},
	{1688, -25000},
	{1720, -20000},
	{1756, -15000},
	{1788, -10000},
	{1824, -5000},
	{1856, 0},
	{1892, 5000},
	{1924, 10000},
	{1956, 15000},
	{1992, 20000},
	{2024, 25000},
	{2060, 30000},
	{2092, 35000},
	{2128, 40000},
	{2160, 45000},
	{2196, 50000},
	{2228, 55000},
	{2264, 60000},
	{2300, 65000},
	{2332, 70000},
	{2368, 75000},
	{2400, 80000},
	{2436, 85000},
	{2468, 90000},
	{2500, 95000},
	{2536, 100000},
	{2572, 105000},
	{2604, 110000},
	{2636, 115000},
	{2672, 120000},
	{2704, 125000},
	{TSADCV2_DATA_MASK, 125000},
};

static const struct tsadc_table rk3588_code_table[] = {
	{0, -40000},
	{215, -40000},
	{285, 25000},
	{350, 85000},
	{395, 125000},
	{TSADCV4_DATA_MASK, 125000},
};

static u32 rk_tsadcv2_temp_to_code(const struct chip_tsadc_table *table,
				   int temp)
{
	int high, low, mid;
	unsigned long num;
	unsigned int denom;
	u32 error = table->data_mask;

	low = 0;
	high = (table->length - 1) - 1; /* ignore the last check for table */
	mid = (high + low) / 2;

	/* Return mask code data when the temp is over table range */
	if (temp < table->id[low].temp || temp > table->id[high].temp)
		goto exit;

	while (low <= high) {
		if (temp == table->id[mid].temp)
			return table->id[mid].code;
		else if (temp < table->id[mid].temp)
			high = mid - 1;
		else
			low = mid + 1;
		mid = (low + high) / 2;
	}

	/*
	 * The conversion code granularity provided by the table. Let's
	 * assume that the relationship between temperature and
	 * analog value between 2 table entries is linear and interpolate
	 * to produce less granular result.
	 */
	num = abs(table->id[mid + 1].code - table->id[mid].code);
	num *= temp - table->id[mid].temp;
	denom = table->id[mid + 1].temp - table->id[mid].temp;

	switch (table->mode) {
	case ADC_DECREMENT:
		return table->id[mid].code - (num / denom);
	case ADC_INCREMENT:
		return table->id[mid].code + (num / denom);
	default:
		pr_err("%s: unknown table mode: %d\n", __func__, table->mode);
		return error;
	}

exit:
	pr_err("%s: invalid temperature, temp=%d error=%d\n",
	       __func__, temp, error);
	return error;
}

static int rk_tsadcv2_code_to_temp(const struct chip_tsadc_table *table,
				   u32 code, int *temp)
{
	unsigned int low = 1;
	unsigned int high = table->length - 1;
	unsigned int mid = (low + high) / 2;
	unsigned int num;
	unsigned long denom;

	WARN_ON(table->length < 2);

	switch (table->mode) {
	case ADC_DECREMENT:
		code &= table->data_mask;
		if (code <= table->id[high].code)
			return -EAGAIN;		/* Incorrect reading */

		while (low <= high) {
			if (code >= table->id[mid].code &&
			    code < table->id[mid - 1].code)
				break;
			else if (code < table->id[mid].code)
				low = mid + 1;
			else
				high = mid - 1;

			mid = (low + high) / 2;
		}
		break;
	case ADC_INCREMENT:
		code &= table->data_mask;
		if (code < table->id[low].code)
			return -EAGAIN;		/* Incorrect reading */

		while (low <= high) {
			if (code <= table->id[mid].code &&
			    code > table->id[mid - 1].code)
				break;
			else if (code > table->id[mid].code)
				low = mid + 1;
			else
				high = mid - 1;

			mid = (low + high) / 2;
		}
		break;
	default:
		pr_err("%s: unknown table mode: %d\n", __func__, table->mode);
		return -EINVAL;
	}

	/*
	 * The 5C granularity provided by the table is too much. Let's
	 * assume that the relationship between sensor readings and
	 * temperature between 2 table entries is linear and interpolate
	 * to produce less granular result.
	 */
	num = table->id[mid].temp - table->id[mid - 1].temp;
	num *= abs(table->id[mid - 1].code - code);
	denom = abs(table->id[mid - 1].code - table->id[mid].code);
	*temp = table->id[mid - 1].temp + (num / denom);

	return 0;
}

/**
 * rk_tsadcv2_initialize - initialize TASDC Controller.
 * @grf: the general register file will be used to do static set by software
 * @regs: the base address of tsadc controller
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 *
 * (1) Set TSADC_V2_AUTO_PERIOD:
 *     Configure the interleave between every two accessing of
 *     TSADC in normal operation.
 *
 * (2) Set TSADCV2_AUTO_PERIOD_HT:
 *     Configure the interleave between every two accessing of
 *     TSADC after the temperature is higher than COM_SHUT or COM_INT.
 *
 * (3) Set TSADCV2_HIGH_INT_DEBOUNCE and TSADC_HIGHT_TSHUT_DEBOUNCE:
 *     If the temperature is higher than COMP_INT or COMP_SHUT for
 *     "debounce" times, TSADC controller will generate interrupt or TSHUT.
 */
static void rk_tsadcv2_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(0U | TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(0U & ~TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);

	writel_relaxed(TSADCV2_AUTO_PERIOD_TIME, regs + TSADCV2_AUTO_PERIOD);
	writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
	writel_relaxed(TSADCV2_AUTO_PERIOD_HT_TIME,
		       regs + TSADCV2_AUTO_PERIOD_HT);
	writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);
}

/**
 * rk_tsadcv3_initialize - initialize TASDC Controller.
 * @grf: the general register file will be used to do static set by software
 * @regs: the base address of tsadc controller
 * @tshut_polarity: the hardware-controlled active polarity (0:LOW 1:HIGH)
 *
 * (1) The tsadc control power sequence.
 *
 * (2) Set TSADC_V2_AUTO_PERIOD:
 *     Configure the interleave between every two accessing of
 *     TSADC in normal operation.
 *
 * (2) Set TSADCV2_AUTO_PERIOD_HT:
 *     Configure the interleave between every two accessing of
 *     TSADC after the temperature is higher than COM_SHUT or COM_INT.
 *
 * (3) Set TSADCV2_HIGH_INT_DEBOUNCE and TSADC_HIGHT_TSHUT_DEBOUNCE:
 *     If the temperature is higher than COMP_INT or COMP_SHUT for
 *     "debounce" times, TSADC controller will generate interrupt or TSHUT.
 */
static void rk_tsadcv3_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	/* The tsadc control power sequence */
	if (IS_ERR(grf)) {
		/* Set interleave value to workround ic time sync issue */
		writel_relaxed(TSADCV2_USER_INTER_PD_SOC, regs +
			       TSADCV2_USER_CON);

		writel_relaxed(TSADCV2_AUTO_PERIOD_TIME,
			       regs + TSADCV2_AUTO_PERIOD);
		writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
		writel_relaxed(TSADCV2_AUTO_PERIOD_HT_TIME,
			       regs + TSADCV2_AUTO_PERIOD_HT);
		writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);

	} else {
		/* Enable the voltage common mode feature */
		regmap_write(grf, GRF_TSADC_TESTBIT_L, GRF_TSADC_VCM_EN_L);
		regmap_write(grf, GRF_TSADC_TESTBIT_H, GRF_TSADC_VCM_EN_H);

		usleep_range(15, 100); /* The spec note says at least 15 us */
		regmap_write(grf, GRF_SARADC_TESTBIT, GRF_SARADC_TESTBIT_ON);
		regmap_write(grf, GRF_TSADC_TESTBIT_H, GRF_TSADC_TESTBIT_H_ON);
		usleep_range(90, 200); /* The spec note says at least 90 us */

		writel_relaxed(TSADCV3_AUTO_PERIOD_TIME,
			       regs + TSADCV2_AUTO_PERIOD);
		writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
		writel_relaxed(TSADCV3_AUTO_PERIOD_HT_TIME,
			       regs + TSADCV2_AUTO_PERIOD_HT);
		writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
			       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);
	}

	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(0U | TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(0U & ~TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
}

static void rk_tsadcv4_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	rk_tsadcv2_initialize(grf, regs, tshut_polarity);
	regmap_write(grf, PX30_GRF_SOC_CON2, GRF_CON_TSADC_CH_INV);
}

static void rk_tsadcv7_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	writel_relaxed(TSADCV5_USER_INTER_PD_SOC, regs + TSADCV2_USER_CON);
	writel_relaxed(TSADCV5_AUTO_PERIOD_TIME, regs + TSADCV2_AUTO_PERIOD);
	writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_INT_DEBOUNCE);
	writel_relaxed(TSADCV5_AUTO_PERIOD_HT_TIME,
		       regs + TSADCV2_AUTO_PERIOD_HT);
	writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
		       regs + TSADCV2_HIGHT_TSHUT_DEBOUNCE);

	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(0U | TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(0U & ~TSADCV2_AUTO_TSHUT_POLARITY_HIGH,
			       regs + TSADCV2_AUTO_CON);

	/*
	 * The general register file will is optional
	 * and might not be available.
	 */
	if (!IS_ERR(grf)) {
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_TSEN);
		/*
		 * RK3568 TRM, section 18.5. requires a delay no less
		 * than 10us between the rising edge of tsadc_tsen_en
		 * and the rising edge of tsadc_ana_reg_0/1/2.
		 */
		udelay(15);
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_ANA_REG0);
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_ANA_REG1);
		regmap_write(grf, RK3568_GRF_TSADC_CON, RK3568_GRF_TSADC_ANA_REG2);

		/*
		 * RK3568 TRM, section 18.5. requires a delay no less
		 * than 90us after the rising edge of tsadc_ana_reg_0/1/2.
		 */
		usleep_range(100, 200);
	}
}

static void rk_tsadcv8_initialize(struct regmap *grf, void __iomem *regs,
				  enum tshut_polarity tshut_polarity)
{
	writel_relaxed(TSADCV6_AUTO_PERIOD_TIME, regs + TSADCV3_AUTO_PERIOD);
	writel_relaxed(TSADCV6_AUTO_PERIOD_HT_TIME,
		       regs + TSADCV3_AUTO_PERIOD_HT);
	writel_relaxed(TSADCV2_HIGHT_INT_DEBOUNCE_COUNT,
		       regs + TSADCV3_HIGHT_INT_DEBOUNCE);
	writel_relaxed(TSADCV2_HIGHT_TSHUT_DEBOUNCE_COUNT,
		       regs + TSADCV3_HIGHT_TSHUT_DEBOUNCE);
	if (tshut_polarity == TSHUT_HIGH_ACTIVE)
		writel_relaxed(TSADCV2_AUTO_TSHUT_POLARITY_HIGH |
			       TSADCV2_AUTO_TSHUT_POLARITY_MASK,
			       regs + TSADCV2_AUTO_CON);
	else
		writel_relaxed(TSADCV2_AUTO_TSHUT_POLARITY_MASK,
			       regs + TSADCV2_AUTO_CON);
}

static void rk_tsadcv2_irq_ack(void __iomem *regs)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_INT_PD);
	writel_relaxed(val & TSADCV2_INT_PD_CLEAR_MASK, regs + TSADCV2_INT_PD);
}

static void rk_tsadcv3_irq_ack(void __iomem *regs)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_INT_PD);
	writel_relaxed(val & TSADCV3_INT_PD_CLEAR_MASK, regs + TSADCV2_INT_PD);
}

static void rk_tsadcv4_irq_ack(void __iomem *regs)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV3_INT_PD);
	writel_relaxed(val & TSADCV4_INT_PD_CLEAR_MASK, regs + TSADCV3_INT_PD);
	val = readl_relaxed(regs + TSADCV3_HSHUT_PD);
	writel_relaxed(val & TSADCV3_INT_PD_CLEAR_MASK,
		       regs + TSADCV3_HSHUT_PD);
}

static void rk_tsadcv2_control(void __iomem *regs, bool enable)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_AUTO_CON);
	if (enable)
		val |= TSADCV2_AUTO_EN;
	else
		val &= ~TSADCV2_AUTO_EN;

	writel_relaxed(val, regs + TSADCV2_AUTO_CON);
}

/**
 * rk_tsadcv3_control - the tsadc controller is enabled or disabled.
 * @regs: the base address of tsadc controller
 * @enable: boolean flag to enable the controller
 *
 * NOTE: TSADC controller works at auto mode, and some SoCs need set the
 * tsadc_q_sel bit on TSADCV2_AUTO_CON[1]. The (1024 - tsadc_q) as output
 * adc value if setting this bit to enable.
 */
static void rk_tsadcv3_control(void __iomem *regs, bool enable)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_AUTO_CON);
	if (enable)
		val |= TSADCV2_AUTO_EN | TSADCV3_AUTO_Q_SEL_EN;
	else
		val &= ~TSADCV2_AUTO_EN;

	writel_relaxed(val, regs + TSADCV2_AUTO_CON);
}

static void rk_tsadcv4_control(void __iomem *regs, bool enable)
{
	u32 val;

	if (enable)
		val = TSADCV2_AUTO_EN | TSADCV2_AUTO_EN_MASK;
	else
		val = TSADCV2_AUTO_EN_MASK;

	writel_relaxed(val, regs + TSADCV2_AUTO_CON);
}

static int rk_tsadcv2_get_temp(const struct chip_tsadc_table *table,
			       int chn, void __iomem *regs, int *temp)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_DATA(chn));

	return rk_tsadcv2_code_to_temp(table, val, temp);
}

static int rk_tsadcv4_get_temp(const struct chip_tsadc_table *table,
			       int chn, void __iomem *regs, int *temp)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV3_DATA(chn));

	return rk_tsadcv2_code_to_temp(table, val, temp);
}

static int rk_tsadcv2_alarm_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 alarm_value;
	u32 int_en, int_clr;

	/*
	 * In some cases, some sensors didn't need the trip points, the
	 * set_trips will pass {-INT_MAX, INT_MAX} to trigger tsadc alarm
	 * in the end, ignore this case and disable the high temperature
	 * interrupt.
	 */
	if (temp == INT_MAX) {
		int_clr = readl_relaxed(regs + TSADCV2_INT_EN);
		int_clr &= ~TSADCV2_INT_SRC_EN(chn);
		writel_relaxed(int_clr, regs + TSADCV2_INT_EN);
		return 0;
	}

	/* Make sure the value is valid */
	alarm_value = rk_tsadcv2_temp_to_code(table, temp);
	if (alarm_value == table->data_mask)
		return -ERANGE;

	writel_relaxed(alarm_value & table->data_mask,
		       regs + TSADCV2_COMP_INT(chn));

	int_en = readl_relaxed(regs + TSADCV2_INT_EN);
	int_en |= TSADCV2_INT_SRC_EN(chn);
	writel_relaxed(int_en, regs + TSADCV2_INT_EN);

	return 0;
}

static int rk_tsadcv3_alarm_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 alarm_value;

	/*
	 * In some cases, some sensors didn't need the trip points, the
	 * set_trips will pass {-INT_MAX, INT_MAX} to trigger tsadc alarm
	 * in the end, ignore this case and disable the high temperature
	 * interrupt.
	 */
	if (temp == INT_MAX) {
		writel_relaxed(TSADCV2_INT_SRC_EN_MASK(chn),
			       regs + TSADCV3_HT_INT_EN);
		return 0;
	}
	/* Make sure the value is valid */
	alarm_value = rk_tsadcv2_temp_to_code(table, temp);
	if (alarm_value == table->data_mask)
		return -ERANGE;
	writel_relaxed(alarm_value & table->data_mask,
		       regs + TSADCV3_COMP_INT(chn));
	writel_relaxed(TSADCV2_INT_SRC_EN(chn) | TSADCV2_INT_SRC_EN_MASK(chn),
		       regs + TSADCV3_HT_INT_EN);
	return 0;
}

static int rk_tsadcv2_tshut_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 tshut_value, val;

	/* Make sure the value is valid */
	tshut_value = rk_tsadcv2_temp_to_code(table, temp);
	if (tshut_value == table->data_mask)
		return -ERANGE;

	writel_relaxed(tshut_value, regs + TSADCV2_COMP_SHUT(chn));

	/* TSHUT will be valid */
	val = readl_relaxed(regs + TSADCV2_AUTO_CON);
	writel_relaxed(val | TSADCV2_AUTO_SRC_EN(chn), regs + TSADCV2_AUTO_CON);

	return 0;
}

static int rk_tsadcv3_tshut_temp(const struct chip_tsadc_table *table,
				 int chn, void __iomem *regs, int temp)
{
	u32 tshut_value;

	/* Make sure the value is valid */
	tshut_value = rk_tsadcv2_temp_to_code(table, temp);
	if (tshut_value == table->data_mask)
		return -ERANGE;

	writel_relaxed(tshut_value, regs + TSADCV3_COMP_SHUT(chn));

	/* TSHUT will be valid */
	writel_relaxed(TSADCV3_AUTO_SRC_EN(chn) | TSADCV3_AUTO_SRC_EN_MASK(chn),
		       regs + TSADCV3_AUTO_SRC_CON);

	return 0;
}

static void rk_tsadcv2_tshut_mode(int chn, void __iomem *regs,
				  enum tshut_mode mode)
{
	u32 val;

	val = readl_relaxed(regs + TSADCV2_INT_EN);
	if (mode == TSHUT_MODE_GPIO) {
		val &= ~TSADCV2_SHUT_2CRU_SRC_EN(chn);
		val |= TSADCV2_SHUT_2GPIO_SRC_EN(chn);
	} else {
		val &= ~TSADCV2_SHUT_2GPIO_SRC_EN(chn);
		val |= TSADCV2_SHUT_2CRU_SRC_EN(chn);
	}

	writel_relaxed(val, regs + TSADCV2_INT_EN);
}

static void rk_tsadcv4_tshut_mode(int chn, void __iomem *regs,
				  enum tshut_mode mode)
{
	u32 val_gpio, val_cru;

	if (mode == TSHUT_MODE_GPIO) {
		val_gpio = TSADCV2_INT_SRC_EN(chn) | TSADCV2_INT_SRC_EN_MASK(chn);
		val_cru = TSADCV2_INT_SRC_EN_MASK(chn);
	} else {
		val_cru = TSADCV2_INT_SRC_EN(chn) | TSADCV2_INT_SRC_EN_MASK(chn);
		val_gpio = TSADCV2_INT_SRC_EN_MASK(chn);
	}
	writel_relaxed(val_gpio, regs + TSADCV3_HSHUT_GPIO_INT_EN);
	writel_relaxed(val_cru, regs + TSADCV3_HSHUT_CRU_INT_EN);
}

static int rk_tsadcv2_get_trim_code(const struct chip_tsadc_table *table,
				    int code, int trim_base, int trim_base_frac)
{
	int temp = trim_base * 1000 + trim_base_frac * 100;
	u32 base_code = rk_tsadcv2_temp_to_code(table, temp);

	return code - base_code;
}

static const struct rockchip_tsadc_chip px30_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* 2 channels for tsadc */

	.tshut_mode = TSHUT_MODE_CRU, /* default TSHUT via CRU */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv4_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3328_code_table,
		.length = ARRAY_SIZE(rk3328_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rv1108_tsadc_data = {
	/* cpu */
	.chn_offset = 0,
	.chn_num = 1, /* one channel for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rv1108_table,
		.length = ARRAY_SIZE(rv1108_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3228_tsadc_data = {
	/* cpu */
	.chn_offset = 0,
	.chn_num = 1, /* one channel for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3228_code_table,
		.length = ARRAY_SIZE(rk3228_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3288_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 1,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv2_irq_ack,
	.control = rk_tsadcv2_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3288_code_table,
		.length = ARRAY_SIZE(rk3288_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_DECREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3328_tsadc_data = {
	/* cpu */
	.chn_offset = 0,
	.chn_num = 1, /* one channels for tsadc */

	.tshut_mode = TSHUT_MODE_CRU, /* default TSHUT via CRU */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3328_code_table,
		.length = ARRAY_SIZE(rk3328_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3366_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv3_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3228_code_table,
		.length = ARRAY_SIZE(rk3228_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3368_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv2_initialize,
	.irq_ack = rk_tsadcv2_irq_ack,
	.control = rk_tsadcv2_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3368_code_table,
		.length = ARRAY_SIZE(rk3368_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3399_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv3_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3399_code_table,
		.length = ARRAY_SIZE(rk3399_code_table),
		.data_mask = TSADCV3_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3568_tsadc_data = {
	/* cpu, gpu */
	.chn_offset = 0,
	.chn_num = 2, /* two channels for tsadc */

	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,

	.initialize = rk_tsadcv7_initialize,
	.irq_ack = rk_tsadcv3_irq_ack,
	.control = rk_tsadcv3_control,
	.get_temp = rk_tsadcv2_get_temp,
	.set_alarm_temp = rk_tsadcv2_alarm_temp,
	.set_tshut_temp = rk_tsadcv2_tshut_temp,
	.set_tshut_mode = rk_tsadcv2_tshut_mode,

	.table = {
		.id = rk3568_code_table,
		.length = ARRAY_SIZE(rk3568_code_table),
		.data_mask = TSADCV2_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3576_tsadc_data = {
	/* top, big_core, little_core, ddr, npu, gpu */
	.chn_offset = 0,
	.chn_num = 6, /* six channels for tsadc */
	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,
	.initialize = rk_tsadcv8_initialize,
	.irq_ack = rk_tsadcv4_irq_ack,
	.control = rk_tsadcv4_control,
	.get_temp = rk_tsadcv4_get_temp,
	.set_alarm_temp = rk_tsadcv3_alarm_temp,
	.set_tshut_temp = rk_tsadcv3_tshut_temp,
	.set_tshut_mode = rk_tsadcv4_tshut_mode,
	.get_trim_code = rk_tsadcv2_get_trim_code,
	.trim_slope = 923,
	.table = {
		.id = rk3588_code_table,
		.length = ARRAY_SIZE(rk3588_code_table),
		.data_mask = TSADCV4_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct rockchip_tsadc_chip rk3588_tsadc_data = {
	/* top, big_core0, big_core1, little_core, center, gpu, npu */
	.chn_offset = 0,
	.chn_num = 7, /* seven channels for tsadc */
	.tshut_mode = TSHUT_MODE_GPIO, /* default TSHUT via GPIO give PMIC */
	.tshut_polarity = TSHUT_LOW_ACTIVE, /* default TSHUT LOW ACTIVE */
	.tshut_temp = 95000,
	.initialize = rk_tsadcv8_initialize,
	.irq_ack = rk_tsadcv4_irq_ack,
	.control = rk_tsadcv4_control,
	.get_temp = rk_tsadcv4_get_temp,
	.set_alarm_temp = rk_tsadcv3_alarm_temp,
	.set_tshut_temp = rk_tsadcv3_tshut_temp,
	.set_tshut_mode = rk_tsadcv4_tshut_mode,
	.table = {
		.id = rk3588_code_table,
		.length = ARRAY_SIZE(rk3588_code_table),
		.data_mask = TSADCV4_DATA_MASK,
		.mode = ADC_INCREMENT,
	},
};

static const struct of_device_id of_rockchip_thermal_match[] = {
	{	.compatible = "rockchip,px30-tsadc",
		.data = (void *)&px30_tsadc_data,
	},
	{
		.compatible = "rockchip,rv1108-tsadc",
		.data = (void *)&rv1108_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3228-tsadc",
		.data = (void *)&rk3228_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3288-tsadc",
		.data = (void *)&rk3288_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3328-tsadc",
		.data = (void *)&rk3328_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3366-tsadc",
		.data = (void *)&rk3366_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3368-tsadc",
		.data = (void *)&rk3368_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3399-tsadc",
		.data = (void *)&rk3399_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3568-tsadc",
		.data = (void *)&rk3568_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3576-tsadc",
		.data = (void *)&rk3576_tsadc_data,
	},
	{
		.compatible = "rockchip,rk3588-tsadc",
		.data = (void *)&rk3588_tsadc_data,
	},
	{ /* end */ },
};
MODULE_DEVICE_TABLE(of, of_rockchip_thermal_match);

static void
rockchip_thermal_toggle_sensor(struct rockchip_thermal_sensor *sensor, bool on)
{
	struct thermal_zone_device *tzd = sensor->tzd;

	if (on)
		thermal_zone_device_enable(tzd);
	else
		thermal_zone_device_disable(tzd);
}

static irqreturn_t rockchip_thermal_alarm_irq_thread(int irq, void *dev)
{
	struct rockchip_thermal_data *thermal = dev;
	int i;

	dev_dbg(&thermal->pdev->dev, "thermal alarm\n");

	thermal->chip->irq_ack(thermal->regs);

	for (i = 0; i < thermal->chip->chn_num; i++)
		thermal_zone_device_update(thermal->sensors[i].tzd,
					   THERMAL_EVENT_UNSPECIFIED);

	return IRQ_HANDLED;
}

static int rockchip_thermal_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	struct rockchip_thermal_sensor *sensor = thermal_zone_device_priv(tz);
	struct rockchip_thermal_data *thermal = sensor->thermal;
	const struct rockchip_tsadc_chip *tsadc = thermal->chip;

	dev_dbg(&thermal->pdev->dev, "%s: sensor %d: low: %d, high %d\n",
		__func__, sensor->id, low, high);

	return tsadc->set_alarm_temp(&tsadc->table,
				     sensor->id, thermal->regs, high + sensor->trim_temp);
}

static int rockchip_thermal_get_temp(struct thermal_zone_device *tz, int *out_temp)
{
	struct rockchip_thermal_sensor *sensor = thermal_zone_device_priv(tz);
	struct rockchip_thermal_data *thermal = sensor->thermal;
	const struct rockchip_tsadc_chip *tsadc = sensor->thermal->chip;
	int retval;

	retval = tsadc->get_temp(&tsadc->table,
				 sensor->id, thermal->regs, out_temp);
	*out_temp -= sensor->trim_temp;

	return retval;
}

static const struct thermal_zone_device_ops rockchip_of_thermal_ops = {
	.get_temp = rockchip_thermal_get_temp,
	.set_trips = rockchip_thermal_set_trips,
};

/**
 * rockchip_get_efuse_value - read an OTP cell from a device node
 * @np: pointer to the device node with the nvmem-cells property
 * @cell_name: name of cell that should be read
 * @value: pointer to where the read value will be placed
 *
 * Return: Negative errno on failure, during which *value will not be touched,
 * or 0 on success.
 */
static int rockchip_get_efuse_value(struct device_node *np, const char *cell_name,
				    int *value)
{
	struct nvmem_cell *cell;
	int ret = 0;
	size_t len;
	u8 *buf;
	int i;

	cell = of_nvmem_cell_get(np, cell_name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);

	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (len > sizeof(*value)) {
		ret = -ERANGE;
		goto exit;
	}

	/* Copy with implicit endian conversion */
	*value = 0;
	for (i = 0; i < len; i++)
		*value |= (int) buf[i] << (8 * i);

exit:
	kfree(buf);
	return ret;
}

static int rockchip_get_trim_configuration(struct device *dev, struct device_node *np,
					   struct rockchip_thermal_data *thermal)
{
	const struct rockchip_tsadc_chip *tsadc = thermal->chip;
	int trim_base = 0, trim_base_frac = 0, trim = 0;
	int trim_code;
	int ret;

	thermal->trim_base = 0;
	thermal->trim_base_frac = 0;
	thermal->trim = 0;

	if (!tsadc->get_trim_code)
		return 0;

	ret = rockchip_get_efuse_value(np, "trim_base", &trim_base);
	if (ret < 0) {
		if (ret == -ENOENT) {
			trim_base = 30;
			dev_dbg(dev, "trim_base is absent, defaulting to 30\n");
		} else {
			dev_err(dev, "failed reading nvmem value of trim_base: %pe\n",
				ERR_PTR(ret));
			return ret;
		}
	}
	ret = rockchip_get_efuse_value(np, "trim_base_frac", &trim_base_frac);
	if (ret < 0) {
		if (ret == -ENOENT) {
			dev_dbg(dev, "trim_base_frac is absent, defaulting to 0\n");
		} else {
			dev_err(dev, "failed reading nvmem value of trim_base_frac: %pe\n",
				ERR_PTR(ret));
			return ret;
		}
	}
	thermal->trim_base = trim_base;
	thermal->trim_base_frac = trim_base_frac;

	/*
	 * If the tsadc node contains the trim property, then it is used in the
	 * absence of per-channel trim values
	 */
	if (!rockchip_get_efuse_value(np, "trim", &trim))
		thermal->trim = trim;
	if (trim) {
		trim_code = tsadc->get_trim_code(&tsadc->table, trim,
						 trim_base, trim_base_frac);
		thermal->trim_temp = thermal->chip->trim_slope * trim_code;
	}

	return 0;
}

static int rockchip_configure_from_dt(struct device *dev,
				      struct device_node *np,
				      struct rockchip_thermal_data *thermal)
{
	u32 shut_temp, tshut_mode, tshut_polarity;

	if (of_property_read_u32(np, "rockchip,hw-tshut-temp", &shut_temp)) {
		dev_warn(dev,
			 "Missing tshut temp property, using default %d\n",
			 thermal->chip->tshut_temp);
		thermal->tshut_temp = thermal->chip->tshut_temp;
	} else {
		if (shut_temp > INT_MAX) {
			dev_err(dev, "Invalid tshut temperature specified: %d\n",
				shut_temp);
			return -ERANGE;
		}
		thermal->tshut_temp = shut_temp;
	}

	if (of_property_read_u32(np, "rockchip,hw-tshut-mode", &tshut_mode)) {
		dev_warn(dev,
			 "Missing tshut mode property, using default (%s)\n",
			 thermal->chip->tshut_mode == TSHUT_MODE_GPIO ?
				"gpio" : "cru");
		thermal->tshut_mode = thermal->chip->tshut_mode;
	} else {
		thermal->tshut_mode = tshut_mode;
	}

	if (thermal->tshut_mode > 1) {
		dev_err(dev, "Invalid tshut mode specified: %d\n",
			thermal->tshut_mode);
		return -EINVAL;
	}

	if (of_property_read_u32(np, "rockchip,hw-tshut-polarity",
				 &tshut_polarity)) {
		dev_warn(dev,
			 "Missing tshut-polarity property, using default (%s)\n",
			 thermal->chip->tshut_polarity == TSHUT_LOW_ACTIVE ?
				"low" : "high");
		thermal->tshut_polarity = thermal->chip->tshut_polarity;
	} else {
		thermal->tshut_polarity = tshut_polarity;
	}

	if (thermal->tshut_polarity > 1) {
		dev_err(dev, "Invalid tshut-polarity specified: %d\n",
			thermal->tshut_polarity);
		return -EINVAL;
	}

	/* The tsadc wont to handle the error in here since some SoCs didn't
	 * need this property.
	 */
	thermal->grf = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(thermal->grf))
		dev_warn(dev, "Missing rockchip,grf property\n");

	rockchip_get_trim_configuration(dev, np, thermal);

	return 0;
}

static int
rockchip_thermal_register_sensor(struct platform_device *pdev,
				 struct rockchip_thermal_data *thermal,
				 struct rockchip_thermal_sensor *sensor,
				 int id)
{
	const struct rockchip_tsadc_chip *tsadc = thermal->chip;
	struct device *dev = &pdev->dev;
	int trim = thermal->trim;
	int trim_code, tshut_temp;
	int trim_temp = 0;
	int error;

	if (thermal->trim_temp)
		trim_temp = thermal->trim_temp;

	if (tsadc->get_trim_code && sensor->of_node) {
		error = rockchip_get_efuse_value(sensor->of_node, "trim", &trim);
		if (error < 0 && error != -ENOENT) {
			dev_err(dev, "failed reading trim of sensor %d: %pe\n",
				id, ERR_PTR(error));
			return error;
		}
		if (trim) {
			trim_code = tsadc->get_trim_code(&tsadc->table, trim,
							 thermal->trim_base,
							 thermal->trim_base_frac);
			trim_temp = thermal->chip->trim_slope * trim_code;
		}
	}

	sensor->trim_temp = trim_temp;

	dev_dbg(dev, "trim of sensor %d is %d\n", id, sensor->trim_temp);

	tshut_temp = min(thermal->tshut_temp + sensor->trim_temp, RK_MAX_TEMP);

	tsadc->set_tshut_mode(id, thermal->regs, thermal->tshut_mode);

	error = tsadc->set_tshut_temp(&tsadc->table, id, thermal->regs, tshut_temp);
	if (error)
		dev_err(dev, "%s: invalid tshut=%d, error=%d\n",
			__func__, tshut_temp, error);

	sensor->thermal = thermal;
	sensor->id = id;
	sensor->tzd = devm_thermal_of_zone_register(dev, id, sensor,
						    &rockchip_of_thermal_ops);
	if (IS_ERR(sensor->tzd)) {
		error = PTR_ERR(sensor->tzd);
		dev_err(dev, "failed to register sensor %d: %d\n",
			id, error);
		return error;
	}

	return 0;
}

/**
 * rockchip_thermal_reset_controller - Reset TSADC Controller, reset all tsadc registers.
 * @reset: the reset controller of tsadc
 */
static void rockchip_thermal_reset_controller(struct reset_control *reset)
{
	reset_control_assert(reset);
	usleep_range(10, 20);
	reset_control_deassert(reset);
}

static int rockchip_thermal_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct rockchip_thermal_data *thermal;
	struct device_node *child;
	int irq;
	int i;
	int error;
	u32 chn;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return -EINVAL;

	thermal = devm_kzalloc(&pdev->dev, sizeof(struct rockchip_thermal_data),
			       GFP_KERNEL);
	if (!thermal)
		return -ENOMEM;

	thermal->pdev = pdev;

	thermal->chip = device_get_match_data(&pdev->dev);
	if (!thermal->chip)
		return -EINVAL;

	thermal->sensors = devm_kcalloc(&pdev->dev, thermal->chip->chn_num,
					sizeof(*thermal->sensors), GFP_KERNEL);
	if (!thermal->sensors)
		return -ENOMEM;

	thermal->regs = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(thermal->regs))
		return PTR_ERR(thermal->regs);

	thermal->reset = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(thermal->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(thermal->reset),
				     "failed to get tsadc reset.\n");

	thermal->clk = devm_clk_get_enabled(&pdev->dev, "tsadc");
	if (IS_ERR(thermal->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(thermal->clk),
				     "failed to get tsadc clock.\n");

	thermal->pclk = devm_clk_get_enabled(&pdev->dev, "apb_pclk");
	if (IS_ERR(thermal->pclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(thermal->pclk),
				     "failed to get apb_pclk clock.\n");

	rockchip_thermal_reset_controller(thermal->reset);

	error = rockchip_configure_from_dt(&pdev->dev, np, thermal);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				"failed to parse device tree data\n");

	thermal->chip->initialize(thermal->grf, thermal->regs,
				  thermal->tshut_polarity);

	for_each_available_child_of_node(np, child) {
		if (!of_property_read_u32(child, "reg", &chn)) {
			if (chn < thermal->chip->chn_num)
				thermal->sensors[chn].of_node = child;
			else
				dev_warn(&pdev->dev,
					 "sensor address (%d) too large, ignoring its trim\n",
					 chn);
		}

	}

	for (i = 0; i < thermal->chip->chn_num; i++) {
		error = rockchip_thermal_register_sensor(pdev, thermal,
						&thermal->sensors[i],
						thermal->chip->chn_offset + i);
		if (error)
			return dev_err_probe(&pdev->dev, error,
				"failed to register sensor[%d].\n", i);
	}

	error = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					  &rockchip_thermal_alarm_irq_thread,
					  IRQF_ONESHOT,
					  "rockchip_thermal", thermal);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				     "failed to request tsadc irq.\n");

	thermal->chip->control(thermal->regs, true);

	for (i = 0; i < thermal->chip->chn_num; i++) {
		rockchip_thermal_toggle_sensor(&thermal->sensors[i], true);
		error = thermal_add_hwmon_sysfs(thermal->sensors[i].tzd);
		if (error)
			dev_warn(&pdev->dev,
				 "failed to register sensor %d with hwmon: %d\n",
				 i, error);
	}

	platform_set_drvdata(pdev, thermal);

	return 0;
}

static void rockchip_thermal_remove(struct platform_device *pdev)
{
	struct rockchip_thermal_data *thermal = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < thermal->chip->chn_num; i++) {
		struct rockchip_thermal_sensor *sensor = &thermal->sensors[i];

		thermal_remove_hwmon_sysfs(sensor->tzd);
		rockchip_thermal_toggle_sensor(sensor, false);
	}

	thermal->chip->control(thermal->regs, false);
}

static int __maybe_unused rockchip_thermal_suspend(struct device *dev)
{
	struct rockchip_thermal_data *thermal = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < thermal->chip->chn_num; i++)
		rockchip_thermal_toggle_sensor(&thermal->sensors[i], false);

	thermal->chip->control(thermal->regs, false);

	clk_disable(thermal->pclk);
	clk_disable(thermal->clk);

	pinctrl_pm_select_sleep_state(dev);

	return 0;
}

static int __maybe_unused rockchip_thermal_resume(struct device *dev)
{
	struct rockchip_thermal_data *thermal = dev_get_drvdata(dev);
	const struct rockchip_tsadc_chip *tsadc = thermal->chip;
	struct rockchip_thermal_sensor *sensor;
	int tshut_temp;
	int error;
	int i;

	error = clk_enable(thermal->clk);
	if (error)
		return error;

	error = clk_enable(thermal->pclk);
	if (error) {
		clk_disable(thermal->clk);
		return error;
	}

	rockchip_thermal_reset_controller(thermal->reset);

	tsadc->initialize(thermal->grf, thermal->regs, thermal->tshut_polarity);

	for (i = 0; i < thermal->chip->chn_num; i++) {
		sensor = &thermal->sensors[i];

		tshut_temp = min(thermal->tshut_temp + sensor->trim_temp,
				 RK_MAX_TEMP);

		tsadc->set_tshut_mode(sensor->id, thermal->regs,
					      thermal->tshut_mode);

		error = tsadc->set_tshut_temp(&thermal->chip->table,
					      sensor->id, thermal->regs,
					      tshut_temp);
		if (error)
			dev_err(dev, "%s: invalid tshut=%d, error=%d\n",
				__func__, tshut_temp, error);
	}

	thermal->chip->control(thermal->regs, true);

	for (i = 0; i < thermal->chip->chn_num; i++)
		rockchip_thermal_toggle_sensor(&thermal->sensors[i], true);

	pinctrl_pm_select_default_state(dev);

	return 0;
}

static SIMPLE_DEV_PM_OPS(rockchip_thermal_pm_ops,
			 rockchip_thermal_suspend, rockchip_thermal_resume);

static struct platform_driver rockchip_thermal_driver = {
	.driver = {
		.name = "rockchip-thermal",
		.pm = &rockchip_thermal_pm_ops,
		.of_match_table = of_rockchip_thermal_match,
	},
	.probe = rockchip_thermal_probe,
	.remove = rockchip_thermal_remove,
};

module_platform_driver(rockchip_thermal_driver);

MODULE_DESCRIPTION("ROCKCHIP THERMAL Driver");
MODULE_AUTHOR("Rockchip, Inc.");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:rockchip-thermal");
