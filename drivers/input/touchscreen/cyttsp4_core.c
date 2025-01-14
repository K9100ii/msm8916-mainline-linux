// SPDX-License-Identifier: GPL-2.0-only
/*
 * cyttsp4_core.c
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
 * Modified by: Cypress Semiconductor to add device functions
 *
 * Contact Cypress Semiconductor at www.cypress.com <ttdrivers@cypress.com>
 */

#include "cyttsp4_regs.h"

#define CY_CORE_STARTUP_RETRY_COUNT		3

static const char * const cyttsp4_tch_abs_string[] = {
	[CY_TCH_X]	= "X",
	[CY_TCH_Y]	= "Y",
	[CY_TCH_P]	= "P",
	[CY_TCH_T]	= "T",
	[CY_TCH_E]	= "E",
	[CY_TCH_O]	= "O",
	[CY_TCH_W]	= "W",
	[CY_TCH_MAJ]	= "MAJ",
	[CY_TCH_MIN]	= "MIN",
	[CY_TCH_OR]	= "OR",
	[CY_TCH_NUM_ABS] = "INVALID"
};

static const u8 security_key[] = {
	0xA5, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0x5A
};

static const u8 ldr_exit[] = {
	0xFF, 0x01, 0x3B, 0x00, 0x00, 0x4F, 0x6D, 0x17
};

static const u8 ldr_fast_exit[] = {
	0xFF, 0x01, 0x3C, 0x00, 0x00, 0xC3, 0x68, 0x17
};

static const u8 ldr_err_app[] = {
	0x01, 0x02, 0x00, 0x00, 0x55, 0xDD, 0x17
};

MODULE_FIRMWARE(CY_FW_FILE_NAME);

struct atten_node {
	struct list_head node;
	char id;
	int (*func)(struct device *);
	struct device *dev;
	int mode;
};

static int _cyttsp4_put_device_into_deep_sleep(struct cyttsp4_core_data *cd,
		u8 hst_mode_reg);

static inline size_t merge_bytes(u8 high, u8 low)
{
	return (high << 8) + low;
}

#ifdef DEBUG
static const char* cy_cat_cmd_str_[] = {
	"CAT_NULL",
	"CAT_RESERVED_1",
	"CAT_GET_CFG_ROW_SZ",
	"CAT_READ_CFG_BLK",
	"CAT_WRITE_CFG_BLK",
	"CAT_RESERVED_2",
	"CAT_LOAD_SELF_TEST_DATA",
	"CAT_RUN_SELF_TEST",
	"CAT_GET_SELF_TEST_RESULT",
	"CAT_CALIBRATE_IDACS",
	"CAT_INIT_BASELINES",
	"CAT_EXEC_PANEL_SCAN",
	"CAT_RETRIEVE_PANEL_SCAN",
	"CAT_START_SENSOR_DATA_MODE",
	"CAT_STOP_SENSOR_DATA_MODE",
	"CAT_INT_PIN_MODE",
	"CAT_RETRIEVE_DATA_STRUCTURE",
	"CAT_VERIFY_CFG_BLK_CRC",
	"CAT_RESERVED_N",
};
static const char* cy_op_cmd_str_[] = {
	"OP_NULL",
	"OP_RESERVED_1",
	"OP_GET_PARAM",
	"OP_SET_PARAM",
	"OP_RESERVED_2",
	"OP_GET_CRC",
	"OP_WAIT_FOR_EVENT",
	""
};

static inline const char* cy_cmd_str(u8 mode, u8 cmd)
{
	switch (mode) {
	case CY_MODE_CAT:
		if (cmd > CY_CMD_CAT_RESERVED_N)
			return cy_op_cmd_str_[7];
		return cy_cat_cmd_str_[cmd];
	case CY_MODE_OPERATIONAL:
		if (cmd > CY_CMD_OP_WAIT_FOR_EVENT)
			return cy_op_cmd_str_[7];
		return cy_op_cmd_str_[cmd];
	default:
		return cy_op_cmd_str_[7];
	}
}
#endif /* DEBUG */

#ifdef VERBOSE_DEBUG
void cyttsp4_pr_buf(struct device *dev, u8 *pr_buf, u8 *dptr, int size,
		const char *data_name)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int i, k;
	const char fmt[] = "%02X ";
	int max;

	if (!size)
		return;

	if (!pr_buf) {
		if (cd->pr_buf)
			pr_buf = cd->pr_buf;
		else
			return;
	}

	max = (CY_MAX_PRBUF_SIZE - 1) - sizeof(CY_PR_TRUNCATED);

	pr_buf[0] = 0;
	for (i = k = 0; i < size && k < max; i++, k += 3)
		scnprintf(pr_buf + k, CY_MAX_PRBUF_SIZE, fmt, dptr[i]);

	dev_vdbg(dev, "%s:  %s[0..%d]=%s%s\n", __func__, data_name, size - 1,
			pr_buf, size <= max ? "" : CY_PR_TRUNCATED);
}
EXPORT_SYMBOL_GPL(cyttsp4_pr_buf);
#endif /* VERBOSE_DEBUG */

static inline int cyttsp4_adap_read(struct cyttsp4_core_data *cd, u16 addr,
		void *buf, int size)
{
	return cd->bus_ops->read(cd->dev, addr, buf, size, cd->max_xfer);
}

static inline int cyttsp4_adap_write(struct cyttsp4_core_data *cd, u16 addr,
		const void *buf, int size)
{
	return cd->bus_ops->write(cd->dev, addr, cd->wr_buf, buf, size,
			cd->max_xfer);
}

/* cyttsp4_platform_detect_read()
 *
 * This function is passed to platform detect
 * function to perform a read operation
 */
static int cyttsp4_platform_detect_read(struct device *dev, u16 addr,
		void *buf, int size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	return cd->bus_ops->read(cd->dev, addr, buf, size, cd->max_xfer);
}

static u16 cyttsp4_calc_partial_app_crc(const u8 *data, int size, u16 crc)
{
	int i, j;

	for (i = 0; i < size; i++) {
		crc ^= ((u16)data[i] << 8);
		for (j = 8; j > 0; j--)
			if (crc & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
	}

	return crc;
}

static inline u16 cyttsp4_calc_app_crc(const u8 *data, int size)
{
	return cyttsp4_calc_partial_app_crc(data, size, 0xFFFF);
}

static const u8 *cyttsp4_get_security_key_(struct device *dev,
		int *size)
{
	if (size)
		*size = sizeof(security_key);

	return security_key;
}

static inline void cyttsp4_get_touch_axis(struct cyttsp4_core_data *cd,
		int *axis, int size, int max, u8 *xy_data, int bofs)
{
	int nbyte;
	int next;

	for (nbyte = 0, *axis = 0, next = 0; nbyte < size; nbyte++) {
		dev_vdbg(cd->dev,
			"%s: *axis=%02X(%d) size=%d max=%08X xy_data=%p xy_data[%d]=%02X(%d) bofs=%d\n",
			__func__, *axis, *axis, size, max, xy_data, next,
			xy_data[next], xy_data[next], bofs);
		*axis = (*axis * 256) + (xy_data[next] >> bofs);
		next++;
	}

	*axis &= max - 1;

	dev_vdbg(cd->dev,
		"%s: *axis=%02X(%d) size=%d max=%08X xy_data=%p xy_data[%d]=%02X(%d)\n",
		__func__, *axis, *axis, size, max, xy_data, next,
		xy_data[next], xy_data[next]);
}

/*
 * cyttsp4_get_touch_record_()
 *
 * Fills touch info for a touch record specified by rec_no
 * Should only be called in Operational mode IRQ attention and
 * rec_no should be less than the number of current touch records
 */
void cyttsp4_get_touch_record_(struct device *dev, int rec_no, int *rec_abs)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	u8 *xy_data = si->xy_data + (rec_no * si->si_ofs.tch_rec_size);
	enum cyttsp4_tch_abs abs;

	memset(rec_abs, 0, CY_TCH_NUM_ABS * sizeof(int));
	for (abs = CY_TCH_X; abs < CY_TCH_NUM_ABS; abs++) {
		cyttsp4_get_touch_axis(cd, &rec_abs[abs],
			si->si_ofs.tch_abs[abs].size,
			si->si_ofs.tch_abs[abs].max,
			xy_data + si->si_ofs.tch_abs[abs].ofs,
			si->si_ofs.tch_abs[abs].bofs);
		dev_vdbg(dev, "%s: get %s=%04X(%d)\n", __func__,
			cyttsp4_tch_abs_string[abs],
			rec_abs[abs], rec_abs[abs]);
	}
}

static int cyttsp4_load_status_and_touch_regs(struct cyttsp4_core_data *cd,
		bool optimize)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	struct device *dev = cd->dev;
	int first_read_len;
	int second_read_off;
	int num_read_rec;
	u8 num_cur_rec;
	u8 hst_mode;
	u8 rep_len;
	u8 rep_stat;
	u8 tt_stat;
	int rc;

	if (!si->xy_mode) {
		dev_err(cd->dev, "%s: NULL xy_mode pointer\n",
			__func__);
		return -EINVAL;
	}

	first_read_len = si->si_ofs.rep_hdr_size;
	/* Read one touch record additionally */
	if (optimize)
		first_read_len += si->si_ofs.tch_rec_size;

	rc = cyttsp4_adap_read(cd, si->si_ofs.rep_ofs,
			&si->xy_mode[si->si_ofs.rep_ofs], first_read_len);
	if (rc < 0) {
		dev_err(dev, "%s: fail read mode regs r=%d\n",
			__func__, rc);
		return rc;
	}

	/* print xy data */
	cyttsp4_pr_buf(dev, cd->pr_buf, si->xy_mode,
		si->si_ofs.mode_size, "xy_mode");

	hst_mode = si->xy_mode[CY_REG_BASE];
	rep_len = si->xy_mode[si->si_ofs.rep_ofs];
	rep_stat = si->xy_mode[si->si_ofs.rep_ofs + 1];
	tt_stat = si->xy_mode[si->si_ofs.tt_stat_ofs];
	dev_vdbg(dev, "%s: %s%02X %s%d %s%02X %s%02X\n", __func__,
		"hst_mode=", hst_mode, "rep_len=", rep_len,
		"rep_stat=", rep_stat, "tt_stat=", tt_stat);

	num_cur_rec = GET_NUM_TOUCH_RECORDS(tt_stat);
	dev_vdbg(dev, "%s: num_cur_rec=%d\n", __func__, num_cur_rec);

	if (rep_len == 0 && num_cur_rec > 0) {
		dev_err(dev, "%s: report length error rep_len=%d num_rec=%d\n",
			__func__, rep_len, num_cur_rec);
		return -EIO;
	}

	if (num_cur_rec > si->si_ofs.max_tchs) {
		dev_err(dev, "%s: %s (n=%d c=%zd)\n", __func__,
			"too many tch; set to max tch",
			num_cur_rec, si->si_ofs.max_tchs);
		num_cur_rec = si->si_ofs.max_tchs;
	}

	num_read_rec = num_cur_rec;
	second_read_off = si->si_ofs.tt_stat_ofs + 1;
	if (optimize) {
		num_read_rec--;
		second_read_off += si->si_ofs.tch_rec_size;
	}

	if (num_read_rec <= 0)
		goto exit_print;

	rc = cyttsp4_adap_read(cd, second_read_off,
			&si->xy_mode[second_read_off],
			num_read_rec * si->si_ofs.tch_rec_size);
	if (rc < 0) {
		dev_err(dev, "%s: read fail on touch regs r=%d\n",
			__func__, rc);
		return rc;
	}

exit_print:
	/* print xy data */
	cyttsp4_pr_buf(dev, cd->pr_buf, si->xy_data,
		num_cur_rec * si->si_ofs.tch_rec_size, "xy_data");

	return 0;
}

static int cyttsp4_handshake(struct cyttsp4_core_data *cd, u8 mode)
{
	u8 cmd = mode ^ CY_HST_TOGGLE;
	int rc;

	if (mode & CY_HST_MODE_CHANGE) {
		dev_err(cd->dev, "%s: Host mode change bit set, NO handshake\n",
				__func__);
		return 0;
	}

	rc = cyttsp4_adap_write(cd, CY_REG_BASE, &cmd, sizeof(cmd));
	if (rc < 0)
		dev_err(cd->dev, "%s: bus write fail on handshake (ret=%d)\n",
				__func__, rc);

	return rc;
}

static int cyttsp4_toggle_low_power_(struct cyttsp4_core_data *cd, u8 mode)
{
	u8 cmd = mode ^ CY_HST_LOWPOW;

	int rc = cyttsp4_adap_write(cd, CY_REG_BASE, &cmd, sizeof(cmd));
	if (rc < 0)
		dev_err(cd->dev,
			"%s: bus write fail on toggle low power (ret=%d)\n",
			__func__, rc);
	return rc;
}

static int cyttsp4_toggle_low_power(struct cyttsp4_core_data *cd, u8 mode)
{
	int rc;

	mutex_lock(&cd->system_lock);
	rc = cyttsp4_toggle_low_power_(cd, mode);
	mutex_unlock(&cd->system_lock);

	return rc;
}

static int cyttsp4_hw_soft_reset_(struct cyttsp4_core_data *cd)
{
	u8 cmd = CY_HST_RESET;

	int rc = cyttsp4_adap_write(cd, CY_REG_BASE, &cmd, sizeof(cmd));
	if (rc < 0) {
		dev_err(cd->dev, "%s: FAILED to execute SOFT reset\n",
				__func__);
		return rc;
	}
	dev_dbg(cd->dev, "%s: execute SOFT reset\n", __func__);
	return 0;
}

static int cyttsp4_hw_soft_reset(struct cyttsp4_core_data *cd)
{
	int rc;

	mutex_lock(&cd->system_lock);
	rc = cyttsp4_hw_soft_reset_(cd);
	mutex_unlock(&cd->system_lock);

	return rc;
}

static int cyttsp4_hw_hard_reset_(struct cyttsp4_core_data *cd)
{
	if (cd->cpdata->xres) {
		cd->cpdata->xres(cd->cpdata, cd->dev);
		dev_dbg(cd->dev, "%s: execute HARD reset\n", __func__);
		return 0;
	}
	dev_err(cd->dev, "%s: FAILED to execute HARD reset\n", __func__);
	return -ENOSYS;
}

static int cyttsp4_hw_hard_reset(struct cyttsp4_core_data *cd)
{
	int rc;

	mutex_lock(&cd->system_lock);
	rc = cyttsp4_hw_hard_reset_(cd);
	mutex_unlock(&cd->system_lock);

	return rc;
}

static int cyttsp4_hw_reset_(struct cyttsp4_core_data *cd)
{
	int rc = cyttsp4_hw_hard_reset_(cd);
	if (rc == -ENOSYS)
		rc = cyttsp4_hw_soft_reset_(cd);
	return rc;
}

static inline int cyttsp4_bits_2_bytes(unsigned int nbits, size_t *max)
{
	*max = 1UL << nbits;
	return (nbits + 7) / 8;
}

static int cyttsp4_si_data_offsets(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	int rc = cyttsp4_adap_read(cd, CY_REG_BASE, &si->si_data,
				   sizeof(si->si_data));
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail read sysinfo data offsets r=%d\n",
			__func__, rc);
		return rc;
	}

	/* Print sysinfo data offsets */
	cyttsp4_pr_buf(cd->dev, cd->pr_buf, (u8 *)&si->si_data,
		       sizeof(si->si_data), "sysinfo_data_offsets");

	/* convert sysinfo data offset bytes into integers */

	si->si_ofs.map_sz = merge_bytes(si->si_data.map_szh,
			si->si_data.map_szl);
	si->si_ofs.map_sz = merge_bytes(si->si_data.map_szh,
			si->si_data.map_szl);
	si->si_ofs.cydata_ofs = merge_bytes(si->si_data.cydata_ofsh,
			si->si_data.cydata_ofsl);
	si->si_ofs.test_ofs = merge_bytes(si->si_data.test_ofsh,
			si->si_data.test_ofsl);
	si->si_ofs.pcfg_ofs = merge_bytes(si->si_data.pcfg_ofsh,
			si->si_data.pcfg_ofsl);
	si->si_ofs.opcfg_ofs = merge_bytes(si->si_data.opcfg_ofsh,
			si->si_data.opcfg_ofsl);
	si->si_ofs.ddata_ofs = merge_bytes(si->si_data.ddata_ofsh,
			si->si_data.ddata_ofsl);
	si->si_ofs.mdata_ofs = merge_bytes(si->si_data.mdata_ofsh,
			si->si_data.mdata_ofsl);
	return rc;
}

static int cyttsp4_si_get_cydata(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	int read_offset;
	int mfgid_sz, calc_mfgid_sz;
	void *p;
	int rc;

	if (si->si_ofs.test_ofs <= si->si_ofs.cydata_ofs) {
		dev_err(cd->dev,
			"%s: invalid offset test_ofs: %zu, cydata_ofs: %zu\n",
			__func__, si->si_ofs.test_ofs, si->si_ofs.cydata_ofs);
		return -EINVAL;
	}

	si->si_ofs.cydata_size = si->si_ofs.test_ofs - si->si_ofs.cydata_ofs;
	dev_dbg(cd->dev, "%s: cydata size: %zd\n", __func__,
			si->si_ofs.cydata_size);

	if (si->si_ofs.cydata_size <= 0)
		return -EINVAL;

	p = krealloc(si->si_ptrs.cydata, si->si_ofs.cydata_size, GFP_KERNEL);
	if (p == NULL) {
		dev_err(cd->dev, "%s: failed to allocate cydata memory\n",
			__func__);
		return -ENOMEM;
	}
	si->si_ptrs.cydata = p;

	read_offset = si->si_ofs.cydata_ofs;

	/* Read the CYDA registers up to MFGID field */
	rc = cyttsp4_adap_read(cd, read_offset, si->si_ptrs.cydata,
			offsetof(struct cyttsp4_cydata, mfgid_sz)
			+ sizeof(si->si_ptrs.cydata->mfgid_sz));
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail read cydata r=%d\n",
			__func__, rc);
		return rc;
	}

	/* Check MFGID size */
	mfgid_sz = si->si_ptrs.cydata->mfgid_sz;
	calc_mfgid_sz = si->si_ofs.cydata_size - sizeof(struct cyttsp4_cydata);
	if (mfgid_sz != calc_mfgid_sz) {
		dev_err(cd->dev, "%s: mismatch in MFGID size, reported:%d calculated:%d\n",
			__func__, mfgid_sz, calc_mfgid_sz);
		return -EINVAL;
	}

	read_offset += offsetof(struct cyttsp4_cydata, mfgid_sz)
			+ sizeof(si->si_ptrs.cydata->mfgid_sz);

	/* Read the CYDA registers for MFGID field */
	rc = cyttsp4_adap_read(cd, read_offset, si->si_ptrs.cydata->mfg_id,
			si->si_ptrs.cydata->mfgid_sz);
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail read cydata r=%d\n",
			__func__, rc);
		return rc;
	}

	read_offset += si->si_ptrs.cydata->mfgid_sz;

	/* Read the rest of the CYDA registers */
	rc = cyttsp4_adap_read(cd, read_offset, &si->si_ptrs.cydata->cyito_idh,
			sizeof(struct cyttsp4_cydata)
			- offsetof(struct cyttsp4_cydata, cyito_idh));
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail read cydata r=%d\n",
			__func__, rc);
		return rc;
	}

	cyttsp4_pr_buf(cd->dev, cd->pr_buf, (u8 *)si->si_ptrs.cydata,
		si->si_ofs.cydata_size - mfgid_sz, "sysinfo_cydata");
	cyttsp4_pr_buf(cd->dev, cd->pr_buf, si->si_ptrs.cydata->mfg_id,
		mfgid_sz, "sysinfo_cydata_mfgid");
	return rc;
}

