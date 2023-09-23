// SPDX-License-Identifier: GPL-2.0-only
/*
 * cyttsp4_core.h
 * Cypress TrueTouch(TM) Standard Product V4 Core driver module.
 * For use with Cypress Txx4xx parts.
 * Supported parts include:
 * TMA4XX
 * TMA1036
 *
 * Copyright (C) 2012 Cypress Semiconductor
 * Copyright (C) 2011 Sony Ericsson Mobile Communications AB.
 *
 * Author: Aleksej Makarov <aleksej.makarov@sonyericsson.com>
 * Modifed by: Cypress Semiconductor to add touch settings
 *
 * Contact Cypress Semiconductor at www.cypress.com <ttdrivers@cypress.com>
 */

#ifndef _CYTTSP4_H_
#define _CYTTSP4_H_

#include <linux/stringify.h>

#define CYTTSP4_CORE_NAME "cyttsp4_core"
#define CYTTSP4_MT_NAME "cyttsp4_mt"
#define CYTTSP4_I2C_NAME "cyttsp4_i2c_adapter"
#define CYTTSP4_SPI_NAME "cyttsp4_spi_adapter"

#define CY_DRIVER_NAME TTDA
#define CY_DRIVER_MAJOR 02
#define CY_DRIVER_MINOR 04

#define CY_DRIVER_REVCTRL 600162

#define CY_DRIVER_VERSION		    \
__stringify(CY_DRIVER_NAME)		    \
"." __stringify(CY_DRIVER_MAJOR)	    \
"." __stringify(CY_DRIVER_MINOR)	    \
"." __stringify(CY_DRIVER_REVCTRL)

#define CY_DRIVER_DATE "20140218"	/* YYYYMMDD */

/* abs settings */
#define CY_IGNORE_VALUE             0xFFFF

enum cyttsp4_core_platform_flags {
	CY_CORE_FLAG_NONE		= 0,
	CY_CORE_FLAG_WAKE_ON_GESTURE	= (1 << 0),
	CY_CORE_FLAG_POWEROFF_ON_SLEEP	= (1 << 1),
	/* choose SCAN_TYPE or TOUCH_MODE RAM ID to alter scan type */
	CY_CORE_FLAG_SCAN_MODE_USES_RAM_ID_SCAN_TYPE = (1 << 2),
};

enum cyttsp4_loader_platform_flags {
	CY_LOADER_FLAG_NONE,
	CY_LOADER_FLAG_CALIBRATE_AFTER_FW_UPGRADE,
	/* Use CONFIG_VER field in TT_CFG to decide TT_CFG update */
	CY_LOADER_FLAG_CHECK_TTCONFIG_VERSION,
};

struct touch_settings {
	const uint8_t *data;
	uint32_t size;
	uint8_t tag;
} __packed;

struct cyttsp4_touch_firmware {
	const uint8_t *img;
	u32 size;
	const uint8_t *ver;
	u8 vsize;
	u8 hw_version;
	u16 fw_version;
	u8 cfg_version;
} __packed;

struct cyttsp4_touch_config {
	struct touch_settings *param_regs;
	struct touch_settings *param_size;
	const uint8_t *fw_ver;
	uint8_t fw_vsize;
};

struct cyttsp4_loader_platform_data {
	struct cyttsp4_touch_firmware *fw;
	struct cyttsp4_touch_config *ttconfig;
	const char *sdcard_path;
	u32 flags;
} __packed;

typedef int (*cyttsp4_platform_read) (struct device *dev, u16 addr,
	void *buf, int size);

#define CY_TOUCH_SETTINGS_MAX 32

struct cyttsp4_core_platform_data {
	int irq_gpio;
	int rst_gpio;
	int level_irq_udelay;
	int max_xfer_len;
	int (*xres)(struct cyttsp4_core_platform_data *pdata,
		struct device *dev);
	int (*init)(struct cyttsp4_core_platform_data *pdata,
		int on, struct device *dev);
	int (*power)(struct cyttsp4_core_platform_data *pdata,
		int on, struct device *dev, atomic_t *ignore_irq);
	int (*detect)(struct cyttsp4_core_platform_data *pdata,
		struct device *dev, cyttsp4_platform_read read);
	int (*irq_stat)(struct cyttsp4_core_platform_data *pdata,
		struct device *dev);
	struct touch_settings *sett[CY_TOUCH_SETTINGS_MAX];
	struct cyttsp4_loader_platform_data *loader_pdata;
	u32 flags;
	u8 easy_wakeup_gesture;
	int avdd_gpio;
	int vddo_gpio;
	struct pinctrl *ts_pinctrl;
	struct pinctrl_state *gpio_state_active;
	struct pinctrl_state *gpio_state_suspend;
};

struct touch_framework {
	const uint16_t  *abs;
	uint8_t         size;
	uint8_t         enable_vkeys;
} __packed;

enum cyttsp4_mt_platform_flags {
	CY_MT_FLAG_NONE = 0x00,
	CY_MT_FLAG_HOVER = 0x04,
	CY_MT_FLAG_FLIP = 0x08,
	CY_MT_FLAG_INV_X = 0x10,
	CY_MT_FLAG_INV_Y = 0x20,
	CY_MT_FLAG_VKEYS = 0x40,
	CY_MT_FLAG_NO_TOUCH_ON_LO = 0x80,
};

struct cyttsp4_mt_platform_data {
	struct touch_framework *frmwrk;
	unsigned short flags;
	char const *inp_dev_name;
	int vkeys_x;
	int vkeys_y;
};

struct cyttsp4_platform_data {
	struct cyttsp4_core_platform_data *core_pdata;
	struct cyttsp4_mt_platform_data *mt_pdata;
	struct cyttsp4_loader_platform_data *loader_pdata;
};

extern struct cyttsp4_loader_platform_data _cyttsp4_loader_platform_data;

int cyttsp4_xres(struct cyttsp4_core_platform_data *pdata,
		struct device *dev);
int cyttsp4_init(struct cyttsp4_core_platform_data *pdata,
		int on, struct device *dev);
int cyttsp4_power(struct cyttsp4_core_platform_data *pdata,
		int on, struct device *dev, atomic_t *ignore_irq);
int cyttsp4_irq_stat(struct cyttsp4_core_platform_data *pdata,
		struct device *dev);

#endif /* _CYTTSP4_H_ */
