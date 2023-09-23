// SPDX-License-Identifier: GPL-2.0-only
/*
 * cyttsp4_platform.c
 * Cypress TrueTouch(TM) Standard Product V4 Platform Module.
 * For use with Cypress Txx4xx parts.
 * Supported parts include:
 * TMA4XX
 * TMA1036
 *
 * Copyright (C) 2013 Cypress Semiconductor
 *
 * Contact Cypress Semiconductor at www.cypress.com <ttdrivers@cypress.com>
 */

#include <linux/regulator/consumer.h>
#include "cyttsp4_regs.h"

#ifdef CYTTSP4_PLATFORM_FW_UPGRADE
#include "cyttsp4_heat_fw.h"
static struct cyttsp4_touch_firmware cyttsp4_firmware = {
	.img = cyttsp4_img,
	.size = ARRAY_SIZE(cyttsp4_img),
	.ver = cyttsp4_ver,
	.vsize = ARRAY_SIZE(cyttsp4_ver),
	.hw_version = 0x02,
	.fw_version = 0x0900,
	.cfg_version = 0x09,
};
#else
static struct cyttsp4_touch_firmware cyttsp4_firmware = {
	.img = NULL,
	.size = 0,
	.ver = NULL,
	.vsize = 0,
};
#endif

#ifdef CYTTSP4_PLATFORM_TTCONFIG_UPGRADE
#include "cyttsp4_params.h"
static struct touch_settings cyttsp4_sett_param_regs = {
	.data = (uint8_t *)&cyttsp4_param_regs[0],
	.size = ARRAY_SIZE(cyttsp4_param_regs),
	.tag = 0,
};

static struct touch_settings cyttsp4_sett_param_size = {
	.data = (uint8_t *)&cyttsp4_param_size[0],
	.size = ARRAY_SIZE(cyttsp4_param_size),
	.tag = 0,
};

static struct cyttsp4_touch_config cyttsp4_ttconfig = {
	.param_regs = &cyttsp4_sett_param_regs,
	.param_size = &cyttsp4_sett_param_size,
	.fw_ver = ttconfig_fw_ver,
	.fw_vsize = ARRAY_SIZE(ttconfig_fw_ver),
};
#else
static struct cyttsp4_touch_config cyttsp4_ttconfig = {
	.param_regs = NULL,
	.param_size = NULL,
	.fw_ver = NULL,
	.fw_vsize = 0,
};
#endif

struct cyttsp4_loader_platform_data _cyttsp4_loader_platform_data = {
	.fw = &cyttsp4_firmware,
	.ttconfig = &cyttsp4_ttconfig,
	.flags = CY_LOADER_FLAG_CALIBRATE_AFTER_FW_UPGRADE,
};

static int pinctrl_init(struct cyttsp4_core_platform_data *pdata)
{
	int retval;

	pdata->gpio_state_active
		= pinctrl_lookup_state(pdata->ts_pinctrl, "pmx_ts_active");
	if (IS_ERR_OR_NULL(pdata->gpio_state_active)) {
		pr_err("%s: Can not get ts default pinstate\n",
			CYTTSP4_I2C_NAME);
		retval = PTR_ERR(pdata->gpio_state_active);
		pdata->ts_pinctrl = NULL;
		return retval;
	}

	pdata->gpio_state_suspend
		= pinctrl_lookup_state(pdata->ts_pinctrl, "pmx_ts_suspend");
	if (IS_ERR_OR_NULL(pdata->gpio_state_suspend)) {
		pr_err("%s: Can not get ts sleep pinstate\n",
			CYTTSP4_I2C_NAME);
		retval = PTR_ERR(pdata->gpio_state_suspend);
		pdata->ts_pinctrl = NULL;
		return retval;
	}

	return 0;
}