static int cyttsp4_si_get_test_data(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	void *p;
	int rc;

	if (si->si_ofs.pcfg_ofs <= si->si_ofs.test_ofs) {
		dev_err(cd->dev,
			"%s: invalid offset pcfg_ofs: %zu, test_ofs: %zu\n",
			__func__, si->si_ofs.pcfg_ofs, si->si_ofs.test_ofs);
		return -EINVAL;
	}

	si->si_ofs.test_size = si->si_ofs.pcfg_ofs - si->si_ofs.test_ofs;

	if (si->si_ofs.test_size <= 0)
		return -EINVAL;

	p = krealloc(si->si_ptrs.test, si->si_ofs.test_size, GFP_KERNEL);
	if (p == NULL) {
		dev_err(cd->dev, "%s: failed to allocate test memory\n",
			__func__);
		return -ENOMEM;
	}
	si->si_ptrs.test = p;

	rc = cyttsp4_adap_read(cd, si->si_ofs.test_ofs, si->si_ptrs.test,
			si->si_ofs.test_size);
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail read test data r=%d\n",
			__func__, rc);
		return rc;
	}

	cyttsp4_pr_buf(cd->dev, cd->pr_buf,
		       (u8 *)si->si_ptrs.test, si->si_ofs.test_size,
		       "sysinfo_test_data");
	if (si->si_ptrs.test->post_codel &
	    CY_POST_CODEL_WDG_RST)
		dev_info(cd->dev, "%s: %s codel=%02X\n",
			 __func__, "Reset was a WATCHDOG RESET",
			 si->si_ptrs.test->post_codel);

	if (!(si->si_ptrs.test->post_codel &
	      CY_POST_CODEL_CFG_DATA_CRC_FAIL))
		dev_info(cd->dev, "%s: %s codel=%02X\n", __func__,
			 "Config Data CRC FAIL",
			 si->si_ptrs.test->post_codel);

	if (!(si->si_ptrs.test->post_codel &
	      CY_POST_CODEL_PANEL_TEST_FAIL))
		dev_info(cd->dev, "%s: %s codel=%02X\n",
			 __func__, "PANEL TEST FAIL",
			 si->si_ptrs.test->post_codel);

	dev_info(cd->dev, "%s: SCANNING is %s codel=%02X\n",
		 __func__, si->si_ptrs.test->post_codel & 0x08 ?
		 "ENABLED" : "DISABLED",
		 si->si_ptrs.test->post_codel);
	return rc;
}

static int cyttsp4_si_get_pcfg_data(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	void *p;
	int rc;

	if (si->si_ofs.opcfg_ofs <= si->si_ofs.pcfg_ofs) {
		dev_err(cd->dev,
			"%s: invalid offset opcfg_ofs: %zu, pcfg_ofs: %zu\n",
			__func__, si->si_ofs.opcfg_ofs, si->si_ofs.pcfg_ofs);
		return -EINVAL;
	}

	si->si_ofs.pcfg_size = si->si_ofs.opcfg_ofs - si->si_ofs.pcfg_ofs;

	if (si->si_ofs.pcfg_size <= 0)
		return -EINVAL;

	p = krealloc(si->si_ptrs.pcfg, si->si_ofs.pcfg_size, GFP_KERNEL);
	if (p == NULL) {
		dev_err(cd->dev, "%s: failed to allocate pcfg memory\n",
			__func__);
		return -ENOMEM;

	}
	si->si_ptrs.pcfg = p;

	rc = cyttsp4_adap_read(cd, si->si_ofs.pcfg_ofs, si->si_ptrs.pcfg,
			si->si_ofs.pcfg_size);
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail read pcfg data r=%d\n",
			__func__, rc);
		return rc;
	}

	si->si_ofs.max_x = merge_bytes((si->si_ptrs.pcfg->res_xh
			& CY_PCFG_RESOLUTION_X_MASK), si->si_ptrs.pcfg->res_xl);
	si->si_ofs.x_origin = !!(si->si_ptrs.pcfg->res_xh
			& CY_PCFG_ORIGIN_X_MASK);
	si->si_ofs.max_y = merge_bytes((si->si_ptrs.pcfg->res_yh
			& CY_PCFG_RESOLUTION_Y_MASK), si->si_ptrs.pcfg->res_yl);
	si->si_ofs.y_origin = !!(si->si_ptrs.pcfg->res_yh
			& CY_PCFG_ORIGIN_Y_MASK);
	si->si_ofs.max_p = merge_bytes(si->si_ptrs.pcfg->max_zh,
			si->si_ptrs.pcfg->max_zl);

	cyttsp4_pr_buf(cd->dev, cd->pr_buf,
		       (u8 *)si->si_ptrs.pcfg,
		       si->si_ofs.pcfg_size, "sysinfo_pcfg_data");
	return rc;
}

static int cyttsp4_si_get_opcfg_data(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	int i;
	enum cyttsp4_tch_abs abs;
	void *p;
	int rc;

	if (si->si_ofs.ddata_ofs <= si->si_ofs.opcfg_ofs) {
		dev_err(cd->dev,
			"%s: invalid offset ddata_ofs: %zu, opcfg_ofs: %zu\n",
			__func__, si->si_ofs.ddata_ofs, si->si_ofs.opcfg_ofs);
		return -EINVAL;
	}

	si->si_ofs.opcfg_size = si->si_ofs.ddata_ofs - si->si_ofs.opcfg_ofs;

	if (si->si_ofs.opcfg_size <= 0)
		return -EINVAL;

	p = krealloc(si->si_ptrs.opcfg, si->si_ofs.opcfg_size, GFP_KERNEL);
	if (p == NULL) {
		dev_err(cd->dev, "%s: failed to allocate opcfg memory\n",
			__func__);
		return -ENOMEM;
	}
	si->si_ptrs.opcfg = p;

	rc = cyttsp4_adap_read(cd, si->si_ofs.opcfg_ofs, si->si_ptrs.opcfg,
			si->si_ofs.opcfg_size);
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail read opcfg data r=%d\n",
			__func__, rc);
		return rc;
	}
	si->si_ofs.cmd_ofs = si->si_ptrs.opcfg->cmd_ofs;
	si->si_ofs.rep_ofs = si->si_ptrs.opcfg->rep_ofs;
	si->si_ofs.rep_sz = (si->si_ptrs.opcfg->rep_szh * 256) +
		si->si_ptrs.opcfg->rep_szl;
	si->si_ofs.num_btns = si->si_ptrs.opcfg->num_btns;
	si->si_ofs.num_btn_regs = (si->si_ofs.num_btns +
		CY_NUM_BTN_PER_REG - 1) / CY_NUM_BTN_PER_REG;
	si->si_ofs.tt_stat_ofs = si->si_ptrs.opcfg->tt_stat_ofs;
	si->si_ofs.obj_cfg0 = si->si_ptrs.opcfg->obj_cfg0;
	si->si_ofs.max_tchs = si->si_ptrs.opcfg->max_tchs &
		CY_BYTE_OFS_MASK;
	si->si_ofs.tch_rec_size = si->si_ptrs.opcfg->tch_rec_size &
		CY_BYTE_OFS_MASK;

	/* Get the old touch fields */
	for (abs = CY_TCH_X; abs < CY_NUM_TCH_FIELDS; abs++) {
		si->si_ofs.tch_abs[abs].ofs =
			si->si_ptrs.opcfg->tch_rec_old[abs].loc &
			CY_BYTE_OFS_MASK;
		si->si_ofs.tch_abs[abs].size =
			cyttsp4_bits_2_bytes
			(si->si_ptrs.opcfg->tch_rec_old[abs].size,
			&si->si_ofs.tch_abs[abs].max);
		si->si_ofs.tch_abs[abs].bofs =
			(si->si_ptrs.opcfg->tch_rec_old[abs].loc &
			CY_BOFS_MASK) >> CY_BOFS_SHIFT;
	}

	/* button fields */
	si->si_ofs.btn_rec_size = si->si_ptrs.opcfg->btn_rec_size;
	si->si_ofs.btn_diff_ofs = si->si_ptrs.opcfg->btn_diff_ofs;
	si->si_ofs.btn_diff_size = si->si_ptrs.opcfg->btn_diff_size;

	if (IS_TTSP_VER_GE(si, 2, 3)) {
		/* Get the extended touch fields */
		for (i = 0; i < CY_NUM_EXT_TCH_FIELDS; abs++, i++) {
			si->si_ofs.tch_abs[abs].ofs =
				si->si_ptrs.opcfg->tch_rec_new[i].loc &
				CY_BYTE_OFS_MASK;
			si->si_ofs.tch_abs[abs].size =
				cyttsp4_bits_2_bytes
				(si->si_ptrs.opcfg->tch_rec_new[i].size,
				&si->si_ofs.tch_abs[abs].max);
			si->si_ofs.tch_abs[abs].bofs =
				(si->si_ptrs.opcfg->tch_rec_new[i].loc
				& CY_BOFS_MASK) >> CY_BOFS_SHIFT;
		}
	}

	if (IS_TTSP_VER_GE(si, 2, 4)) {
		si->si_ofs.noise_data_ofs = si->si_ptrs.opcfg->noise_data_ofs;
		si->si_ofs.noise_data_sz = si->si_ptrs.opcfg->noise_data_sz;
	}

	si->si_ofs.mode_size = si->si_ofs.tt_stat_ofs + 1;
	si->si_ofs.data_size = si->si_ofs.max_tchs *
		si->si_ptrs.opcfg->tch_rec_size;
	si->si_ofs.rep_hdr_size = si->si_ofs.mode_size - si->si_ofs.rep_ofs;

	cyttsp4_pr_buf(cd->dev, cd->pr_buf, (u8 *)si->si_ptrs.opcfg,
		si->si_ofs.opcfg_size, "sysinfo_opcfg_data");

	return 0;
}

static int cyttsp4_si_get_ddata(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	void *p;
	int rc;

	dev_vdbg(cd->dev, "%s: get ddata data\n", __func__);
	si->si_ofs.ddata_size = si->si_ofs.mdata_ofs - si->si_ofs.ddata_ofs;

	if (si->si_ofs.ddata_size <= 0)
		return -EINVAL;

	p = krealloc(si->si_ptrs.ddata, si->si_ofs.ddata_size, GFP_KERNEL);
	if (p == NULL) {
		dev_err(cd->dev, "%s: fail alloc ddata memory\n", __func__);
		return -ENOMEM;
	}
	si->si_ptrs.ddata = p;

	rc = cyttsp4_adap_read(cd, si->si_ofs.ddata_ofs, si->si_ptrs.ddata,
			si->si_ofs.ddata_size);
	if (rc < 0)
		dev_err(cd->dev, "%s: fail read ddata data r=%d\n",
			__func__, rc);
	else {
		cyttsp4_pr_buf(cd->dev, cd->pr_buf,
			       (u8 *)si->si_ptrs.ddata,
			       si->si_ofs.ddata_size, "sysinfo_ddata");
	}
	return rc;
}

static int cyttsp4_si_get_mdata(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	void *p;
	int rc;

	dev_vdbg(cd->dev, "%s: get mdata data\n", __func__);
	si->si_ofs.mdata_size = si->si_ofs.map_sz - si->si_ofs.mdata_ofs;

	if (si->si_ofs.mdata_size <= 0)
		return -EINVAL;

	p = krealloc(si->si_ptrs.mdata, si->si_ofs.mdata_size, GFP_KERNEL);
	if (p == NULL) {
		dev_err(cd->dev, "%s: fail alloc mdata memory\n", __func__);
		return -ENOMEM;
	}
	si->si_ptrs.mdata = p;

	rc = cyttsp4_adap_read(cd, si->si_ofs.mdata_ofs, si->si_ptrs.mdata,
			si->si_ofs.mdata_size);
	if (rc < 0)
		dev_err(cd->dev, "%s: fail read mdata data r=%d\n",
			__func__, rc);
	else
		cyttsp4_pr_buf(cd->dev, cd->pr_buf,
			       (u8 *)si->si_ptrs.mdata,
			       si->si_ofs.mdata_size, "sysinfo_mdata");
	return rc;
}

static int cyttsp4_si_get_btn_data(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	int btn;
	int num_defined_keys;
	u16 *key_table;
	void *p;
	int rc = 0;

	dev_vdbg(cd->dev, "%s: get btn data\n", __func__);

	if (!si->si_ofs.num_btns) {
		si->si_ofs.btn_keys_size = 0;
		kfree(si->btn);
		si->btn = NULL;
		return rc;
	}

	si->si_ofs.btn_keys_size = si->si_ofs.num_btns *
		sizeof(struct cyttsp4_btn);

	if (si->si_ofs.btn_keys_size <= 0)
		return -EINVAL;

	p = krealloc(si->btn, si->si_ofs.btn_keys_size, GFP_KERNEL|__GFP_ZERO);
	if (p == NULL) {
		dev_err(cd->dev, "%s: %s\n", __func__,
			"fail alloc btn_keys memory");
		return -ENOMEM;
	}
	si->btn = p;

	if (cd->cpdata->sett[CY_IC_GRPNUM_BTN_KEYS] == NULL)
		num_defined_keys = 0;
	else if (cd->cpdata->sett[CY_IC_GRPNUM_BTN_KEYS]->data == NULL)
		num_defined_keys = 0;
	else
		num_defined_keys =
			cd->cpdata->sett[CY_IC_GRPNUM_BTN_KEYS]->size;

	for (btn = 0; btn < si->si_ofs.num_btns
			&& btn < num_defined_keys; btn++) {
		key_table =
			(u16 *)cd->cpdata->sett[CY_IC_GRPNUM_BTN_KEYS]->data;
		si->btn[btn].key_code = key_table[btn];
		si->btn[btn].state = CY_BTN_RELEASED;
		si->btn[btn].enabled = true;
	}
	for (; btn < si->si_ofs.num_btns; btn++) {
		si->btn[btn].key_code = KEY_RESERVED;
		si->btn[btn].state = CY_BTN_RELEASED;
		si->btn[btn].enabled = true;
	}

	return rc;
}

static int cyttsp4_si_get_op_data_ptrs(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	void *p;
	int size;

	p = krealloc(si->xy_mode, si->si_ofs.mode_size +
			si->si_ofs.data_size, GFP_KERNEL|__GFP_ZERO);
	if (p == NULL)
		return -ENOMEM;
	si->xy_mode = p;
	si->xy_data = &si->xy_mode[si->si_ofs.tt_stat_ofs + 1];

	size = si->si_ofs.btn_rec_size * si->si_ofs.num_btns;
	if (!size)
		return 0;

	p = krealloc(si->btn_rec_data, size, GFP_KERNEL|__GFP_ZERO);
	if (p == NULL)
		return -ENOMEM;
	si->btn_rec_data = p;

	return 0;
}

static void cyttsp4_si_put_log_data(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	dev_dbg(cd->dev, "%s: cydata_ofs =%4zd siz=%4zd\n", __func__,
		si->si_ofs.cydata_ofs, si->si_ofs.cydata_size);
	dev_dbg(cd->dev, "%s: test_ofs   =%4zd siz=%4zd\n", __func__,
		si->si_ofs.test_ofs, si->si_ofs.test_size);
	dev_dbg(cd->dev, "%s: pcfg_ofs   =%4zd siz=%4zd\n", __func__,
		si->si_ofs.pcfg_ofs, si->si_ofs.pcfg_size);
	dev_dbg(cd->dev, "%s: opcfg_ofs  =%4zd siz=%4zd\n", __func__,
		si->si_ofs.opcfg_ofs, si->si_ofs.opcfg_size);
	dev_dbg(cd->dev, "%s: ddata_ofs  =%4zd siz=%4zd\n", __func__,
		si->si_ofs.ddata_ofs, si->si_ofs.ddata_size);
	dev_dbg(cd->dev, "%s: mdata_ofs  =%4zd siz=%4zd\n", __func__,
		si->si_ofs.mdata_ofs, si->si_ofs.mdata_size);

	dev_dbg(cd->dev, "%s: cmd_ofs       =%4zd\n", __func__,
		si->si_ofs.cmd_ofs);
	dev_dbg(cd->dev, "%s: rep_ofs       =%4zd\n", __func__,
		si->si_ofs.rep_ofs);
	dev_dbg(cd->dev, "%s: rep_sz        =%4zd\n", __func__,
		si->si_ofs.rep_sz);
	dev_dbg(cd->dev, "%s: num_btns      =%4zd\n", __func__,
		si->si_ofs.num_btns);
	dev_dbg(cd->dev, "%s: num_btn_regs  =%4zd\n", __func__,
		si->si_ofs.num_btn_regs);
	dev_dbg(cd->dev, "%s: tt_stat_ofs   =%4zd\n", __func__,
		si->si_ofs.tt_stat_ofs);
	dev_dbg(cd->dev, "%s: tch_rec_size  =%4zd\n", __func__,
		si->si_ofs.tch_rec_size);
	dev_dbg(cd->dev, "%s: max_tchs      =%4zd\n", __func__,
		si->si_ofs.max_tchs);
	dev_dbg(cd->dev, "%s: mode_size     =%4zd\n", __func__,
		si->si_ofs.mode_size);
	dev_dbg(cd->dev, "%s: data_size     =%4zd\n", __func__,
		si->si_ofs.data_size);
	dev_dbg(cd->dev, "%s: map_sz        =%4zd\n", __func__,
		si->si_ofs.map_sz);

	dev_dbg(cd->dev, "%s: btn_rec_size   =%2zd\n", __func__,
		si->si_ofs.btn_rec_size);
	dev_dbg(cd->dev, "%s: btn_diff_ofs   =%2zd\n", __func__,
		si->si_ofs.btn_diff_ofs);
	dev_dbg(cd->dev, "%s: btn_diff_size  =%2zd\n", __func__,
		si->si_ofs.btn_diff_size);

	dev_dbg(cd->dev, "%s: max_x    = 0x%04zX (%zd)\n", __func__,
		si->si_ofs.max_x, si->si_ofs.max_x);
	dev_dbg(cd->dev, "%s: x_origin = %zd (%s)\n", __func__,
		si->si_ofs.x_origin,
		si->si_ofs.x_origin == CY_NORMAL_ORIGIN ?
		"left corner" : "right corner");
	dev_dbg(cd->dev, "%s: max_y    = 0x%04zX (%zd)\n", __func__,
		si->si_ofs.max_y, si->si_ofs.max_y);
	dev_dbg(cd->dev, "%s: y_origin = %zd (%s)\n", __func__,
		si->si_ofs.y_origin,
		si->si_ofs.y_origin == CY_NORMAL_ORIGIN ?
		"upper corner" : "lower corner");
	dev_dbg(cd->dev, "%s: max_p    = 0x%04zX (%zd)\n", __func__,
		si->si_ofs.max_p, si->si_ofs.max_p);

	dev_dbg(cd->dev, "%s: xy_mode=%p xy_data=%p\n", __func__,
		si->xy_mode, si->xy_data);
}

static int cyttsp4_get_sysinfo_regs(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	int rc;

	rc = cyttsp4_si_data_offsets(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_cydata(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_test_data(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_pcfg_data(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_opcfg_data(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_ddata(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_mdata(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_btn_data(cd);
	if (rc < 0)
		return rc;

	rc = cyttsp4_si_get_op_data_ptrs(cd);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to get_op_data\n",
			__func__);
		return rc;
	}

	cyttsp4_si_put_log_data(cd);

	/* provide flow control handshake */
	rc = cyttsp4_handshake(cd, si->si_data.hst_mode);
	if (rc < 0)
		dev_err(cd->dev, "%s: handshake fail on sysinfo reg\n",
			__func__);

	mutex_lock(&cd->system_lock);
	si->ready = true;
	mutex_unlock(&cd->system_lock);
	return rc;
}

#ifdef DEBUG
static char *ss2str(enum cyttsp4_startup_state ss)
{
	switch (ss) {
	case STARTUP_NONE:
		return "none";
		break;
	case STARTUP_QUEUED:
		return "queued";
		break;
	case STARTUP_RUNNING:
		return "running";
		break;
	default:
		return "default";
		break;
	}
}
#endif /* DEBUG */

static void cyttsp4_queue_startup(struct cyttsp4_core_data *cd)
{
	if (cd->startup_state == STARTUP_NONE) {
		cd->startup_state = STARTUP_QUEUED;
		schedule_work(&cd->startup_work);
		dev_dbg(cd->dev, "%s: cyttsp4_startup queued\n", __func__);
	} else {
#ifdef DEBUG
		dev_dbg(cd->dev, "%s: bypassed because startup_state = %s\n", __func__,
			ss2str(cd->startup_state));
#endif
	}
}

static void call_atten_cb(struct cyttsp4_core_data *cd,
		enum cyttsp4_atten_type type, int mode)
{
	struct atten_node *atten, *atten_n;

	dev_vdbg(cd->dev, "%s: check list type=%d mode=%d\n",
		__func__, type, mode);
	spin_lock(&cd->spinlock);
	list_for_each_entry_safe(atten, atten_n,
			&cd->atten_list[type], node) {
		if (!mode || atten->mode & mode) {
			spin_unlock(&cd->spinlock);
			dev_vdbg(cd->dev, "%s: attention for '%s'", __func__,
				dev_name(atten->dev));
			atten->func(atten->dev);
			spin_lock(&cd->spinlock);
		}
	}
	spin_unlock(&cd->spinlock);
}

static char *int_status2str(int int_status)
{
	switch (int_status) {
	case CY_INT_NONE:
		return "regular";
		break;
	case CY_INT_IGNORE:
		return "ignore";
		break;
	case CY_INT_MODE_CHANGE:
		return "mode_change";
		break;
	case CY_INT_EXEC_CMD:
		return "exec_cmd";
		break;
	case CY_INT_AWAKE:
		return "awake";
		break;
	default:
		return "int_failure";
		break;
	}
}

static char *mode2str(int mode)
{
	switch (mode) {
	case CY_MODE_UNKNOWN:
		return "unknown";
		break;
	case CY_MODE_BOOTLOADER:
		return "bootloader";
		break;
	case CY_MODE_OPERATIONAL:
		return "operational";
		break;
	case CY_MODE_SYSINFO:
		return "sysinfo";
		break;
	case CY_MODE_CAT:
		return "cat";
		break;
	case CY_MODE_STARTUP:
		return "startup";
		break;
	case CY_MODE_LOADER:
		return "loader";
		break;
	case CY_MODE_CHANGE_MODE:
		return "hange_mode";
		break;
	case CY_MODE_CHANGED:
		return "changed";
		break;
	case CY_MODE_CMD_COMPLETE:
		return "cmd_complete";
		break;
	default:
		return "mode_failure";
		break;
	}
}

static irqreturn_t cyttsp4_irq(int irq, void *handle)
{
	struct cyttsp4_core_data *cd = handle;
	struct device *dev = cd->dev;
	enum cyttsp4_mode cur_mode;
	u8 cmd_ofs = cd->sysinfo.si_ofs.cmd_ofs;
	bool command_complete = false;
	u8 mode[3];
	int rc;
	u8 cat_masked_cmd;

	dev_vdbg(dev, "%s int:%s\n", __func__, int_status2str(cd->int_status));

	mutex_lock(&cd->system_lock);

	if (cd->sleep_state == SS_SLEEP_ON) {
		mutex_unlock(&cd->system_lock);
		dev_dbg(dev, "%s: irq during sleep on\n", __func__);
		return IRQ_HANDLED;
	}

	rc = cyttsp4_adap_read(cd, CY_REG_BASE, mode, sizeof(mode));
	if (rc) {
		dev_err(cd->dev, "%s: Fail read adapter r=%d\n", __func__, rc);
		goto cyttsp4_irq_exit;
	}
	dev_vdbg(dev, "%s mode[0-2]:0x%X 0x%X 0x%X\n", __func__,
			mode[0], mode[1], mode[2]);

	if (IS_BOOTLOADER(mode[0], mode[1])) {
		cur_mode = CY_MODE_BOOTLOADER;
		dev_vdbg(dev, "%s: bl running\n", __func__);
		call_atten_cb(cd, CY_ATTEN_IRQ, cur_mode);

		/* switch to bootloader */
		if (cd->mode != CY_MODE_BOOTLOADER) {
			dev_dbg(dev, "%s: restart switch to bl m=%s -> m=%s\n",
			__func__, mode2str(cd->mode), mode2str(cur_mode));
			cd->heartbeat_count = 0;
		}

		/* catch operation->bl glitch */
		if (cd->mode != CY_MODE_BOOTLOADER
				&& cd->mode != CY_MODE_UNKNOWN) {
			/* Incase startup_state do not let startup_() */
			cd->mode = CY_MODE_UNKNOWN;
			cyttsp4_queue_startup(cd);
			goto cyttsp4_irq_exit;
		}

		/* Recover if stuck in bootloader idle mode */
		if (cd->mode == CY_MODE_BOOTLOADER) {
			if (IS_BOOTLOADER_IDLE(mode[0], mode[1])) {
				dev_dbg(dev, "%s: heartbeat_count %d\n", __func__,
					cd->heartbeat_count);
				if (cd->heartbeat_count > 3) {
					cd->heartbeat_count = 0;
					dev_dbg(dev, "%s: stuck in bootloader\n", __func__);
					cyttsp4_queue_startup(cd);
					goto cyttsp4_irq_exit;
				}
				cd->heartbeat_count++;
			}
		}

		cd->mode = cur_mode;
		/* Signal bootloader heartbeat heard */
		wake_up(&cd->wait_q);
		goto cyttsp4_irq_exit;
	}

	switch (mode[0] & CY_HST_DEVICE_MODE) {
	case CY_HST_OPERATE:
		cur_mode = CY_MODE_OPERATIONAL;
		dev_vdbg(dev, "%s: operational\n", __func__);
		break;
	case CY_HST_CAT:
		cur_mode = CY_MODE_CAT;
		/* set the start sensor mode state. */
		cat_masked_cmd = mode[2] & CY_CMD_MASK;

		/* Get the Debug info for the interrupt. */
		if (cat_masked_cmd != CY_CMD_CAT_NULL &&
				cat_masked_cmd !=
					CY_CMD_CAT_RETRIEVE_PANEL_SCAN &&
				cat_masked_cmd != CY_CMD_CAT_EXEC_PANEL_SCAN)
			dev_info(cd->dev,
				"%s: cyttsp4_CaT_IRQ=%02X %02X %02X\n",
				__func__, mode[0], mode[1], mode[2]);
		dev_vdbg(dev, "%s: CaT\n", __func__);
		break;
	case CY_HST_SYSINFO:
		cur_mode = CY_MODE_SYSINFO;
		dev_vdbg(dev, "%s: sysinfo\n", __func__);
		break;
	default:
		cur_mode = CY_MODE_UNKNOWN;
		dev_err(dev, "%s: unknown HST mode 0x%02X\n", __func__,
			mode[0]);
		break;
	}

	/* Check whether this IRQ should be ignored (internal) */
	if (cd->int_status & CY_INT_IGNORE) {
		if (IS_DEEP_SLEEP_CONFIGURED(cd->easy_wakeup_gesture)) {
			/* Put device back to sleep on premature wakeup */
			dev_dbg(dev, "%s: Put device back to sleep\n",
				__func__);
			_cyttsp4_put_device_into_deep_sleep(cd, mode[0]);
			goto cyttsp4_irq_exit;
		}
		/* Check for Wait for Event command */
		if ((mode[cmd_ofs] & CY_CMD_MASK) == CY_CMD_OP_WAIT_FOR_EVENT
				&& mode[cmd_ofs] & CY_CMD_COMPLETE) {
			cd->wake_initiated_by_device = 1;
			call_atten_cb(cd, CY_ATTEN_WAKE, 0);
			goto cyttsp4_irq_handshake;
		}
	}

	/* Check for wake up interrupt */
	if (cd->int_status & CY_INT_AWAKE) {
		cd->int_status &= ~CY_INT_AWAKE;
		wake_up(&cd->wait_q);
		dev_vdbg(dev, "%s: Received wake up interrupt\n", __func__);
		goto cyttsp4_irq_handshake;
	}

	/* Expecting mode change interrupt */
	if ((cd->int_status & CY_INT_MODE_CHANGE)
			&& (mode[0] & CY_HST_MODE_CHANGE) == 0) {
		cd->int_status &= ~CY_INT_MODE_CHANGE;
		dev_dbg(dev, "%s: finish mode switch m=%s -> m=%s\n",
			__func__, mode2str(cd->mode), mode2str(cur_mode));
		cd->mode = cur_mode;
		wake_up(&cd->wait_q);
		goto cyttsp4_irq_handshake;
	}

	/* compare current core mode to current device mode */
	dev_vdbg(dev, "%s: cd->mode=%d cur_mode=%d\n",
			__func__, cd->mode, cur_mode);
	if ((mode[0] & CY_HST_MODE_CHANGE) == 0 && cd->mode != cur_mode) {
		/* Unexpected mode change occurred */
		dev_err(dev, "%s %d->%d 0x%x\n", __func__, cd->mode,
				cur_mode, cd->int_status);
		dev_vdbg(dev, "%s: Unexpected mode change, startup\n",
				__func__);
		cyttsp4_queue_startup(cd);
		goto cyttsp4_irq_exit;
	}

	/* Expecting command complete interrupt */
	dev_vdbg(dev, "%s: command byte:0x%x, toggle:0x%x\n",
			__func__, mode[cmd_ofs], cd->cmd_toggle);
	if ((cd->int_status & CY_INT_EXEC_CMD)
			&& mode[cmd_ofs] & CY_CMD_COMPLETE) {
		command_complete = true;
		cd->int_status &= ~CY_INT_EXEC_CMD;
		dev_vdbg(dev, "%s: Received command complete interrupt\n",
				__func__);
		wake_up(&cd->wait_q);
		/*
		 * It is possible to receive a single interrupt for
		 * command complete and touch/button status report.
		 * Continue processing for a possible status report.
		 */
	}

	/* Copy the mode registers */
	if (cd->sysinfo.xy_mode)
		memcpy(cd->sysinfo.xy_mode, mode, sizeof(mode));

	/* This should be status report, read status and touch regs */
	if (cd->mode == CY_MODE_OPERATIONAL) {
		dev_vdbg(dev, "%s: Read status and touch registers\n",
			__func__);
		rc = cyttsp4_load_status_and_touch_regs(cd, !command_complete);
		if (rc < 0)
			dev_err(dev, "%s: fail read mode/touch regs r=%d\n",
				__func__, rc);
	}

	/* attention IRQ */
	call_atten_cb(cd, CY_ATTEN_IRQ, cd->mode);

cyttsp4_irq_handshake:
	/* handshake the event */
	dev_vdbg(dev, "%s: Handshake mode=0x%02X r=%d\n",
			__func__, mode[0], rc);
	rc = cyttsp4_handshake(cd, mode[0]);
	if (rc < 0)
		dev_err(dev, "%s: Fail handshake mode=0x%02X r=%d\n",
				__func__, mode[0], rc);

	/*
	 * a non-zero udelay period is required for using
	 * IRQF_TRIGGER_LOW in order to delay until the
	 * device completes isr deassert
	 */
	udelay(cd->cpdata->level_irq_udelay);

cyttsp4_irq_exit:
	mutex_unlock(&cd->system_lock);
	dev_vdbg(dev, "%s: irq done\n", __func__);
	return IRQ_HANDLED;
}

static void cyttsp4_start_wd_timer(struct cyttsp4_core_data *cd)
{
	if (!CY_WATCHDOG_TIMEOUT)
		return;

	mod_timer(&cd->watchdog_timer, jiffies +
			msecs_to_jiffies(CY_WATCHDOG_TIMEOUT));
}

static void cyttsp4_stop_wd_timer(struct cyttsp4_core_data *cd)
{
	if (!CY_WATCHDOG_TIMEOUT)
		return;

	/*
	 * Ensure we wait until the watchdog timer
	 * running on a different CPU finishes
	 */
	timer_shutdown_sync(&cd->watchdog_timer);
	cancel_work_sync(&cd->watchdog_work);
}

static void cyttsp4_watchdog_timer(struct timer_list *t)
{
	struct cyttsp4_core_data *cd = from_timer(cd, t, watchdog_timer);

	dev_vdbg(cd->dev, "%s: Watchdog timer triggered\n", __func__);

	schedule_work(&cd->watchdog_work);

	return;
}

int cyttsp4_write_(struct device *dev, int mode, u16 addr, const void *buf,
	int size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc = 0;

	mutex_lock(&cd->adap_lock);
	if (mode != cd->mode) {
		dev_dbg(dev, "%s: %s (having %x while %x requested)\n",
			__func__, "attempt to write in missing mode",
			cd->mode, mode);
		rc = -EACCES;
		goto exit;
	}
	rc = cyttsp4_adap_write(cd, addr, buf, size);
exit:
	mutex_unlock(&cd->adap_lock);
	return rc;
}

int cyttsp4_read_(struct device *dev, int mode, u16 addr, void *buf, int size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc = 0;

	mutex_lock(&cd->adap_lock);
	if (mode != cd->mode) {
		dev_dbg(dev, "%s: %s (having %x while %x requested)\n",
			__func__, "attempt to read in missing mode",
			cd->mode, mode);
		rc = -EACCES;
		goto exit;
	}
	rc = cyttsp4_adap_read(cd, addr, buf, size);
exit:
	mutex_unlock(&cd->adap_lock);
	return rc;
}

int _cyttsp4_subscribe_attention(struct device *dev,
	enum cyttsp4_atten_type type, char id, int (*func)(struct device *),
	int mode)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	struct atten_node *atten, *atten_new;

	atten_new = kzalloc(sizeof(*atten_new), GFP_KERNEL);
	if (!atten_new) {
		dev_err(cd->dev, "%s: Fail alloc atten node\n", __func__);
		return -ENOMEM;
	}

	dev_dbg(cd->dev, "%s from '%s'\n", __func__, dev_name(cd->dev));

	spin_lock(&cd->spinlock);
	list_for_each_entry(atten, &cd->atten_list[type], node) {
		if (atten->id == id && atten->mode == mode) {
			spin_unlock(&cd->spinlock);
			kfree(atten_new);
			dev_vdbg(cd->dev, "%s: %s=%p %s=%d\n",
				 __func__,
				 "already subscribed attention",
				 dev, "mode", mode);

			return 0;
		}
	}

	atten_new->id = id;
	atten_new->dev = dev;
	atten_new->mode = mode;
	atten_new->func = func;

	list_add(&atten_new->node, &cd->atten_list[type]);
	spin_unlock(&cd->spinlock);

	return 0;
}

int _cyttsp4_unsubscribe_attention(struct device *dev,
	enum cyttsp4_atten_type type, char id, int (*func)(struct device *),
	int mode)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	struct atten_node *atten, *atten_n;

	spin_lock(&cd->spinlock);
	list_for_each_entry_safe(atten, atten_n, &cd->atten_list[type], node) {
		if (atten->id == id && atten->mode == mode) {
			list_del(&atten->node);
			spin_unlock(&cd->spinlock);
			kfree(atten);
			dev_vdbg(cd->dev, "%s: %s=%p %s=%d\n",
				__func__,
				"unsub for atten->ttsp", atten->dev,
				"atten->mode", atten->mode);
			return 0;
		}
	}
	spin_unlock(&cd->spinlock);

	return -ENODEV;
}

int request_exclusive(struct cyttsp4_core_data *cd, void *ownptr,
		int timeout_ms)
{
	int t = msecs_to_jiffies(timeout_ms);
	bool with_timeout = (timeout_ms != 0);

	mutex_lock(&cd->system_lock);
	if (!cd->exclusive_dev && cd->exclusive_waits == 0) {
		cd->exclusive_dev = ownptr;
		goto exit;
	}

	cd->exclusive_waits++;
wait:
	mutex_unlock(&cd->system_lock);
	if (with_timeout) {
		t = wait_event_timeout(cd->wait_q, !cd->exclusive_dev, t);
		if (IS_TMO(t)) {
			dev_err(cd->dev, "%s: tmo waiting exclusive access\n",
				__func__);
			mutex_lock(&cd->system_lock);
			cd->exclusive_waits--;
			mutex_unlock(&cd->system_lock);
			return -ETIME;
		}
	} else {
		wait_event(cd->wait_q, !cd->exclusive_dev);
	}
	mutex_lock(&cd->system_lock);
	if (cd->exclusive_dev)
		goto wait;
	cd->exclusive_dev = ownptr;
	cd->exclusive_waits--;
exit:
	mutex_unlock(&cd->system_lock);
	dev_vdbg(cd->dev, "%s: request_exclusive ok=%p\n",
		__func__, ownptr);

	return 0;
}

static int cyttsp4_request_exclusive_(struct device *dev,
		int timeout_ms)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	return request_exclusive(cd, (void *)dev, timeout_ms);
}

/*
 * returns error if was not owned
 */
int release_exclusive(struct cyttsp4_core_data *cd, void *ownptr)
{
	mutex_lock(&cd->system_lock);
	if (cd->exclusive_dev != ownptr) {
		mutex_unlock(&cd->system_lock);
		return -EINVAL;
	}

	dev_vdbg(cd->dev, "%s: exclusive_dev %p freed\n",
		__func__, cd->exclusive_dev);
	cd->exclusive_dev = NULL;
	wake_up(&cd->wait_q);
	mutex_unlock(&cd->system_lock);
	return 0;
}

static int cyttsp4_release_exclusive_(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	return release_exclusive(cd, (void *)dev);
}

static int cyttsp4_wait_bl_heartbeat(struct cyttsp4_core_data *cd)
{
	long t;
	int rc = 0;

	/* wait heartbeat */
	dev_vdbg(cd->dev, "%s: wait heartbeat...\n", __func__);
	t = wait_event_timeout(cd->wait_q, cd->mode == CY_MODE_BOOTLOADER,
			msecs_to_jiffies(CY_CORE_RESET_AND_WAIT_TIMEOUT));
	if (IS_TMO(t)) {
		dev_err(cd->dev, "%s: tmo waiting bl heartbeat cd->mode=%d\n",
			__func__, cd->mode);
		rc = -ETIME;
	}

	return rc;
}

static int cyttsp4_wait_sysinfo_mode(struct cyttsp4_core_data *cd)
{
	long t;

	dev_dbg(cd->dev, "%s: wait sysinfo...\n", __func__);

	t = wait_event_timeout(cd->wait_q, cd->mode == CY_MODE_SYSINFO,
			msecs_to_jiffies(CY_CORE_WAIT_SYSINFO_MODE_TIMEOUT));
	if (IS_TMO(t)) {
		dev_err(cd->dev, "%s: tmo waiting exit bl cd->mode=%d\n",
			__func__, cd->mode);
		mutex_lock(&cd->system_lock);
		cd->int_status &= ~CY_INT_MODE_CHANGE;
		mutex_unlock(&cd->system_lock);
		return -ETIME;
	}

	return 0;
}

static int cyttsp4_reset_and_wait(struct cyttsp4_core_data *cd)
{
	int rc;

	/* reset hardware */
	mutex_lock(&cd->system_lock);
	dev_dbg(cd->dev, "%s: reset hw...\n", __func__);
	rc = cyttsp4_hw_reset_(cd);
	cd->mode = CY_MODE_UNKNOWN;
	mutex_unlock(&cd->system_lock);
	if (rc < 0) {
		dev_err(cd->dev, "%s: %s dev='%s' r=%d\n", __func__,
			"Fail hw reset", dev_name(cd->dev), rc);
		return rc;
	}

	return cyttsp4_wait_bl_heartbeat(cd);
}

/*
 * returns err if refused or timeout; block until mode change complete
 * bit is set (mode change interrupt)
 */
static int set_mode(struct cyttsp4_core_data *cd, int new_mode)
{
	u8 new_dev_mode;
	u8 mode;
	long t;
	int rc;

	switch (new_mode) {
	case CY_MODE_OPERATIONAL:
		new_dev_mode = CY_HST_OPERATE;
		break;
	case CY_MODE_SYSINFO:
		new_dev_mode = CY_HST_SYSINFO;
		break;
	case CY_MODE_CAT:
		new_dev_mode = CY_HST_CAT;
		break;
	default:
		dev_err(cd->dev, "%s: invalid mode: %02X(%d)\n",
			__func__, new_mode, new_mode);
		return -EINVAL;
	}

	/* change mode */
	dev_dbg(cd->dev, "%s: new_dev_mode=%02X new_mode=%s\n",
			__func__, new_dev_mode, mode2str(new_mode));

	mutex_lock(&cd->system_lock);
	rc = cyttsp4_adap_read(cd, CY_REG_BASE, &mode, sizeof(mode));
	if (rc < 0) {
		mutex_unlock(&cd->system_lock);
		dev_err(cd->dev, "%s: Fail read mode r=%d\n",
			__func__, rc);
		goto exit;
	}

	/* Clear device mode bits and set to new mode */
	mode &= ~CY_HST_DEVICE_MODE;
	mode |= new_dev_mode | CY_HST_MODE_CHANGE;

	cd->int_status |= CY_INT_MODE_CHANGE;
	rc = cyttsp4_adap_write(cd, CY_REG_BASE, &mode, sizeof(mode));
	mutex_unlock(&cd->system_lock);
	if (rc < 0) {
		dev_err(cd->dev, "%s: Fail write mode change r=%d\n",
				__func__, rc);
		goto exit;
	}

	/* wait for mode change done interrupt */
	t = wait_event_timeout(cd->wait_q,
			(cd->int_status & CY_INT_MODE_CHANGE) == 0,
			msecs_to_jiffies(CY_CORE_MODE_CHANGE_TIMEOUT));
	dev_dbg(cd->dev, "%s: back from wait t=%ld cd->mode=%s\n",
			__func__, t, mode2str(cd->mode));

	if (IS_TMO(t)) {
		dev_err(cd->dev, "%s: %s\n", __func__,
				"tmo waiting mode change");
		mutex_lock(&cd->system_lock);
		cd->int_status &= ~CY_INT_MODE_CHANGE;
		mutex_unlock(&cd->system_lock);
		rc = -EINVAL;
	}

exit:
	return rc;
}

/*
 * returns err if refused or timeout(core uses fixed timeout period) occurs;
 * blocks until ISR occurs
 */
static int cyttsp4_request_reset_(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc;

	mutex_lock(&cd->system_lock);
	cd->sysinfo.ready = false;
	mutex_unlock(&cd->system_lock);

	rc = cyttsp4_reset_and_wait(cd);
	if (rc < 0)
		dev_err(dev, "%s: Error on h/w reset r=%d\n", __func__, rc);

	return rc;
}

/*
 * returns err if refused ; if no error then restart has completed
 * and system is in normal operating mode
 */
static int cyttsp4_startup(struct cyttsp4_core_data *cd);
int cyttsp4_fw_calibrate(struct device *dev);
static int cyttsp4_request_restart_(struct device *dev, bool wait)
// called after loader downloaded new firmware
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc = 0;

	mutex_lock(&cd->system_lock);
	cd->bl_fast_exit = false;
	mutex_unlock(&cd->system_lock);

	rc = cyttsp4_startup(cd);
	if (rc < 0) {
		dev_err(dev, "%s: fail startup, rc=%d\n", __func__,
			rc);
		return rc;
	}

	if (!cd->pdata->loader_pdata) {
		return rc;
	}

	if (cd->pdata->loader_pdata->flags &
		CY_LOADER_FLAG_CALIBRATE_AFTER_FW_UPGRADE) {
		dev_dbg(dev, "%s: calibrate after fw upgrade\n", __func__);
		rc = cyttsp4_fw_calibrate(cd->dev);
		if (rc < 0) {
			dev_err(dev, "%s: fail startup, rc=%d\n", __func__,
				rc);
			return rc;
		}
	}

	return 0;
}

static int cyttsp4_request_set_mode_(struct device *dev, int mode)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc;

	rc = set_mode(cd, mode);
	if (rc < 0)
		dev_err(dev, "%s: fail set_mode=%02X(%d)\n",
			__func__, cd->mode, cd->mode);

	return rc;
}