static int cyttsp4_pinctrl_select(struct cyttsp4_core_platform_data *pdata,
				  bool on)
{
	struct pinctrl_state *pins_state;
	int ret;

	pins_state = on ? pdata->gpio_state_active
		: pdata->gpio_state_suspend;
	if (!IS_ERR_OR_NULL(pins_state)) {
		ret = pinctrl_select_state(pdata->ts_pinctrl, pins_state);
		if (ret) {
			pr_err("%s: can not set %s pins\n",
				CYTTSP4_I2C_NAME,
				on ? "pmx_ts_active" : "pmx_ts_suspend");
			return ret;
		}
	} else
		pr_err("%s: not a valid '%s' pinstate\n",
			CYTTSP4_I2C_NAME,
			on ? "pmx_ts_active" : "pmx_ts_suspend");

	return 0;
}

/*************************************************************************************************
 * Power
 *************************************************************************************************/

static int cy_hw_power(struct cyttsp4_core_platform_data *pdata, int on)
{
	int ret = 0;

	pr_debug("%s: power %s\n", CYTTSP4_I2C_NAME, on ? "on" : "off");

	ret = gpio_direction_output(pdata->vddo_gpio, on);
	if (ret) {
		pr_err("%s: %s: unable to set_direction for gpio[%d] %d\n",
			 CYTTSP4_I2C_NAME, __func__, pdata->vddo_gpio, ret);
		return -EINVAL;
	}
	cyttsp4_pinctrl_select(pdata, on);

	ret = gpio_direction_output(pdata->avdd_gpio, on);
	if (ret) {
		pr_err("%s: %s: unable to set_direction for gpio[%d] %d\n",
			 CYTTSP4_I2C_NAME, __func__, pdata->avdd_gpio, ret);
		return -EINVAL;
	}
	msleep(50);
	return 0;
}
/*************************************************************************************************
 *
 *************************************************************************************************/
int cyttsp4_xres(struct cyttsp4_core_platform_data *pdata,
		struct device *dev)
{
	int irq_gpio = pdata->irq_gpio;

	dev_dbg(dev, "%s: The TOUCH IRQ no in cyttsp4_xres() is %d",
		__func__, irq_gpio);

	cy_hw_power(pdata, 0);
	cy_hw_power(pdata, 1);

	return 0;
}

int cyttsp4_init(struct cyttsp4_core_platform_data *pdata,
		int on, struct device *dev)
{
	int rc = 0;
	int irq_gpio = pdata->irq_gpio;

	if (on) {
		pinctrl_init(pdata);
		rc = gpio_request(irq_gpio, "TSP_INT");
		if (rc < 0) {
			dev_err(dev, "%s: unable to request TSP_INT\n", __func__);
			return rc;
		}
		gpio_direction_input(irq_gpio);
		rc = gpio_request(pdata->avdd_gpio, "TSP_AVDD_gpio");
		if (rc < 0) {
			dev_err(dev, "%s: unable to request TSP_AVDD_gpio\n", __func__);
			return rc;
		}
		rc = gpio_request(pdata->vddo_gpio, "TSP_VDDO_gpio");
		if (rc < 0) {
			dev_err(dev, "%s: unable to request TSP_VDDO_gpio\n", __func__);
			return rc;
		}

		cy_hw_power(pdata, 1);
	} else {
		cy_hw_power(pdata, 0);
		gpio_free(irq_gpio);
	}
	dev_info(dev,
		"%s: INIT CYTTSP IRQ gpio=%d onoff=%d r=%d\n",
		__func__, irq_gpio, on, rc);
	return rc;
}

static int cyttsp4_wakeup(struct cyttsp4_core_platform_data *pdata,
		struct device *dev, atomic_t *ignore_irq)
{
	return cy_hw_power(pdata, 1);
}

static int cyttsp4_sleep(struct cyttsp4_core_platform_data *pdata,
		struct device *dev, atomic_t *ignore_irq)
{
	return cy_hw_power(pdata, 0);
}

int cyttsp4_power(struct cyttsp4_core_platform_data *pdata,
		int on, struct device *dev, atomic_t *ignore_irq)
{
	if (on)
		return cyttsp4_wakeup(pdata, dev, ignore_irq);

	return cyttsp4_sleep(pdata, dev, ignore_irq);
}

int cyttsp4_irq_stat(struct cyttsp4_core_platform_data *pdata,
		struct device *dev)
{
	return gpio_get_value(pdata->irq_gpio);
}