/*
 * returns NULL if sysinfo has not been acquired from the device yet
 */
struct cyttsp4_sysinfo *cyttsp4_request_sysinfo_(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	bool ready;

	mutex_lock(&cd->system_lock);
	ready = cd->sysinfo.ready;
	mutex_unlock(&cd->system_lock);
	if (ready)
		return &cd->sysinfo;

	return NULL;
}

static struct cyttsp4_loader_platform_data *cyttsp4_request_loader_pdata_(
		struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	return cd->pdata->loader_pdata;
}

static int cyttsp4_request_handshake_(struct device *dev, u8 mode)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc;

	rc = cyttsp4_handshake(cd, mode);
	if (rc < 0)
		dev_err(dev, "%s: Fail handshake r=%d\n", __func__, rc);

	return rc;
}

static int cyttsp4_request_toggle_lowpower_(struct device *dev,
		u8 mode)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc = cyttsp4_toggle_low_power(cd, mode);
	if (rc < 0)
		dev_err(dev, "%s: Fail toggle low power r=%d\n", __func__, rc);
	return rc;
}

static int _cyttsp4_wait_cmd_exec(struct cyttsp4_core_data *cd, int timeout_ms)
{
	struct device *dev = cd->dev;
	int rc;

	rc = wait_event_timeout(cd->wait_q,
			(cd->int_status & CY_INT_EXEC_CMD) == 0,
			msecs_to_jiffies(timeout_ms));
	if (IS_TMO(rc)) {
		dev_err(dev, "%s: Command execution timed out\n",
				__func__);
		cd->int_status &= ~CY_INT_EXEC_CMD;
		return -ETIME;
	}
	return 0;
}

static int _get_cmd_offs(struct cyttsp4_core_data *cd, u8 mode)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	struct device *dev = cd->dev;
	int cmd_ofs;

	switch (mode) {
	case CY_MODE_CAT:
		cmd_ofs = CY_REG_CAT_CMD;
		break;
	case CY_MODE_OPERATIONAL:
		cmd_ofs = si->si_ofs.cmd_ofs;
		break;
	default:
		dev_err(dev, "%s: Unsupported mode %x for exec cmd\n",
				__func__, mode);
		return -EACCES;
	}

	return cmd_ofs;
}

/*
 * Send command to device for CAT and OP modes
 * return negative value on error, 0 on success
 */
static int _cyttsp4_exec_cmd(struct cyttsp4_core_data *cd, u8 mode,
		u8 *cmd_buf, size_t cmd_size)
{
	struct device *dev = cd->dev;
	int cmd_ofs;
	int cmd_param_ofs;
	u8 command;
	u8 *cmd_param_buf;
	size_t cmd_param_size;
	int rc;

	if (mode != cd->mode) {
		dev_err(dev, "%s: %s (having %x while %x requested)\n",
				__func__, "attempt to exec cmd in missing mode",
				cd->mode, mode);
		return -EACCES;
	}

	cmd_ofs = _get_cmd_offs(cd, mode);
	if (cmd_ofs < 0)
		return -EACCES;

	cmd_param_ofs = cmd_ofs + 1;
	cmd_param_buf = cmd_buf + 1;
	cmd_param_size = cmd_size - 1;

	/* Check if complete is set, so write new command */
	rc = cyttsp4_adap_read(cd, cmd_ofs, &command, 1);
	if (rc < 0) {
		dev_err(dev, "%s: Error on read r=%d\n", __func__, rc);
		return rc;
	}

	cd->cmd_toggle = GET_TOGGLE(command);
	cd->int_status |= CY_INT_EXEC_CMD;

	if ((command & CY_CMD_COMPLETE_MASK) == 0)
		return -EBUSY;

	/*
	 * Write new command
	 * Only update command bits 0:5
	 * Clear command complete bit & toggle bit
	 */
	cmd_buf[0] = cmd_buf[0] & CY_CMD_MASK;
	/* Write command parameters first */
	if (cmd_size > 1) {
		rc = cyttsp4_adap_write(cd, cmd_param_ofs, cmd_param_buf,
				cmd_param_size);
		if (rc < 0) {
			dev_err(dev, "%s: Error on write command parameters r=%d\n",
				__func__, rc);
			return rc;
		}
	}
	/* Write the command */
	rc = cyttsp4_adap_write(cd, cmd_ofs, cmd_buf, 1);
	if (rc < 0) {
		dev_err(dev, "%s: Error on write command r=%d\n",
				__func__, rc);
		return rc;
	}

#ifdef DEBUG
	dev_dbg(dev, "%s: cmd=%s rc=%d\n", __func__,
		cy_cmd_str(mode, cmd_buf[0]), rc);
#endif
	return 0;
}

static int cyttsp4_exec_cmd(struct cyttsp4_core_data *cd, u8 mode,
		u8 *cmd_buf, size_t cmd_size, u8 *return_buf,
		size_t return_buf_size, int timeout_ms)
{
	struct device *dev = cd->dev;
	int cmd_ofs;
	int cmd_return_ofs;
	int rc;

	mutex_lock(&cd->system_lock);
	rc = _cyttsp4_exec_cmd(cd, mode, cmd_buf, cmd_size);
	mutex_unlock(&cd->system_lock);

	if (rc == -EBUSY) {
		rc = _cyttsp4_wait_cmd_exec(cd, CY_COMMAND_COMPLETE_TIMEOUT);
		if (rc)
			return rc;
		mutex_lock(&cd->system_lock);
		rc = _cyttsp4_exec_cmd(cd, mode, cmd_buf, cmd_size);
		mutex_unlock(&cd->system_lock);
	}

	if (rc < 0)
		return rc;

	if (timeout_ms == 0)
		return 0;

	/*
	 * Wait command to be completed
	 */
	rc = _cyttsp4_wait_cmd_exec(cd, timeout_ms);
	if (rc < 0)
		return rc;

	if (return_buf_size == 0 || return_buf == NULL)
		return 0;

	mutex_lock(&cd->system_lock);
	cmd_ofs = _get_cmd_offs(cd, mode);
	mutex_unlock(&cd->system_lock);
	if (cmd_ofs < 0)
		return -EACCES;

	cmd_return_ofs = cmd_ofs + 1;

	rc = cyttsp4_adap_read(cd, cmd_return_ofs, return_buf, return_buf_size);
	if (rc < 0) {
		dev_err(dev, "%s: Error on read 3 r=%d\n", __func__, rc);
		return rc;
	}

	return 0;
}

static int cyttsp4_request_exec_cmd_(struct device *dev, u8 mode,
		u8 *cmd_buf, size_t cmd_size, u8 *return_buf,
		size_t return_buf_size, int timeout_ms)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	return cyttsp4_exec_cmd(cd, mode, cmd_buf, cmd_size,
			return_buf, return_buf_size, timeout_ms);
}

static int cyttsp4_get_parameter(struct cyttsp4_core_data *cd, u8 param_id,
		u32 *param_value)
{
	u8 command_buf[CY_CMD_OP_GET_PARAM_CMD_SZ];
	u8 return_buf[CY_CMD_OP_GET_PARAM_RET_SZ];
	u8 param_size;
	u8 *value_buf;
	int rc;

	command_buf[0] = CY_CMD_OP_GET_PARAM;
	command_buf[1] = param_id;
	rc = cyttsp4_exec_cmd(cd, CY_MODE_OPERATIONAL,
			command_buf, CY_CMD_OP_GET_PARAM_CMD_SZ,
			return_buf, CY_CMD_OP_GET_PARAM_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
	if (rc < 0) {
		dev_err(cd->dev, "%s: Unable to execute get parameter command.\n",
			__func__);
		return rc;
	}

	if (return_buf[0] != param_id) {
		dev_err(cd->dev, "%s: Fail to execute get parameter command.\n",
			__func__);
		return -EIO;
	}

	param_size = return_buf[1];
	value_buf = &return_buf[2];

	*param_value = 0;
	while (param_size--)
		*param_value += *(value_buf++) << (8 * param_size);

	return 0;
}

static int cyttsp4_set_parameter(struct cyttsp4_core_data *cd, u8 param_id,
		u8 param_size, u32 param_value)
{
	u8 command_buf[CY_CMD_OP_SET_PARAM_CMD_SZ];
	u8 return_buf[CY_CMD_OP_SET_PARAM_RET_SZ];
	int rc;

	command_buf[0] = CY_CMD_OP_SET_PARAM;
	command_buf[1] = param_id;
	command_buf[2] = param_size;

	if (param_size == 1) {
		command_buf[3] = (u8)param_value;
	} else if (param_size == 2) {
		command_buf[3] = (u8)(param_value >> 8);
		command_buf[4] = (u8)param_value;
	} else if (param_size == 4) {
		command_buf[3] = (u8)(param_value >> 24);
		command_buf[4] = (u8)(param_value >> 16);
		command_buf[5] = (u8)(param_value >> 8);
		command_buf[6] = (u8)param_value;
	} else {
		dev_err(cd->dev, "%s: Invalid parameter size %d\n",
			__func__, param_size);
		return -EINVAL;
	}

	rc = cyttsp4_exec_cmd(cd, CY_MODE_OPERATIONAL,
			command_buf, 3 + param_size,
			return_buf, CY_CMD_OP_SET_PARAM_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
	if (rc < 0) {
		dev_err(cd->dev, "%s: Unable to execute set parameter command.\n",
			__func__);
		return rc;
	}

	if (return_buf[0] != param_id || return_buf[1] != param_size) {
		dev_err(cd->dev, "%s: Fail to execute set parameter command.\n",
			__func__);
		return -EIO;
	}

	return 0;
}

static int cyttsp4_get_scantype(struct cyttsp4_core_data *cd, u8 *scantype)
{
	int rc;
	u32 value;

	rc = cyttsp4_get_parameter(cd, CY_RAM_ID_SCAN_TYPE, &value);
	if (!rc)
		*scantype = (u8)value;

	return rc;
}

static int cyttsp4_set_scantype(struct cyttsp4_core_data *cd, u8 scantype)
{
	int rc;

	rc = cyttsp4_set_parameter(cd, CY_RAM_ID_SCAN_TYPE, 1, scantype);

	return rc;
}

static u8 _cyttsp4_generate_new_scantype(struct cyttsp4_core_data *cd)
{
	u8 new_scantype = cd->default_scantype;

	if (cd->apa_mc_en)
		new_scantype |= CY_SCAN_TYPE_APA_MC;
	if (cd->glove_en)
		new_scantype |= CY_SCAN_TYPE_GLOVE;
	if (cd->stylus_en)
		new_scantype |= CY_SCAN_TYPE_STYLUS;
	if (cd->proximity_en)
		new_scantype |= CY_SCAN_TYPE_PROXIMITY;

	return new_scantype;
}

static int cyttsp4_set_new_scan_type(struct cyttsp4_core_data *cd,
		u8 scan_type, bool enable)
{
	int inc = enable ? 1 : -1;
	int *en;
	int rc;
	u8 new_scantype;

	switch (scan_type) {
	case CY_ST_GLOVE:
		en = &cd->glove_en;
		break;
	case CY_ST_STYLUS:
		en = &cd->stylus_en;
		break;
	case CY_ST_PROXIMITY:
		en = &cd->proximity_en;
		break;
	case CY_ST_APA_MC:
		en = &cd->apa_mc_en;
		break;
	default:
		return -EINVAL;
	}

	*en += inc;

	new_scantype = _cyttsp4_generate_new_scantype(cd);

	rc = cyttsp4_set_scantype(cd, new_scantype);
	if (rc)
		*en -= inc;

	return rc;
}

static int cyttsp4_set_proximity(struct cyttsp4_core_data *cd, bool enable)
{
	int touchmode, touchmode_orig;
	int rc;

	rc = cyttsp4_get_parameter(cd, CY_RAM_ID_TOUCHMODE_ENABLED, &touchmode);
	if (rc)
		return rc;
	touchmode_orig = touchmode;

	if (enable)
		touchmode |= 0x80;
	else
		touchmode &= 0x7F;

	if (touchmode_orig == touchmode)
		return rc;

	rc = cyttsp4_set_parameter(cd, CY_RAM_ID_TOUCHMODE_ENABLED, 1,
			touchmode);
	return rc;
}

static int cyttsp4_request_enable_scan_type_(struct device *dev, u8 scan_type)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);

	if (cd->cpdata->flags & CY_CORE_FLAG_SCAN_MODE_USES_RAM_ID_SCAN_TYPE) {
		return cyttsp4_set_new_scan_type(cd, scan_type, true);
	} else {
		if (scan_type == CY_ST_PROXIMITY)
			return cyttsp4_set_proximity(cd, true);
	}
	return -EINVAL;
}

static int cyttsp4_request_disable_scan_type_(struct device *dev, u8 scan_type)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);

	if (cd->cpdata->flags & CY_CORE_FLAG_SCAN_MODE_USES_RAM_ID_SCAN_TYPE) {
		return cyttsp4_set_new_scan_type(cd, scan_type, false);
	} else {
		if (scan_type == CY_ST_PROXIMITY)
			return cyttsp4_set_proximity(cd, false);
	}
	return -EINVAL;
}

static int cyttsp4_read_config_block(struct cyttsp4_core_data *cd, u8 ebid,
		u16 row, u8 *data, u16 length)
{
	u8 command_buf[CY_CMD_CAT_READ_CFG_BLK_CMD_SZ];
	u8 *return_buf;
	int return_buf_sz;
	u16 crc;
	int rc;

	/* Allocate buffer for read config block command response
	 * Header(5) + Data(length) + CRC(2)
	 */
	return_buf_sz = CY_CMD_CAT_READ_CFG_BLK_RET_SZ + length;
	return_buf = kmalloc(return_buf_sz, GFP_KERNEL);
	if (!return_buf) {
		dev_err(cd->dev, "%s: Cannot allocate buffer\n",
			__func__);
		rc = -ENOMEM;
		goto exit;
	}

	command_buf[0] = CY_CMD_CAT_READ_CFG_BLK;
	command_buf[1] = HI_BYTE(row);
	command_buf[2] = LO_BYTE(row);
	command_buf[3] = HI_BYTE(length);
	command_buf[4] = LO_BYTE(length);
	command_buf[5] = ebid;

	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			command_buf, CY_CMD_CAT_READ_CFG_BLK_CMD_SZ,
			return_buf, return_buf_sz,
			CY_COMMAND_COMPLETE_TIMEOUT);
	if (rc) {
		dev_err(cd->dev, "%s: Error executing command r=%d\n",
			__func__, rc);
		goto free_buffer;
	}

	crc = cyttsp4_calc_app_crc(
		&return_buf[CY_CMD_CAT_READ_CFG_BLK_RET_HDR_SZ], length);

	/* Validate response */
	if (return_buf[0] != CY_CMD_STATUS_SUCCESS
			|| return_buf[1] != ebid
			|| return_buf[2] != HI_BYTE(length)
			|| return_buf[3] != LO_BYTE(length)
			|| return_buf[CY_CMD_CAT_READ_CFG_BLK_RET_HDR_SZ
				+ length] != HI_BYTE(crc)
			|| return_buf[CY_CMD_CAT_READ_CFG_BLK_RET_HDR_SZ
				+ length + 1] != LO_BYTE(crc)) {
		dev_err(cd->dev, "%s: Fail executing command\n",
				__func__);
		rc = -EINVAL;
		goto free_buffer;
	}

	memcpy(data, &return_buf[CY_CMD_CAT_READ_CFG_BLK_RET_HDR_SZ], length);

	cyttsp4_pr_buf(cd->dev, cd->pr_buf, data, length, "read_config_block");

free_buffer:
	kfree(return_buf);
exit:
	return rc;
}

static int cyttsp4_write_config_block(struct cyttsp4_core_data *cd, u8 ebid,
		u16 row, const u8 *data, u16 length)
{
	u8 return_buf[CY_CMD_CAT_WRITE_CFG_BLK_RET_SZ];
	u8 *command_buf;
	int command_buf_sz;
	u16 crc;
	int rc;

	/* Allocate buffer for write config block command
	 * Header(6) + Data(length) + Security Key(8) + CRC(2)
	 */
	command_buf_sz = CY_CMD_CAT_WRITE_CFG_BLK_CMD_SZ + length
		+ sizeof(security_key);
	command_buf = kmalloc(command_buf_sz, GFP_KERNEL);
	if (!command_buf) {
		dev_err(cd->dev, "%s: Cannot allocate buffer\n",
			__func__);
		rc = -ENOMEM;
		goto exit;
	}

	crc = cyttsp4_calc_app_crc(data, length);

	command_buf[0] = CY_CMD_CAT_WRITE_CFG_BLK;
	command_buf[1] = HI_BYTE(row);
	command_buf[2] = LO_BYTE(row);
	command_buf[3] = HI_BYTE(length);
	command_buf[4] = LO_BYTE(length);
	command_buf[5] = ebid;

	command_buf[CY_CMD_CAT_WRITE_CFG_BLK_CMD_HDR_SZ + length
		+ sizeof(security_key)] = HI_BYTE(crc);
	command_buf[CY_CMD_CAT_WRITE_CFG_BLK_CMD_HDR_SZ + 1 + length
		+ sizeof(security_key)] = LO_BYTE(crc);

	memcpy(&command_buf[CY_CMD_CAT_WRITE_CFG_BLK_CMD_HDR_SZ], data,
		length);
	memcpy(&command_buf[CY_CMD_CAT_WRITE_CFG_BLK_CMD_HDR_SZ + length],
		security_key, sizeof(security_key));

	cyttsp4_pr_buf(cd->dev, cd->pr_buf, command_buf, command_buf_sz,
		"write_config_block");

	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			command_buf, command_buf_sz,
			return_buf, CY_CMD_CAT_WRITE_CFG_BLK_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
	if (rc) {
		dev_err(cd->dev, "%s: Error executing command r=%d\n",
			__func__, rc);
		goto free_buffer;
	}

	/* Validate response */
	if (return_buf[0] != CY_CMD_STATUS_SUCCESS
			|| return_buf[1] != ebid
			|| return_buf[2] != HI_BYTE(length)
			|| return_buf[3] != LO_BYTE(length)) {
		dev_err(cd->dev, "%s: Fail executing command\n",
				__func__);
		rc = -EINVAL;
		goto free_buffer;
	}

free_buffer:
	kfree(command_buf);
exit:
	return rc;
}

static int cyttsp4_get_config_row_size(struct cyttsp4_core_data *cd,
		u16 *config_row_size)
{
	u8 command_buf[CY_CMD_CAT_GET_CFG_ROW_SIZE_CMD_SZ];
	u8 return_buf[CY_CMD_CAT_GET_CFG_ROW_SIZE_RET_SZ];
	int rc;

	command_buf[0] = CY_CMD_CAT_GET_CFG_ROW_SZ;

	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			command_buf, CY_CMD_CAT_GET_CFG_ROW_SIZE_CMD_SZ,
			return_buf, CY_CMD_CAT_GET_CFG_ROW_SIZE_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
	if (rc) {
		dev_err(cd->dev, "%s: Error executing command r=%d\n",
			__func__, rc);
		goto exit;
	}

	*config_row_size = get_unaligned_be16(&return_buf[0]);

exit:
	return rc;
}

static int cyttsp4_request_config_row_size_(struct device *dev,
		u16 *config_row_size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	return cyttsp4_get_config_row_size(cd, config_row_size);
}

static int cyttsp4_verify_config_block_crc(struct cyttsp4_core_data *cd,
		u8 ebid, u16 *calc_crc, u16 *stored_crc, bool *match)
{
	u8 command_buf[CY_CMD_CAT_VERIFY_CFG_BLK_CRC_CMD_SZ];
	u8 return_buf[CY_CMD_CAT_VERIFY_CFG_BLK_CRC_RET_SZ];
	int rc;

	command_buf[0] = CY_CMD_CAT_VERIFY_CFG_BLK_CRC;
	command_buf[1] = ebid;

	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			command_buf, CY_CMD_CAT_VERIFY_CFG_BLK_CRC_CMD_SZ,
			return_buf, CY_CMD_CAT_VERIFY_CFG_BLK_CRC_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
	if (rc) {
		dev_err(cd->dev, "%s: Error executing command r=%d\n",
			__func__, rc);
		goto exit;
	}

	*calc_crc = get_unaligned_be16(&return_buf[1]);
	*stored_crc = get_unaligned_be16(&return_buf[3]);
	if (match)
		*match = !return_buf[0];
exit:
	return rc;
}

static int cyttsp4_get_config_block_crc(struct cyttsp4_core_data *cd,
		u8 ebid, u16 *crc)
{
	u8 command_buf[CY_CMD_OP_GET_CFG_BLK_CRC_CMD_SZ];
	u8 return_buf[CY_CMD_OP_GET_CFG_BLK_CRC_RET_SZ];
	int rc;

	command_buf[0] = CY_CMD_OP_GET_CRC;
	command_buf[1] = ebid;

	rc = cyttsp4_exec_cmd(cd, CY_MODE_OPERATIONAL,
			command_buf, CY_CMD_OP_GET_CFG_BLK_CRC_CMD_SZ,
			return_buf, CY_CMD_OP_GET_CFG_BLK_CRC_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
	if (rc) {
		dev_err(cd->dev, "%s: Error executing command r=%d\n",
			__func__, rc);
		goto exit;
	}

	/* Validate response */
	if (return_buf[0] != CY_CMD_STATUS_SUCCESS) {
		dev_err(cd->dev, "%s: Fail executing command\n",
				__func__);
		rc = -EINVAL;
		goto exit;
	}

	*crc = get_unaligned_be16(&return_buf[1]);

exit:
	return rc;
}

static int cyttsp4_get_ttconfig_version(struct cyttsp4_core_data *cd,
		u16 *version)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	u8 data[CY_TTCONFIG_VERSION_OFFSET + CY_TTCONFIG_VERSION_SIZE];
	int rc;
	bool ready;

	mutex_lock(&cd->system_lock);
	ready = si->ready;
	mutex_unlock(&cd->system_lock);

	if (!ready) {
		rc  = -ENODEV;
		goto exit;
	}

	rc = cyttsp4_read_config_block(cd, CY_TCH_PARM_EBID,
			CY_TTCONFIG_VERSION_ROW, data, sizeof(data));
	if (rc) {
		dev_err(cd->dev, "%s: Error on read config block\n",
			__func__);
		goto exit;
	}

	*version = GET_FIELD16(si, &data[CY_TTCONFIG_VERSION_OFFSET]);

exit:
	return rc;
}

static int cyttsp4_get_config_length(struct cyttsp4_core_data *cd, u8 ebid,
		u16 *length, u16 *max_length)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	u8 data[CY_CONFIG_LENGTH_INFO_SIZE];
	int rc;
	bool ready;

	mutex_lock(&cd->system_lock);
	ready = si->ready;
	mutex_unlock(&cd->system_lock);

	if (!ready) {
		rc  = -ENODEV;
		goto exit;
	}

	rc = cyttsp4_read_config_block(cd, ebid, CY_CONFIG_LENGTH_INFO_OFFSET,
			data, sizeof(data));
	if (rc) {
		dev_err(cd->dev, "%s: Error on read config block\n",
			__func__);
		goto exit;
	}

	*length = GET_FIELD16(si, &data[CY_CONFIG_LENGTH_OFFSET]);
	*max_length = GET_FIELD16(si, &data[CY_CONFIG_MAXLENGTH_OFFSET]);

exit:
	return rc;
}

static int cyttsp4_write_config_common(struct cyttsp4_core_data *cd, u8 ebid,
		u16 offset, u8 *data, u16 length)
{
	u16 cur_block, cur_off, end_block, end_off;
	int copy_len;
	u16 config_row_size = 0;
	u8 *row_data = NULL;
	int rc;

	rc = cyttsp4_get_config_row_size(cd, &config_row_size);
	if (rc) {
		dev_err(cd->dev, "%s: Cannot get config row size\n",
			__func__);
		goto exit;
	}

	cur_block = offset / config_row_size;
	cur_off = offset % config_row_size;

	end_block = (offset + length) / config_row_size;
	end_off = (offset + length) % config_row_size;

	/* Check whether we need to fetch the whole block first */
	if (cur_off == 0)
		goto no_offset;

	row_data = kmalloc(config_row_size, GFP_KERNEL);
	if (!row_data) {
		dev_err(cd->dev, "%s: Cannot allocate buffer\n", __func__);
		rc = -ENOMEM;
		goto exit;
	}

	copy_len = (cur_block == end_block) ?
		length : config_row_size - cur_off;

	/* Read up to current offset, append the new data and write it back */
	rc = cyttsp4_read_config_block(cd, ebid, cur_block, row_data, cur_off);
	if (rc) {
		dev_err(cd->dev, "%s: Error on read config block\n", __func__);
		goto free_row_data;
	}

	memcpy(&row_data[cur_off], data, copy_len);

	rc = cyttsp4_write_config_block(cd, ebid, cur_block, row_data,
			cur_off + copy_len);
	if (rc) {
		dev_err(cd->dev, "%s: Error on initial write config block\n",
			__func__);
		goto free_row_data;
	}

	data += copy_len;
	cur_off = 0;
	cur_block++;

no_offset:
	while (cur_block < end_block) {
		rc = cyttsp4_write_config_block(cd, ebid, cur_block, data,
				config_row_size);
		if (rc) {
			dev_err(cd->dev, "%s: Error on write config block\n",
				__func__);
			goto free_row_data;
		}

		data += config_row_size;
		cur_block++;
	}

	/* Last block */
	if (cur_block == end_block) {
		rc = cyttsp4_write_config_block(cd, ebid, end_block, data,
				end_off);
		if (rc) {
			dev_err(cd->dev, "%s: Error on last write config block\n",
				__func__);
			goto free_row_data;
		}
	}

free_row_data:
	kfree(row_data);
exit:
	return rc;
}

static int cyttsp4_write_config(struct cyttsp4_core_data *cd, u8 ebid,
		u16 offset, u8 *data, u16 length) {
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	u16 crc_new, crc_old;
	u16 crc_offset;
	u16 conf_len;
	u8 crc_data[2];
	int rc;
	bool ready;

	mutex_lock(&cd->system_lock);
	ready = si->ready;
	mutex_unlock(&cd->system_lock);

	if (!ready) {
		rc  = -ENODEV;
		goto exit;
	}

	/* CRC is stored at config max length offset */
	rc = cyttsp4_get_config_length(cd, ebid, &conf_len, &crc_offset);
	if (rc) {
		dev_err(cd->dev, "%s: Error on get config length\n",
			__func__);
		goto exit;
	}

	/* Allow CRC update also */
	if (offset + length > crc_offset + 2) {
		dev_err(cd->dev, "%s: offset + length exceeds max length(%d)\n",
			__func__, crc_offset + 2);
		rc = -EINVAL;
		goto exit;
	}

	rc = cyttsp4_write_config_common(cd, ebid, offset, data, length);
	if (rc) {
		dev_err(cd->dev, "%s: Error on write config\n",
			__func__);
		goto exit;
	}

	/* Verify config block CRC */
	rc = cyttsp4_verify_config_block_crc(cd, ebid,
			&crc_new, &crc_old, NULL);
	if (rc) {
		dev_err(cd->dev, "%s: Error on verify config block crc\n",
			__func__);
		goto exit;
	}

	dev_vdbg(cd->dev, "%s: crc_new:%04X crc_old:%04X\n",
		__func__, crc_new, crc_old);

	if (crc_new == crc_old) {
		dev_vdbg(cd->dev, "%s: Calculated crc matches stored crc\n",
			__func__);
		goto exit;
	}

	PUT_FIELD16(si, crc_new, crc_data);

	rc = cyttsp4_write_config_common(cd, ebid, crc_offset, crc_data, 2);
	if (rc) {
		dev_err(cd->dev, "%s: Error on write config crc\n",
			__func__);
		goto exit;
	}

exit:
	return rc;
}

static int cyttsp4_request_write_config_(struct device *dev, u8 ebid,
		u16 offset, u8 *data, u16 length) {
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	return cyttsp4_write_config(cd, ebid, offset, data, length);
}

static struct cyttsp4_sysinfo *cyttsp4_update_sysinfo_(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc, rc1;
	bool ready;

	rc = request_exclusive(cd, (void *)dev, CY_CORE_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0) {
		dev_err(dev, "%s: Error on request exclusive r=%d\n",
				__func__, rc);
		goto error;
	}

	rc = set_mode(cd, CY_MODE_SYSINFO);
	if (rc < 0) {
		dev_err(dev, "%s: fail switch mode to CAT\n",
			__func__);
		goto err_release;
	}

	rc = cyttsp4_get_sysinfo_regs(cd);
	if (rc < 0)
		dev_err(dev, "%s: Error on cyttsp4_get_sysinfo_regs r=%d\n",
						__func__, rc);

	rc = set_mode(cd, CY_MODE_OPERATIONAL);
	if (rc < 0) {
		dev_err(dev, "%s: fail switch mode to OPMODE\n",
			__func__);
		goto err_release;
	}

err_release:
	rc1 = release_exclusive(cd, (void *)dev);
	if (rc1 < 0) {
		dev_err(dev, "%s: Error on release exclusive r=%d\n",
				__func__, rc1);
	}

	if (rc < 0)
		goto error;

	mutex_lock(&cd->system_lock);
	ready = cd->sysinfo.ready;
	mutex_unlock(&cd->system_lock);
	if (ready)
		return &cd->sysinfo;
error:
	return NULL;
}

static int cyttsp4_exec_panel_scan_(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	u8 cmd_buf[CY_CMD_CAT_EXECUTE_PANEL_SCAN_CMD_SZ];
	u8 return_buf[CY_CMD_CAT_EXECUTE_PANEL_SCAN_RET_SZ];

	cmd_buf[0] = CY_CMD_CAT_EXEC_PANEL_SCAN;

	return cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			cmd_buf, CY_CMD_CAT_EXECUTE_PANEL_SCAN_CMD_SZ,
			return_buf, CY_CMD_CAT_EXECUTE_PANEL_SCAN_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
}

static int cyttsp4_retrieve_panel_scan_(struct device *dev, int read_offset,
	int num_element, u8 data_type, u8 *return_buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	u8 cmd_buf[CY_CMD_CAT_RETRIEVE_PANEL_SCAN_CMD_SZ];

	cmd_buf[0] = CY_CMD_CAT_RETRIEVE_PANEL_SCAN;
	cmd_buf[1] = HI_BYTE(read_offset);
	cmd_buf[2] = LO_BYTE(read_offset);
	cmd_buf[3] = HI_BYTE(num_element);
	cmd_buf[4] = LO_BYTE(num_element);
	cmd_buf[5] = data_type;

	return cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			cmd_buf, CY_CMD_CAT_RETRIEVE_PANEL_SCAN_CMD_SZ,
			return_buf, CY_CMD_CAT_RETRIEVE_PANEL_SCAN_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
}

static int cyttsp4_scan_and_retrieve_(struct device *dev, bool switch_to_cat, bool scan_start, int read_offset,
	int num_element, u8 data_type, u8 *big_buf, int *r_read_element_offset, u8 *r_element_size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	u8 return_buf[CY_CMD_CAT_RETRIEVE_PANEL_SCAN_RET_SZ];

	int rc = 0;
	int rc1 = 0;
	int data_idx = 0;
	u8 cmd_param_ofs = cd->sysinfo.si_ofs.cmd_ofs + 1;
	int read_byte = CY_CMD_CAT_RETRIEVE_PANEL_SCAN_RET_SZ + cmd_param_ofs;
	int left_over_element = num_element;
	int read_element_offset = CY_CMD_IN_DATA_OFFSET_VALUE;
	int returned_element;
	u8 element_start_offset = cmd_param_ofs
		+ CY_CMD_CAT_RETRIEVE_PANEL_SCAN_RET_SZ;
	u8 element_size;

	rc = request_exclusive(cd, (void *)dev, CY_CORE_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0) {
		dev_err(dev, "%s: Error on request exclusive r=%d\n",
				__func__, rc);
		goto err_release;
	}

	if (!switch_to_cat)
		goto do_scan;

	rc = set_mode(cd, CY_MODE_CAT);
	if (rc < 0) {
		dev_err(dev, "%s: fail switch mode to CAT\n",
			__func__);
		goto err_release;
	}

do_scan:
	if (scan_start)	{
		/* Start scan */
		rc = cyttsp4_exec_panel_scan_(dev);
		if (rc < 0) {
			dev_err(dev, "%s: Error on cyttsp4_exec_panel_scan_()\n",
				__func__);
			goto err_release;
		}
	}

	/* retrieve scan data */
	rc = cyttsp4_retrieve_panel_scan_(dev, read_element_offset,
			left_over_element, data_type, return_buf);
	if (rc < 0) {
		dev_err(dev, "%s: Error, offset=%d num_element:%d\n",
			__func__, read_element_offset, left_over_element);
		goto err_release;
	}
	if (return_buf[CY_CMD_OUT_STATUS_OFFSET] != CY_CMD_STATUS_SUCCESS) {
		dev_err(dev, "%s: Fail, offset=%d num_element:%d\n",
			__func__, read_element_offset, left_over_element);
		goto err_release;
	}

	returned_element = return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_H] * 256
		+ return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_L];

	dev_dbg(dev, "%s: num_element:%d\n",
		__func__, returned_element);

	element_size = return_buf[CY_CMD_RET_PNL_OUT_DATA_FORMAT_OFFS] &
			CY_CMD_RET_PANEL_ELMNT_SZ_MASK;

	dev_dbg(dev, "%s: element_size:%d\n",
			__func__, element_size);
	if (r_element_size)
		*r_element_size = element_size;

	/* read data */
	read_byte += returned_element * element_size;

	rc = cyttsp4_read_(dev, CY_MODE_CAT, 0, big_buf, read_byte);
	if (rc < 0) {
		dev_err(dev, "%s: Error on read r=%d\n", __func__, rc);
		goto err_release;
	}

	left_over_element = num_element - returned_element;
	read_element_offset = returned_element;
	data_idx = read_byte;

	while (left_over_element > 0) {
		/* get the data */
		rc = cyttsp4_retrieve_panel_scan_(dev, read_element_offset,
				left_over_element, data_type, return_buf);
		if (rc < 0) {
			dev_err(dev, "%s: Error %d, offset=%d num_element:%d\n",
				__func__, rc, read_element_offset,
				left_over_element);
			goto err_release;
		}
		if (return_buf[CY_CMD_OUT_STATUS_OFFSET]
				!= CY_CMD_STATUS_SUCCESS) {
			dev_err(dev, "%s: Fail, offset=%d num_element:%d\n",
				__func__, read_element_offset,
				left_over_element);
			goto err_release;
		}

		returned_element =
			return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_H] * 256
			+ return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_L];

		dev_dbg(dev, "%s: num_element:%d\n",
			__func__, returned_element);

		/* Check if we requested more elements than the device has */
		if (returned_element == 0) {
			dev_dbg(dev, "%s: returned_element=0, left_over_element=%d\n",
				__func__, left_over_element);
			break;
		}

		/* DO read */
		read_byte = returned_element * element_size;

		rc = cyttsp4_read_(dev, CY_MODE_CAT, element_start_offset,
				big_buf + data_idx, read_byte);
		if (rc < 0) {
			dev_err(dev, "%s: Error on read r=%d\n", __func__, rc);
			goto err_release;
		}

		/* Update element status */
		left_over_element -= returned_element;
		read_element_offset += returned_element;
		data_idx += read_byte;
	}
	if (r_read_element_offset)
		*r_read_element_offset = read_element_offset;

	if (!switch_to_cat)
		goto err_release;

	rc = set_mode(cd, CY_MODE_OPERATIONAL);
	if (rc < 0) {
		dev_err(dev, "%s: fail switch mode to OPMODE\n",
			__func__);
		goto err_release;
	}

err_release:
	rc1 = release_exclusive(cd, (void *)dev);
	if (rc1 < 0) {
		dev_err(dev, "%s: Error on release exclusive r=%d\n",
				__func__, rc1);
	}
	dev_dbg(dev, "%s: big_buf[0~11]:"
		"0x%02x 0x%02x 0x%02x 0x%02x "
		"0x%02x 0x%02x 0x%02x 0x%02x "
		"0x%02x 0x%02x 0x%02x 0x%02x ", __func__,
		big_buf[0], big_buf[1], big_buf[2], big_buf[3],
		big_buf[4], big_buf[5], big_buf[6], big_buf[7],
		big_buf[8], big_buf[9], big_buf[10], big_buf[11]);


	dev_dbg(dev, "%s: rc=%d", __func__, rc);
	return rc;
}

static int exec_cmd_retrieve_data_structure(struct device *dev, int read_offset,
	int num_element, u8 data_id, u8 *return_buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	u8 cmd_buf[CY_CMD_CAT_RETRIEVE_DATA_STRUCT_CMD_SZ];

	cmd_buf[0] = CY_CMD_CAT_RETRIEVE_DATA_STRUCTURE;
	cmd_buf[1] = HI_BYTE(read_offset);
	cmd_buf[2] = LO_BYTE(read_offset);
	cmd_buf[3] = HI_BYTE(num_element);
	cmd_buf[4] = LO_BYTE(num_element);
	cmd_buf[5] = data_id;

	return cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			cmd_buf, CY_CMD_CAT_RETRIEVE_DATA_STRUCT_CMD_SZ,
			return_buf, CY_CMD_CAT_RETRIEVE_DATA_STRUCT_RET_SZ,
			CY_COMMAND_COMPLETE_TIMEOUT);
}

static int cyttsp4_retrieve_data_structure_(struct device *dev, int read_offset,
	int num_element, u8 data_id, u8 *big_buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	u8 return_buf[CY_CMD_CAT_RETRIEVE_DATA_STRUCT_RET_SZ];

	int rc = 0;
	int rc1 = 0;
	int data_idx = 0;
	u8 cmd_param_ofs = cd->sysinfo.si_ofs.cmd_ofs + 1;
	int read_byte = CY_CMD_CAT_RETRIEVE_DATA_STRUCT_RET_SZ + cmd_param_ofs;
	int left_over_element = num_element;
	int read_element_offset = CY_CMD_IN_DATA_OFFSET_VALUE;
	int returned_element;
	u8 element_start_offset = cmd_param_ofs
		+ CY_CMD_CAT_RETRIEVE_DATA_STRUCT_RET_SZ;

	dev_dbg(dev, "%s: ", __func__);

	rc = request_exclusive(cd, (void *)dev, CY_CORE_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0) {
		dev_err(dev, "%s: Error on request exclusive r=%d\n",
				__func__, rc);
		goto err_release;
	}

	rc = set_mode(cd, CY_MODE_CAT);
	if (rc < 0) {
		dev_err(dev, "%s: fail switch mode to CAT\n",
			__func__);
		goto err_release;
	}

	/* retrieve scan data */
	rc = exec_cmd_retrieve_data_structure(dev, read_element_offset,
			left_over_element, data_id, return_buf);
	if (rc < 0) {
		dev_err(dev, "%s: Error, offset=%d num_element:%d\n",
			__func__, read_element_offset, left_over_element);
		goto err_release;
	}
	if (return_buf[CY_CMD_OUT_STATUS_OFFSET] != CY_CMD_STATUS_SUCCESS) {
		dev_err(dev, "%s: Fail, offset=%d num_element:%d\n",
			__func__, read_element_offset, left_over_element);
		goto err_release;
	}

	returned_element = return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_H] * 256
		+ return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_L];

	dev_dbg(dev, "%s: num_element:%d\n",
		__func__, returned_element);


	/* read data */
	read_byte += returned_element;

	rc = cyttsp4_read_(dev, CY_MODE_CAT, 0, big_buf, read_byte);
	if (rc < 0) {
		dev_err(dev, "%s: Error on read r=%d\n", __func__, rc);
		goto err_release;
	}

	left_over_element = num_element - returned_element;
	read_element_offset = returned_element;
	data_idx = read_byte;

	while (left_over_element > 0) {
		/* get the data */
		rc = exec_cmd_retrieve_data_structure(dev, read_element_offset,
				left_over_element, data_id, return_buf);
		if (rc < 0) {
			dev_err(dev, "%s: Error %d, offset=%d num_element:%d\n",
				__func__, rc, read_element_offset,
				left_over_element);
			goto err_release;
		}
		if (return_buf[CY_CMD_OUT_STATUS_OFFSET]
				!= CY_CMD_STATUS_SUCCESS) {
			dev_err(dev, "%s: Fail, offset=%d num_element:%d\n",
				__func__, read_element_offset,
				left_over_element);
			goto err_release;
		}

		returned_element =
			return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_H] * 256
			+ return_buf[CY_CMD_RET_PNL_OUT_ELMNT_SZ_OFFS_L];

		dev_dbg(dev, "%s: num_element:%d\n",
			__func__, returned_element);

		/* Check if we requested more elements than the device has */
		if (returned_element == 0) {
			dev_dbg(dev, "%s: returned_element=0, left_over_element=%d\n",
				__func__, left_over_element);
			break;
		}

		/* DO read */
		read_byte = returned_element;

		rc = cyttsp4_read_(dev, CY_MODE_CAT, element_start_offset,
				big_buf + data_idx, read_byte);
		if (rc < 0) {
			dev_err(dev, "%s: Error on read r=%d\n", __func__, rc);
			goto err_release;
		}

		/* Update element status */
		left_over_element -= returned_element;
		read_element_offset += returned_element;
		data_idx += read_byte;
	}

	rc = set_mode(cd, CY_MODE_OPERATIONAL);
	if (rc < 0) {
		dev_err(dev, "%s: fail switch mode to OPMODE\n",
			__func__);
		goto err_release;
	}

err_release:
	rc1 = release_exclusive(cd, (void *)dev);
	if (rc1 < 0) {
		dev_err(dev, "%s: Error on release exclusive r=%d\n",
				__func__, rc1);
	}
	dev_dbg(dev, "%s: big_buf[0~11]:"
		"0x%02x 0x%02x 0x%02x 0x%02x "
		"0x%02x 0x%02x 0x%02x 0x%02x "
		"0x%02x 0x%02x 0x%02x 0x%02x ", __func__,
		big_buf[0], big_buf[1], big_buf[2], big_buf[3],
		big_buf[4], big_buf[5], big_buf[6], big_buf[7],
		big_buf[8], big_buf[9], big_buf[10], big_buf[11]);

	dev_dbg(dev, "%s: rc=%d", __func__, rc);
	return rc;
}

int cyttsp4_fw_calibrate(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);

	u8 cmd_buf[CY_CMD_CAT_CALIBRATE_IDAC_CMD_SZ];
	u8 return_buf[CY_CMD_CAT_CALIBRATE_IDAC_RET_SZ];
	int rc;

	dev_dbg(dev, "%s: \n", __func__);

	rc = request_exclusive(cd, (void *)dev, CY_CORE_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0) {
		dev_err(dev, "%s: Error on request exclusive r=%d\n",
				__func__, rc);
		goto exit;
	}

	rc = set_mode(cd, CY_MODE_CAT);
	if (rc < 0) {
		dev_err(dev, "%s: Error on request set mode r=%d\n",
				__func__, rc);
		goto exit_release;
	}

	cmd_buf[0] = CY_CMD_CAT_CALIBRATE_IDACS;
	cmd_buf[1] = 0x00; /* Mutual Capacitance Screen */
	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			cmd_buf, CY_CMD_CAT_CALIBRATE_IDAC_CMD_SZ,
			return_buf, CY_CMD_CAT_CALIBRATE_IDAC_RET_SZ,
			CY_CALIBRATE_COMPLETE_TIMEOUT);
	if (rc < 0) {
		dev_err(dev, "%s: Unable to execute calibrate command.\n",
			__func__);
		goto exit_setmode;
	}
	if (return_buf[0] != CY_CMD_STATUS_SUCCESS) {
		dev_err(dev, "%s: calibrate command unsuccessful\n", __func__);
		goto exit_setmode;
	}

	cmd_buf[1] = 0x01; /* Mutual Capacitance Button */
	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			cmd_buf, CY_CMD_CAT_CALIBRATE_IDAC_CMD_SZ,
			return_buf, CY_CMD_CAT_CALIBRATE_IDAC_RET_SZ,
			CY_CALIBRATE_COMPLETE_TIMEOUT);
	if (rc < 0) {
		dev_err(dev, "%s: Unable to execute calibrate command.\n",
			__func__);
		goto exit_setmode;
	}
	if (return_buf[0] != CY_CMD_STATUS_SUCCESS) {
		dev_err(dev, "%s: calibrate command unsuccessful\n", __func__);
		goto exit_setmode;
	}

	cmd_buf[1] = 0x02; /* Self Capacitance */
	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			cmd_buf, CY_CMD_CAT_CALIBRATE_IDAC_CMD_SZ,
			return_buf, CY_CMD_CAT_CALIBRATE_IDAC_RET_SZ,
			CY_CALIBRATE_COMPLETE_TIMEOUT);
	if (rc < 0) {
		dev_err(dev, "%s: Unable to execute calibrate command.\n",
			__func__);
		goto exit_setmode;
	}
	if (return_buf[0] != CY_CMD_STATUS_SUCCESS) {
		dev_err(dev, "%s: calibrate command unsuccessful\n", __func__);
		goto exit_setmode;
	}

	cmd_buf[0] = CY_CMD_CAT_INIT_BASELINES;
	cmd_buf[1] = 0x07; /* SELF, BTN, MUT */
	rc = cyttsp4_exec_cmd(cd, CY_MODE_CAT,
			cmd_buf, CY_CMD_CAT_INIT_BASELINE_CMD_SZ,
			return_buf, CY_CMD_CAT_INIT_BASELINE_RET_SZ,
			500);
	if (rc < 0) {
		dev_err(dev, "%s: Unable to execute init baseline command.\n",
			__func__);
		goto exit_setmode;
	}
	if (return_buf[0] != CY_CMD_STATUS_SUCCESS) {
		dev_err(dev, "%s: init baseline command unsuccessful\n", __func__);
		goto exit_setmode;
	}

exit_setmode:
	rc = set_mode(cd, CY_MODE_OPERATIONAL);
	if (rc < 0)
		dev_err(dev, "%s: Error on request set mode 2 r=%d\n",
				__func__, rc);

exit_release:
	rc = release_exclusive(cd, (void *)dev);
	if (rc < 0)
		dev_err(dev, "%s: Error on release exclusive r=%d\n",
				__func__, rc);

exit:
	dev_dbg(dev, "%s: rc=%d", __func__, rc);
	return rc;
}

static void cyttsp4_watchdog_work(struct work_struct *work)
{
	struct cyttsp4_core_data *cd =
		container_of(work, struct cyttsp4_core_data, watchdog_work);
	u8 mode[2];
	bool restart = false;
	int rc;

	rc = request_exclusive(cd, cd->dev, 1);
	if (rc < 0) {
		dev_vdbg(cd->dev, "%s: fail get exclusive ex=%p own=%p\n",
				__func__, cd->exclusive_dev, cd->dev);
		goto exit;
	}
	rc = cyttsp4_adap_read(cd, CY_REG_BASE, &mode, sizeof(mode));
	if (rc) {
		dev_err(cd->dev, "%s: failed to access device r=%d\n",
			__func__, rc);
		restart = true;
		goto release;
	}

	dev_vdbg(cd->dev, "%s mode[0-1]:0x%X 0x%X\n", __func__,
			mode[0], mode[1]);
	if (IS_BOOTLOADER(mode[0], mode[1])) {
		dev_err(cd->dev, "%s: device found in bootloader mode\n",
			__func__);
		restart = true;
	}
release:
	if (release_exclusive(cd, cd->dev) < 0)
		dev_err(cd->dev, "%s: fail to release exclusive\n", __func__);
	else
		dev_vdbg(cd->dev, "%s: pass release exclusive\n", __func__);
exit:
	if (restart)
		cyttsp4_queue_startup(cd);
	else
		cyttsp4_start_wd_timer(cd);
}

static int cyttsp4_request_stop_wd_(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	cyttsp4_stop_wd_timer(cd);
	return 0;
}

static int _cyttsp4_put_device_into_deep_sleep(struct cyttsp4_core_data *cd,
		u8 hst_mode_reg)
{
	int rc;

	hst_mode_reg |= CY_HST_SLEEP;

	dev_vdbg(cd->dev, "%s: write DEEP SLEEP...\n", __func__);
	rc = cyttsp4_adap_write(cd, CY_REG_BASE, &hst_mode_reg,
			sizeof(hst_mode_reg));
	if (rc) {
		dev_err(cd->dev, "%s: Fail write adapter r=%d\n", __func__, rc);
		return -EINVAL;
	}
	dev_vdbg(cd->dev, "%s: write DEEP SLEEP succeeded\n", __func__);

	if (cd->cpdata->power) {
		dev_dbg(cd->dev, "%s: Power down HW\n", __func__);
		rc = cd->cpdata->power(cd->cpdata, 0, cd->dev, &cd->ignore_irq);
	} else {
		dev_dbg(cd->dev, "%s: No power function\n", __func__);
		rc = 0;
	}
	if (rc < 0) {
		dev_err(cd->dev, "%s: HW Power down fails r=%d\n",
				__func__, rc);
		return -EINVAL;
	}

	return 0;
}

static int _cyttsp4_put_device_into_easy_wakeup(struct cyttsp4_core_data *cd)
{
	u8 command_buf[CY_CMD_OP_WAIT_FOR_EVENT_CMD_SZ];
	int rc;

	if (!IS_TTSP_VER_GE(&cd->sysinfo, 2, 5))
		return -EINVAL;

	command_buf[0] = CY_CMD_OP_WAIT_FOR_EVENT;
	command_buf[1] = cd->easy_wakeup_gesture;

	rc = _cyttsp4_exec_cmd(cd, CY_MODE_OPERATIONAL, command_buf,
			CY_CMD_OP_WAIT_FOR_EVENT_CMD_SZ);
	cd->int_status &= ~CY_INT_EXEC_CMD;
	if (rc)
		dev_err(cd->dev, "%s: Error executing command r=%d\n",
			__func__, rc);

	return rc;
}

static int _cyttsp4_wait_for_refresh_cycle(struct cyttsp4_core_data *cd,
		int cycle)
{
	int active_refresh_cycle_ms;

	if (cd->active_refresh_cycle_ms)
		active_refresh_cycle_ms = cd->active_refresh_cycle_ms;
	else
		active_refresh_cycle_ms = 20;

	msleep(cycle * active_refresh_cycle_ms);

	return 0;
}

static int _cyttsp4_put_device_into_sleep(struct cyttsp4_core_data *cd,
		u8 hst_mode_reg)
{
	int rc;

	if (IS_DEEP_SLEEP_CONFIGURED(cd->easy_wakeup_gesture))
		rc = _cyttsp4_put_device_into_deep_sleep(cd, hst_mode_reg);
	else
		rc = _cyttsp4_put_device_into_easy_wakeup(cd);

	return rc;
}

static int _cyttsp4_core_sleep_device(struct cyttsp4_core_data *cd)
{
	u8 mode[2];
	int rc = 0;

	rc = cyttsp4_adap_read(cd, CY_REG_BASE, &mode, sizeof(mode));
	if (rc) {
		dev_err(cd->dev, "%s: Fail read adapter r=%d\n", __func__, rc);
		goto exit;
	}

	if (IS_BOOTLOADER(mode[0], mode[1])) {
		dev_err(cd->dev, "%s: Device in BOOTLOADER mode.\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	/* Deep sleep is only allowed in Operating mode */
	if (GET_HSTMODE(mode[0]) != CY_HST_OPERATE) {
		dev_err(cd->dev, "%s: Device is not in Operating mode (%02X)\n",
			__func__, GET_HSTMODE(mode[0]));
		mutex_unlock(&cd->system_lock);
		enable_irq(cd->irq);
		/* Try switching to Operating mode */
		rc = set_mode(cd, CY_MODE_OPERATIONAL);
		disable_irq(cd->irq);
		mutex_lock(&cd->system_lock);
		if (rc < 0) {
			dev_err(cd->dev, "%s: failed to set mode to Operational rc=%d\n",
				__func__, rc);
			cyttsp4_queue_startup(cd);
			rc = 0;
			goto exit;
		}

		/* Get the new host mode register value */
		rc = cyttsp4_adap_read(cd, CY_REG_BASE, &mode, sizeof(mode));
		if (rc) {
			dev_err(cd->dev, "%s: Fail read adapter r=%d\n",
				__func__, rc);
			goto exit;
		}
	}

	rc = _cyttsp4_put_device_into_sleep(cd, mode[0]);

exit:
	return rc;
}

static int _cyttsp4_core_poweroff_device(struct cyttsp4_core_data *cd)
{
	int rc;

	/* No need for cd->pdata->power check since we did it in probe */
	rc = cd->cpdata->power(cd->cpdata, 0, cd->dev, 0);
	if (rc < 0)
		dev_err(cd->dev, "%s: HW Power down fails r=%d\n",
				__func__, rc);
	return rc;
}

static int cyttsp4_core_sleep_(struct cyttsp4_core_data *cd)
{
	int rc = 0;

	mutex_lock(&cd->system_lock);
	if (cd->sleep_state == SS_SLEEP_OFF) {
		cd->sleep_state = SS_SLEEPING;
	} else {
		mutex_unlock(&cd->system_lock);
		return 1;
	}
	mutex_unlock(&cd->system_lock);

	cyttsp4_stop_wd_timer(cd);

	if (cd->cpdata->flags & CY_CORE_FLAG_POWEROFF_ON_SLEEP)
		rc = _cyttsp4_core_poweroff_device(cd);
	else
		rc = _cyttsp4_core_sleep_device(cd);

	mutex_lock(&cd->system_lock);
	cd->sleep_state = SS_SLEEP_ON;
	mutex_unlock(&cd->system_lock);

	return rc;
}

static int cyttsp4_core_sleep(struct cyttsp4_core_data *cd,
	bool _disable_irq)
{
	int rc;

	rc = request_exclusive(cd, cd->dev,
			CY_CORE_SLEEP_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail get exclusive ex=%p own=%p\n",
				__func__, cd->exclusive_dev, cd->dev);
		return 0;
	}

	if (cd->cpdata->flags & CY_CORE_FLAG_POWEROFF_ON_SLEEP) {
		if (_disable_irq && cd->irq_enabled) {
			cd->irq_enabled = false;
			disable_irq_nosync(cd->irq);
			dev_dbg(cd->dev, "%s: irq disabled\n", __func__);
		}
	}

	rc = cyttsp4_core_sleep_(cd);

	if (release_exclusive(cd, cd->dev) < 0)
		dev_err(cd->dev, "%s: fail to release exclusive\n", __func__);
	else
		dev_vdbg(cd->dev, "%s: pass release exclusive\n", __func__);

	/* Give time to FW to sleep */
	_cyttsp4_wait_for_refresh_cycle(cd, 2);

	return rc;
}

static int _cyttsp4_awake_device_from_deep_sleep(struct cyttsp4_core_data *cd,
		int timeout_ms)
{
	struct device *dev = cd->dev;
	u8 mode;
	int t;
	int rc;

	cd->int_status |= CY_INT_AWAKE;

	if (cd->cpdata->power) {
		/* Wake up using platform power function */
		dev_dbg(dev, "%s: Power up HW\n", __func__);
		rc = cd->cpdata->power(cd->cpdata, 1, dev, &cd->ignore_irq);
	} else {
		/* Initiate a read transaction to wake up */
		rc = cyttsp4_adap_read(cd, CY_REG_BASE, &mode, sizeof(mode));
	}
	if (rc < 0) {
		dev_err(dev, "%s: HW Power up fails r=%d\n", __func__, rc);
		/* Initiate another read transaction to wake up */
		rc = cyttsp4_adap_read(cd, CY_REG_BASE, &mode, sizeof(mode));
	} else
		dev_vdbg(cd->dev, "%s: HW power up succeeds\n", __func__);
	mutex_unlock(&cd->system_lock);

	t = wait_event_timeout(cd->wait_q,
			(cd->int_status & CY_INT_AWAKE) == 0,
			msecs_to_jiffies(timeout_ms));
	mutex_lock(&cd->system_lock);
	if (IS_TMO(t)) {
		dev_dbg(dev, "%s: TMO waiting for wakeup\n", __func__);
		cd->int_status &= ~CY_INT_AWAKE;
		/* Perform a read transaction to check if device is awake */
		rc = cyttsp4_adap_read(cd, CY_REG_BASE, &mode, sizeof(mode));
		if (rc < 0 || GET_HSTMODE(mode) != CY_HST_OPERATE) {
			dev_err(dev, "%s: Queueing startup\n", __func__);
			/* Try starting up */
			cyttsp4_queue_startup(cd);
		}
	}

	return rc;
}

static int _cyttsp4_awake_device(struct cyttsp4_core_data *cd)
{
	int timeout_ms;

	if (cd->wake_initiated_by_device) {
		cd->wake_initiated_by_device = 0;
		/* To prevent sequential wake/sleep caused by ttsp modules */
		msleep(20);
		return 0;
	}

	if (IS_DEEP_SLEEP_CONFIGURED(cd->easy_wakeup_gesture))
		timeout_ms = CY_CORE_WAKEUP_TIMEOUT;
	else
		timeout_ms = CY_CORE_WAKEUP_TIMEOUT * 4;

	return _cyttsp4_awake_device_from_deep_sleep(cd, timeout_ms);
}

static int _cyttsp4_ldr_exit(struct cyttsp4_core_data *cd)
{
	if (cd->cpdata->flags & CY_CORE_FLAG_POWEROFF_ON_SLEEP &&
		cd->bl_fast_exit) {
		dev_dbg(cd->dev, "%s: fast bootloader exit\n", __func__);
		return cyttsp4_adap_write(cd, CY_REG_BASE, (u8 *)ldr_fast_exit,
				sizeof(ldr_fast_exit));
	}

	return cyttsp4_adap_write(cd, CY_REG_BASE, (u8 *)ldr_exit,
			sizeof(ldr_exit));
}

static int _cyttsp4_core_poweron_device(struct cyttsp4_core_data *cd)
{
	struct device *dev = cd->dev;
	int rc;

	cd->mode = CY_MODE_UNKNOWN;

	/* No need for cd->pdata->power check since we did it in probe */
	rc = cd->cpdata->power(cd->cpdata, 1, dev, 0);
	if (rc < 0) {
		dev_err(dev, "%s: HW Power up fails r=%d\n", __func__, rc);
		goto exit;
	}

	mutex_unlock(&cd->system_lock);
	rc = cyttsp4_wait_bl_heartbeat(cd);
	mutex_lock(&cd->system_lock);
	if (rc) {
		dev_err(dev, "%s: Error on waiting bl heartbeat r=%d\n",
			__func__, rc);
		goto exit;
	}

	/* exit bl into sysinfo mode */
	dev_vdbg(dev, "%s: write exit ldr...\n", __func__);
	cd->int_status &= ~CY_INT_IGNORE;
	cd->int_status |= CY_INT_MODE_CHANGE;

	rc = _cyttsp4_ldr_exit(cd);
	if (rc < 0) {
		dev_err(dev, "%s: Fail to write rc=%d\n", __func__, rc);
		goto exit;
	}

	mutex_unlock(&cd->system_lock);
	rc = cyttsp4_wait_sysinfo_mode(cd);
	if (rc) {
		dev_err(dev, "%s: Fail switch to sysinfo mode, r=%d\n",
			__func__, rc);
		goto exit_lock;
	}

	rc = set_mode(cd, CY_MODE_OPERATIONAL);
	if (rc) {
		dev_err(dev, "%s: Fail set mode to Operational mode, r=%d\n",
			__func__, rc);
		goto exit_lock;
	}

exit_lock:
	mutex_lock(&cd->system_lock);
exit:
	return rc;
}

static int cyttsp4_core_wake_(struct cyttsp4_core_data *cd)
{
	int rc;

	/* Already woken? */
	mutex_lock(&cd->system_lock);
	if (cd->sleep_state == SS_SLEEP_ON) {
		cd->sleep_state = SS_WAKING;
	} else {
		mutex_unlock(&cd->system_lock);
		return 1;
	}

	cd->int_status &= ~CY_INT_IGNORE;
	cd->sleep_state = SS_WAKING;

	if (cd->cpdata->flags & CY_CORE_FLAG_POWEROFF_ON_SLEEP)
		rc = _cyttsp4_core_poweron_device(cd);
	else
		rc = _cyttsp4_awake_device(cd);

	if (rc)
		cyttsp4_queue_startup(cd);

	cd->sleep_state = SS_SLEEP_OFF;
	mutex_unlock(&cd->system_lock);

	cyttsp4_start_wd_timer(cd);

	return 0;
}

static int cyttsp4_core_wake(struct cyttsp4_core_data *cd,
	bool _enable_irq)
{
	int rc;

	rc = request_exclusive(cd, cd->dev,
			CY_CORE_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail get exclusive ex=%p own=%p\n",
				__func__, cd->exclusive_dev, cd->dev);
		return 0;
	}

	if (cd->cpdata->flags & CY_CORE_FLAG_POWEROFF_ON_SLEEP) {
		if (_enable_irq && !cd->irq_enabled) {
			cd->irq_enabled = true;
			enable_irq(cd->irq);
			dev_dbg(cd->dev, "%s: irq enabled\n", __func__);
		}
	}
	rc = cyttsp4_core_wake_(cd);

	if (release_exclusive(cd, cd->dev) < 0)
		dev_err(cd->dev, "%s: fail to release exclusive\n", __func__);
	else
		dev_vdbg(cd->dev, "%s: pass release exclusive\n", __func__);

	/* If a startup queued in wake, wait it to finish */
	wait_event_timeout(cd->wait_q, cd->startup_state == STARTUP_NONE,
			msecs_to_jiffies(CY_CORE_RESET_AND_WAIT_TIMEOUT));

	return rc;
}

static int cyttsp4_get_ttconfig_info(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;
	u16 length, max_length;
	u16 version = 0;
	u16 crc = 0;
	int rc;

	dev_dbg(cd->dev, "%s: \n", __func__);

	rc = set_mode(cd, CY_MODE_CAT);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to set mode to CAT rc=%d\n",
			__func__, rc);
		return rc;
	}

	rc = cyttsp4_get_ttconfig_version(cd, &version);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to get ttconfig version rc=%d\n",
			__func__, rc);
		return rc;
	}

	rc = cyttsp4_get_config_length(cd, CY_TCH_PARM_EBID,
			&length, &max_length);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to get ttconfig length rc=%d\n",
			__func__, rc);
		return rc;
	}

	rc = set_mode(cd, CY_MODE_OPERATIONAL);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to set mode to Operational rc=%d\n",
			__func__, rc);
		return rc;
	}

	rc = cyttsp4_get_config_block_crc(cd, CY_TCH_PARM_EBID, &crc);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to get ttconfig crc rc=%d\n",
			__func__, rc);
		return rc;
	}

	si->ttconfig.version = version;
	si->ttconfig.length = length;
	si->ttconfig.max_length = max_length;
	si->ttconfig.crc = crc;

	dev_vdbg(cd->dev, "%s: TT Config Version:%04X Length:%d Max Length:%d CRC:%04X\n",
		__func__, si->ttconfig.version, si->ttconfig.length,
		si->ttconfig.length, si->ttconfig.crc);

	return 0;
}

static int cyttsp4_get_active_refresh_cycle(struct cyttsp4_core_data *cd)
{
	int rc;
	u32 value;

	dev_dbg(cd->dev, "%s: \n", __func__);

	rc = cyttsp4_get_parameter(cd, CY_RAM_ID_REFRESH_INTERVAL, &value);
	if (!rc)
		cd->active_refresh_cycle_ms = (u8)value;

	return rc;
}

static int cyttsp4_set_initial_scantype(struct cyttsp4_core_data *cd)
{
	u8 new_scantype;
	int rc;

	rc = cyttsp4_get_scantype(cd, &cd->default_scantype);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to get scantype rc=%d\n",
			__func__, rc);
		goto exit;
	}

	/* Disable proximity sensing by default */
	cd->default_scantype &= ~CY_SCAN_TYPE_PROXIMITY;

	new_scantype = _cyttsp4_generate_new_scantype(cd);

	rc = cyttsp4_set_scantype(cd, new_scantype);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to set scantype rc=%d\n",
			__func__, rc);
		goto exit;
	}
exit:
	return rc;
}

static int cyttsp4_startup_(struct cyttsp4_core_data *cd)
{
	int retry = CY_CORE_STARTUP_RETRY_COUNT;
	int rc;
	bool detected = false;

	dev_dbg(cd->dev, "%s: enter...\n", __func__);

	cyttsp4_stop_wd_timer(cd);

reset:
	if (retry != CY_CORE_STARTUP_RETRY_COUNT)
		dev_dbg(cd->dev, "%s: Retry %d\n", __func__,
			CY_CORE_STARTUP_RETRY_COUNT - retry);

	/* reset hardware and wait for heartbeat */
	rc = cyttsp4_reset_and_wait(cd);
	if (rc < 0) {
		dev_err(cd->dev, "%s: Error on h/w reset r=%d\n", __func__, rc);
		RETRY_OR_EXIT(retry--, reset, exit);
	}

	detected = true;

	/* exit bl into sysinfo mode */
	dev_vdbg(cd->dev, "%s: write exit ldr...\n", __func__);
	mutex_lock(&cd->system_lock);
	cd->int_status &= ~CY_INT_IGNORE;
	cd->int_status |= CY_INT_MODE_CHANGE;

	rc = _cyttsp4_ldr_exit(cd);
	mutex_unlock(&cd->system_lock);
	if (rc < 0) {
		dev_err(cd->dev, "%s: Fail to write rc=%d\n", __func__, rc);
		RETRY_OR_EXIT(retry--, reset, exit);
	}

	rc = cyttsp4_wait_sysinfo_mode(cd);
	if (rc < 0) {
		u8 buf[sizeof(ldr_err_app)];
		int rc1;

		/* Check for invalid/corrupted touch application */
		rc1 = cyttsp4_adap_read(cd, CY_REG_BASE, buf,
				sizeof(ldr_err_app));
		if (rc1) {
			dev_err(cd->dev, "%s: Fail to read rc=%d\n",
				__func__, rc1);
		} else if (!memcmp(buf, ldr_err_app, sizeof(ldr_err_app))) {
			dev_err(cd->dev, "%s: Error launching touch application\n",
				__func__);
			mutex_lock(&cd->system_lock);
			cd->invalid_touch_app = true;
			mutex_unlock(&cd->system_lock);
			goto exit_no_wd;
		}

		RETRY_OR_EXIT(retry--, reset, exit);
	}

	mutex_lock(&cd->system_lock);
	cd->invalid_touch_app = false;
	mutex_unlock(&cd->system_lock);

	/* read sysinfo data */
	dev_vdbg(cd->dev, "%s: get sysinfo regs..\n", __func__);
	rc = cyttsp4_get_sysinfo_regs(cd);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to get sysinfo regs rc=%d\n",
			__func__, rc);
		RETRY_OR_EXIT(retry--, reset, exit);
	}

	rc = set_mode(cd, CY_MODE_OPERATIONAL);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to set mode to operational rc=%d\n",
			__func__, rc);
		RETRY_OR_EXIT(retry--, reset, exit);
	}

	if (cd->cpdata->flags & CY_CORE_FLAG_SCAN_MODE_USES_RAM_ID_SCAN_TYPE)
		rc = cyttsp4_set_initial_scantype(cd);
	else
		rc = cyttsp4_set_proximity(cd, false);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to set scantype rc=%d\n",
			__func__, rc);
		RETRY_OR_EXIT(retry--, reset, exit);
	}

	rc = cyttsp4_get_ttconfig_info(cd);
	if (rc < 0) {
		dev_err(cd->dev, "%s: failed to get ttconfig info rc=%d\n",
			__func__, rc);
		RETRY_OR_EXIT(retry--, reset, exit);
	}

	rc = cyttsp4_get_active_refresh_cycle(cd);
	if (rc < 0)
		dev_err(cd->dev, "%s: failed to get refresh cycle time rc=%d\n",
			__func__, rc);

	/* attention startup */
	call_atten_cb(cd, CY_ATTEN_STARTUP, 0);

	/* restore to sleep if was suspended */
	mutex_lock(&cd->system_lock);
	cd->bl_fast_exit = true;
	if (cd->sleep_state == SS_SLEEP_ON) {
		cd->sleep_state = SS_SLEEP_OFF;
		mutex_unlock(&cd->system_lock);
		/* watchdog is restarted by cyttsp4_core_sleep_() on error */
		cyttsp4_core_sleep_(cd);
		goto exit_no_wd;
	}
	mutex_unlock(&cd->system_lock);

exit:
	cyttsp4_start_wd_timer(cd);

exit_no_wd:
	if (!detected)
		rc = -ENODEV;

	/* Required for signal to the TTHE */
	dev_info(cd->dev, "%s: cyttsp4_exit startup r=%d...\n", __func__, rc);

	return rc;
}

static int cyttsp4_startup(struct cyttsp4_core_data *cd)
{
	int rc;

	dev_dbg(cd->dev, "%s\n",__func__);

	mutex_lock(&cd->system_lock);
	cd->startup_state = STARTUP_RUNNING;
	mutex_unlock(&cd->system_lock);

	rc = request_exclusive(cd, cd->dev,
			CY_CORE_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0) {
		dev_err(cd->dev, "%s: fail get exclusive ex=%p own=%p\n",
				__func__, cd->exclusive_dev, cd->dev);
		goto exit;
	}

	rc = cyttsp4_startup_(cd);

	if (release_exclusive(cd, cd->dev) < 0)
		/* Don't return fail code, mode is already changed. */
		dev_err(cd->dev, "%s: fail to release exclusive\n", __func__);
	else
		dev_vdbg(cd->dev, "%s: pass release exclusive\n", __func__);

exit:
	mutex_lock(&cd->system_lock);
	cd->startup_state = STARTUP_NONE;
	mutex_unlock(&cd->system_lock);

	/* Wake the waiters for end of startup */
	wake_up(&cd->wait_q);

	dev_dbg(cd->dev, "%s done\n",__func__);
	return rc;
}

static void cyttsp4_startup_work_function(struct work_struct *work)
{
	struct cyttsp4_core_data *cd =  container_of(work,
		struct cyttsp4_core_data, startup_work);
	int rc;

	dev_dbg(cd->dev, "%s: start\n", __func__);
	/*
	 * Force clear exclusive access
	 * startup queue is called for abnormal case,
	 * and when a this called access can be acquired in other context
	 */
	/*
	mutex_lock(&cd->system_lock);
	if (cd->exclusive_dev != cd->dev)
		cd->exclusive_dev = NULL;
	mutex_unlock(&cd->system_lock);
	*/
	rc = cyttsp4_startup(cd);
	if (rc < 0)
		dev_err(cd->dev, "%s: Fail queued startup r=%d\n",
			__func__, rc);
}

static void cyttsp4_free_si_ptrs(struct cyttsp4_core_data *cd)
{
	struct cyttsp4_sysinfo *si = &cd->sysinfo;

	if (!si)
		return;

	kfree(si->si_ptrs.cydata);
	kfree(si->si_ptrs.test);
	kfree(si->si_ptrs.pcfg);
	kfree(si->si_ptrs.opcfg);
	kfree(si->si_ptrs.ddata);
	kfree(si->si_ptrs.mdata);
	kfree(si->btn);
	kfree(si->xy_mode);
	kfree(si->btn_rec_data);
}

int cyttsp4_core_stop(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);

	if (cd->touch_stopped) {
		dev_err(dev, "%s: already off\n", __func__);
		return 0;
	}

	cd->touch_stopped = true;
	cyttsp4_core_sleep(cd, 1);

	if (cd->irq_enabled) {
		disable_irq_nosync(cd->irq);
		cd->irq_enabled = false;
	}

	return 0;
}

int cyttsp4_core_start(struct device *dev)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);

	if (!cd->touch_stopped) {
		dev_err(dev, "%s: already on\n", __func__);
		return 0;
	}

	if (!cd->irq_enabled) {
		enable_irq(cd->irq);
		cd->irq_enabled = true;
	}

	cyttsp4_core_wake(cd, 1);
	cd->touch_stopped = false;

	return 0;
}

/*
 * Show Firmware version via sysfs
 */
static ssize_t cyttsp4_ic_ver_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	struct cyttsp4_cydata *cydata;

	mutex_lock(&cd->system_lock);
	if (!cd->sysinfo.ready) {
		if (cd->invalid_touch_app) {
			mutex_unlock(&cd->system_lock);
			return snprintf(buf, CY_MAX_PRBUF_SIZE,
					"Corrupted Touch application!\n");
		} else {
			mutex_unlock(&cd->system_lock);
			return snprintf(buf, CY_MAX_PRBUF_SIZE,
					"System Information not ready!\n");
		}
	}
	mutex_unlock(&cd->system_lock);

	cydata = cd->sysinfo.si_ptrs.cydata;

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"%s: 0x%02X 0x%02X\n"
		"%s: 0x%02X\n"
		"%s: 0x%02X\n"
		"%s: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n"
		"%s: 0x%04X\n"
		"%s: 0x%02X\n"
		"%s: 0x%02X\n",
		"TrueTouch Product ID", cydata->ttpidh, cydata->ttpidl,
		"Firmware Major Version", cydata->fw_ver_major,
		"Firmware Minor Version", cydata->fw_ver_minor,
		"Revision Control Number", cydata->revctrl[0],
		cydata->revctrl[1], cydata->revctrl[2], cydata->revctrl[3],
		cydata->revctrl[4], cydata->revctrl[5], cydata->revctrl[6],
		cydata->revctrl[7],
		"TrueTouch Config Version", cd->sysinfo.ttconfig.version,
		"Bootloader Major Version", cydata->blver_major,
		"Bootloader Minor Version", cydata->blver_minor);
}

/*
 * Show TT Config version via sysfs
 */
static ssize_t cyttsp4_ttconfig_ver_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE, "0x%04X\n",
			cd->sysinfo.ttconfig.version);
}

/*
 * Show Driver version via sysfs
 */
static ssize_t cyttsp4_drv_ver_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Driver: %s\nVersion: %s\nDate: %s\n",
		CYTTSP4_CORE_NAME, CY_DRIVER_VERSION,
		CY_DRIVER_DATE);
}

/*
 * HW reset via sysfs
 */
static ssize_t cyttsp4_hw_reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int rc = 0;

	rc = cyttsp4_startup(cd);
	if (rc < 0)
		dev_err(dev, "%s: HW reset failed r=%d\n",
			__func__, rc);

	return size;
}

/*
 * Show IRQ status via sysfs
 */
static ssize_t cyttsp4_hw_irq_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	int retval;

	if (cd->cpdata->irq_stat) {
		retval = cd->cpdata->irq_stat(cd->cpdata, dev);
		switch (retval) {
		case 0:
			return snprintf(buf, CY_MAX_PRBUF_SIZE,
				"Interrupt line is LOW.\n");
		case 1:
			return snprintf(buf, CY_MAX_PRBUF_SIZE,
				"Interrupt line is HIGH.\n");
		default:
			return snprintf(buf, CY_MAX_PRBUF_SIZE,
				"Function irq_stat() returned %d.\n", retval);
		}
	}

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Function irq_stat() undefined.\n");
}

/*
 * Show IRQ enable/disable status via sysfs
 */
static ssize_t cyttsp4_drv_irq_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&cd->system_lock);
	if (cd->irq_enabled)
		ret = snprintf(buf, CY_MAX_PRBUF_SIZE,
			"Driver interrupt is ENABLED\n");
	else
		ret = snprintf(buf, CY_MAX_PRBUF_SIZE,
			"Driver interrupt is DISABLED\n");
	mutex_unlock(&cd->system_lock);

	return ret;
}

/*
 * Enable/disable IRQ via sysfs
 */
static ssize_t cyttsp4_drv_irq_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	unsigned long value;
	int retval;

	retval = kstrtoul(buf, 10, &value);
	if (retval < 0) {
		dev_err(dev, "%s: Invalid value\n", __func__);
		goto cyttsp4_drv_irq_store_error_exit;
	}

	mutex_lock(&cd->system_lock);
	switch (value) {
	case 0:
		if (cd->irq_enabled) {
			cd->irq_enabled = false;
			/* Disable IRQ */
			disable_irq_nosync(cd->irq);
			dev_info(dev, "%s: Driver IRQ now disabled\n",
				__func__);
		} else
			dev_info(dev, "%s: Driver IRQ already disabled\n",
				__func__);
		break;

	case 1:
		if (cd->irq_enabled == false) {
			cd->irq_enabled = true;
			/* Enable IRQ */
			enable_irq(cd->irq);
			dev_info(dev, "%s: Driver IRQ now enabled\n",
				__func__);
		} else
			dev_info(dev, "%s: Driver IRQ already enabled\n",
				__func__);
		break;

	default:
		dev_err(dev, "%s: Invalid value\n", __func__);
	}
	mutex_unlock(&(cd->system_lock));

cyttsp4_drv_irq_store_error_exit:

	return size;
}

/*
 * Debugging options via sysfs
 */
static ssize_t cyttsp4_drv_debug_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	unsigned long value = 0;
	int rc = 0;

	rc = kstrtoul(buf, 10, &value);
	if (rc < 0) {
		dev_err(dev, "%s: Invalid value\n", __func__);
		goto cyttsp4_drv_debug_store_exit;
	}

	switch (value) {
	case CY_DBG_SUSPEND:
		dev_info(dev, "%s: SUSPEND (cd=%p)\n", __func__, cd);
		rc = cyttsp4_core_sleep(cd, 0);
		if (rc)
			dev_err(dev, "%s: Suspend failed rc=%d\n",
				__func__, rc);
		else
			dev_info(dev, "%s: Suspend succeeded\n", __func__);
		break;

	case CY_DBG_RESUME:
		dev_info(dev, "%s: RESUME (cd=%p)\n", __func__, cd);
		rc = cyttsp4_core_wake(cd, 0);
		if (rc)
			dev_err(dev, "%s: Resume failed rc=%d\n",
				__func__, rc);
		else
			dev_info(dev, "%s: Resume succeeded\n", __func__);
		break;
	case CY_DBG_SOFT_RESET:
		dev_info(dev, "%s: SOFT RESET (cd=%p)\n", __func__, cd);
		rc = cyttsp4_hw_soft_reset(cd);
		break;
	case CY_DBG_RESET:
		dev_info(dev, "%s: HARD RESET (cd=%p)\n", __func__, cd);
		rc = cyttsp4_hw_hard_reset(cd);
		break;
	default:
		dev_err(dev, "%s: Invalid value\n", __func__);
	}

cyttsp4_drv_debug_store_exit:
	return size;
}

/*
 * Show system status on deep sleep status via sysfs
 */
static ssize_t cyttsp4_sleep_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&cd->system_lock);
	if (cd->sleep_state == SS_SLEEP_ON)
		ret = snprintf(buf, CY_MAX_PRBUF_SIZE,
				"Deep Sleep is ENABLED\n");
	else
		ret = snprintf(buf, CY_MAX_PRBUF_SIZE,
				"Deep Sleep is DISABLED\n");
	mutex_unlock(&cd->system_lock);

	return ret;
}

static ssize_t cyttsp4_easy_wakeup_gesture_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&cd->system_lock);
	ret = snprintf(buf, CY_MAX_PRBUF_SIZE, "0x%02X\n",
			cd->easy_wakeup_gesture);
	mutex_unlock(&cd->system_lock);
	return ret;
}

static ssize_t cyttsp4_easy_wakeup_gesture_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);
	unsigned long value;
	int ret;

	ret = kstrtoul(buf, 10, &value);
	if (ret < 0)
		return ret;

	if (value > 0xFF)
		return -EINVAL;

	pm_runtime_get_sync(dev);

	mutex_lock(&cd->system_lock);
	if (cd->sysinfo.ready && IS_TTSP_VER_GE(&cd->sysinfo, 2, 5))
		cd->easy_wakeup_gesture = (u8)value;
	else
		ret = -ENODEV;
	mutex_unlock(&cd->system_lock);

	pm_runtime_put(dev);

	if (ret)
		return ret;

	return size;
}

static struct device_attribute attributes[] = {
	__ATTR(ic_ver, S_IRUGO, cyttsp4_ic_ver_show, NULL),
	__ATTR(ttconfig_ver, S_IRUGO, cyttsp4_ttconfig_ver_show, NULL),
	__ATTR(drv_ver, S_IRUGO, cyttsp4_drv_ver_show, NULL),
	__ATTR(hw_reset, S_IWUSR, NULL, cyttsp4_hw_reset_store),
	__ATTR(hw_irq_stat, S_IRUSR, cyttsp4_hw_irq_stat_show, NULL),
	__ATTR(drv_irq, S_IRUSR | S_IWUSR, cyttsp4_drv_irq_show,
		cyttsp4_drv_irq_store),
	__ATTR(drv_debug, S_IWUSR, NULL, cyttsp4_drv_debug_store),
	__ATTR(sleep_status, S_IRUSR, cyttsp4_sleep_status_show, NULL),
	__ATTR(easy_wakeup_gesture, S_IRUSR | S_IWUSR,
		cyttsp4_easy_wakeup_gesture_show,
		cyttsp4_easy_wakeup_gesture_store),
};

static int add_sysfs_interfaces(struct cyttsp4_core_data *cd,
		struct device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(attributes); i++)
		if (device_create_file(dev, attributes + i))
			goto undo;

	return 0;
undo:
	for (i--; i >= 0 ; i--)
		device_remove_file(dev, attributes + i);
	dev_err(dev, "%s: failed to create sysfs interface\n", __func__);
	return -ENODEV;
}

static void remove_sysfs_interfaces(struct cyttsp4_core_data *cd,
		struct device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(attributes); i++)
		device_remove_file(dev, attributes + i);
}

static struct cyttsp4_core_commands _cyttsp4_core_commands = {
	.subscribe_attention = _cyttsp4_subscribe_attention,
	.unsubscribe_attention = _cyttsp4_unsubscribe_attention,
	.request_exclusive = cyttsp4_request_exclusive_,
	.release_exclusive = cyttsp4_release_exclusive_,
	.request_reset = cyttsp4_request_reset_,
	.request_restart = cyttsp4_request_restart_,
	.request_set_mode = cyttsp4_request_set_mode_,
	.request_sysinfo = cyttsp4_request_sysinfo_,
	.request_loader_pdata = cyttsp4_request_loader_pdata_,
	.request_handshake = cyttsp4_request_handshake_,
	.request_exec_cmd = cyttsp4_request_exec_cmd_,
	.request_stop_wd = cyttsp4_request_stop_wd_,
	.request_toggle_lowpower = cyttsp4_request_toggle_lowpower_,
	.request_config_row_size = cyttsp4_request_config_row_size_,
	.request_write_config = cyttsp4_request_write_config_,
	.request_enable_scan_type = cyttsp4_request_enable_scan_type_,
	.request_disable_scan_type = cyttsp4_request_disable_scan_type_,
	.get_security_key = cyttsp4_get_security_key_,
	.get_touch_record = cyttsp4_get_touch_record_,
	.write = cyttsp4_write_,
	.read = cyttsp4_read_,

	.update_sysinfo = cyttsp4_update_sysinfo_,
	.exec_panel_scan = cyttsp4_exec_panel_scan_,
 	.retrieve_panel_scan = cyttsp4_retrieve_panel_scan_,
 	.scan_and_retrieve = cyttsp4_scan_and_retrieve_,
 	.retrieve_data_structure = cyttsp4_retrieve_data_structure_,
};

struct cyttsp4_core_commands *cyttsp4_get_commands(void)
{
	return &_cyttsp4_core_commands;
}
EXPORT_SYMBOL_GPL(cyttsp4_get_commands);

static LIST_HEAD(core_list);
static int core_number;
struct cyttsp4_core_data *cyttsp4_get_core_data(char *id)
{
	struct cyttsp4_core_data *d;

	list_for_each_entry(d, &core_list, node)
		if (!strncmp(d->core_id, id, 20))
			return d;
	return NULL;
}
EXPORT_SYMBOL_GPL(cyttsp4_get_core_data);

static void cyttsp4_add_core(struct device *dev)
{
	struct cyttsp4_core_data *d;
	struct cyttsp4_core_data *cd = dev_get_drvdata(dev);

	list_for_each_entry(d, &core_list, node)
		if (d->dev == dev)
			return;

	list_add(&cd->node, &core_list);
}

static void cyttsp4_del_core(struct device *dev)
{
	struct cyttsp4_core_data *d, *d_n;

	list_for_each_entry_safe(d, d_n, &core_list, node) {
		if (d->dev == dev) {
			list_del(&d->node);
			return;
		}
	}

	return;
}

int cyttsp4_probe(const struct cyttsp4_bus_ops *ops, struct device *dev,
		u16 irq, size_t xfer_buf_size)
{
	struct cyttsp4_core_data *cd;
	struct cyttsp4_platform_data *pdata = dev_get_platdata(dev);

	enum cyttsp4_atten_type type;
	unsigned long irq_flags;
	int rc = 0;

	if (!pdata || !pdata->core_pdata || !pdata->mt_pdata) {
		dev_err(dev, "%s: Missing platform data\n", __func__);
		rc = -ENODEV;
		goto error_no_pdata;
	}

	/* get context and debug print buffers */
	cd = kzalloc(sizeof(*cd), GFP_KERNEL);
	if (!cd) {
		dev_err(dev, "%s: Error, kzalloc\n", __func__);
		rc = -ENOMEM;
		goto error_free_cd;
	}

	/* Initialize device info */
	cd->dev = dev;
	cd->pdata = pdata;
	cd->cpdata = pdata->core_pdata;
	cd->bus_ops = ops;
	cd->max_xfer = CY_DEFAULT_ADAP_MAX_XFER;
	if (cd->cpdata->max_xfer_len) {
		if (cd->cpdata->max_xfer_len < CY_ADAP_MIN_XFER) {
			dev_err(dev, "%s: max_xfer_len invalid (min=%d)\n",
				__func__, CY_ADAP_MIN_XFER);
			rc = -EINVAL;
			goto error_max_xfer;
		}
		cd->max_xfer = cd->cpdata->max_xfer_len;
		dev_dbg(dev, "%s: max_xfer set to %d\n",
			__func__, cd->max_xfer);
	}
	scnprintf(cd->core_id, 20, "%s%d", CYTTSP4_CORE_NAME, core_number++);

	/* Check POWEROFF_ON_SLEEP flag and power function */
	if ((cd->cpdata->flags & CY_CORE_FLAG_POWEROFF_ON_SLEEP)
			&& (cd->cpdata->power == NULL)) {
		dev_err(dev, "%s: No power function with POWEROFF_ON_SLEEP flag\n",
			__func__);
		rc = -EINVAL;
		goto error_power;
	}

	/* Initialize mutexes and spinlocks */
	mutex_init(&cd->system_lock);
	mutex_init(&cd->adap_lock);
	spin_lock_init(&cd->spinlock);

	/* Initialize attention lists */
	for (type = 0; type < CY_ATTEN_NUM_ATTEN; type++)
		INIT_LIST_HEAD(&cd->atten_list[type]);

	/* Initialize wait queue */
	init_waitqueue_head(&cd->wait_q);

	/* Initialize works */
	INIT_WORK(&cd->startup_work, cyttsp4_startup_work_function);
	INIT_WORK(&cd->watchdog_work, cyttsp4_watchdog_work);

	/* Initialize IRQ */
	cd->irq = gpio_to_irq(cd->cpdata->irq_gpio);
	if (cd->irq < 0) {
		rc = -EINVAL;
		goto error_free_cd;
	}
	cd->irq_enabled = true;

	dev_set_drvdata(dev, cd);
	cyttsp4_add_core(dev);

	/* Get pinctrl if target uses pinctrl */
	cd->cpdata->ts_pinctrl = devm_pinctrl_get(cd->dev);
	if (IS_ERR_OR_NULL(cd->cpdata->ts_pinctrl)) {
		dev_err(cd->dev, "%s: Target does not use pinctrl\n", __func__);
		rc = PTR_ERR(cd->cpdata->ts_pinctrl);
		cd->cpdata->ts_pinctrl = NULL;
		return rc;
	}

	/* Call platform init function */
	if (cd->cpdata->init) {
		dev_dbg(cd->dev, "%s: Init HW\n", __func__);
		rc = cd->cpdata->init(cd->cpdata, 1, cd->dev);
	} else {
		dev_info(cd->dev, "%s: No HW INIT function\n", __func__);
		rc = 0;
	}
	if (rc < 0)
		dev_err(cd->dev, "%s: HW Init fail r=%d\n", __func__, rc);

	/* Call platform detect function */
	if (cd->cpdata->detect) {
		dev_info(cd->dev, "%s: Detect HW\n", __func__);
		rc = cd->cpdata->detect(cd->cpdata, cd->dev,
				cyttsp4_platform_detect_read);
		if (rc) {
			dev_info(cd->dev, "%s: No HW detected\n", __func__);
			rc = -ENODEV;
			goto error_detect;
		}
	}

	dev_dbg(dev, "%s: initialize threaded irq=%d\n", __func__, cd->irq);
	if (cd->cpdata->level_irq_udelay > 0)
		/* use level triggered interrupts */
		irq_flags = IRQF_TRIGGER_LOW | IRQF_ONESHOT;
	else
		/* use edge triggered interrupts */
		irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;

	rc = request_threaded_irq(cd->irq, NULL, cyttsp4_irq,
			irq_flags, dev_name(dev), cd);
	if (rc < 0) {
		dev_err(dev, "%s: Error, could not request irq\n", __func__);
		goto error_request_irq;
	}

	/* Setup watchdog timer */
	timer_setup(&cd->watchdog_timer, cyttsp4_watchdog_timer, 0);

	/*
	 * call startup directly to ensure that the device
	 * is tested before leaving the probe
	 */
	dev_dbg(dev, "%s: call startup\n", __func__);
	rc = cyttsp4_startup(cd);

	pm_runtime_put_sync(dev);

	/* Do not fail probe if startup fails but the device is detected */
	if (rc == -ENODEV) { // if heartbeat not detected
		dev_err(cd->dev, "%s: Fail initial startup r=%d\n",
			__func__, rc);
		goto error_startup;
	}

	if (IS_TTSP_VER_GE(&cd->sysinfo, 2, 5))
		cd->easy_wakeup_gesture = cd->cpdata->easy_wakeup_gesture;
	else
		cd->easy_wakeup_gesture = 0xFF;

	dev_dbg(dev, "%s: add sysfs interfaces\n", __func__);
	rc = add_sysfs_interfaces(cd, dev);
	if (rc < 0) {
		dev_err(dev, "%s: Error, fail sysfs init\n", __func__);
		goto error_startup;
	}
#ifdef CYTTSP4_LOADER
	rc = cyttsp4_loader_probe(dev);
	if (rc < 0) {
		dev_err(dev, "%s: Error, fail loader probe\n", __func__);
		goto error_sysfs_interfaces;
	}
#endif
	rc = cyttsp4_mt_probe(dev);
	if (rc < 0) {
		dev_err(dev, "%s: Error, fail mt probe\n", __func__);
		goto error_startup_loader;
	}

	dev_info(dev, "%s done\n",__func__);
	return 0;

error_startup_loader:
#ifdef CYTTSP4_LOADER
	cyttsp4_loader_release(dev);
error_sysfs_interfaces:
#endif
	remove_sysfs_interfaces(cd, dev);
error_startup:
	pm_runtime_disable(dev);
	cancel_work_sync(&cd->startup_work);
	cyttsp4_stop_wd_timer(cd);
	cyttsp4_free_si_ptrs(cd);
	del_timer(&cd->watchdog_timer);
	free_irq(cd->irq, cd);
error_request_irq:
error_detect:
	if (cd->cpdata->init)
		cd->cpdata->init(cd->cpdata, 0, dev);
	cyttsp4_del_core(dev);
error_free_cd:
error_power:
error_max_xfer:
	kfree(cd);
error_no_pdata:
	dev_err(dev, "%s failed.\n", __func__);
	return rc;
}
EXPORT_SYMBOL_GPL(cyttsp4_probe);

int cyttsp4_release(struct cyttsp4_core_data *cd)
{
	struct device *dev = cd->dev;

	cyttsp4_mt_release(dev);
#ifdef CYTTSP4_LOADER
	cyttsp4_loader_release(dev);
#endif
	/*
	 * Suspend the device before freeing the startup_work and stopping
	 * the watchdog since sleep function restarts watchdog on failure
	 */
	pm_runtime_suspend(dev);
	pm_runtime_disable(dev);

	cancel_work_sync(&cd->startup_work);

	cyttsp4_stop_wd_timer(cd);

	remove_sysfs_interfaces(cd, dev);
	free_irq(cd->irq, cd);
	if (cd->cpdata->init)
		cd->cpdata->init(cd->cpdata, 0, dev);
	cyttsp4_del_core(dev);
	cyttsp4_free_si_ptrs(cd);
	kfree(cd);
	return 0;
}
EXPORT_SYMBOL_GPL(cyttsp4_release);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cypress TrueTouch(R) Standard Product Core Driver");
MODULE_AUTHOR("Cypress Semiconductor <ttdrivers@cypress.com>");
