/*
 * Copyright (C) 2010 Willow Garage <http://www.willowgarage.com>
 * Copyright (C) 2004 - 2010 Ivo van Doorn <IvDoorn@gmail.com>
 * <http://rt2x00.serialmonkey.com>
 *
 * GPL-2.0-or-later
 *
 * Userspace port (C) 2019 Hak5 Inc
 *
 */

#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include "Windows.h"
#define usleep(x) Sleep((x) < 1000 ? 1 : (x) / 1000)
#define sleep(x) Sleep(x*1000)
#endif
#include <string.h>

#include "kernel/bitops.h"
#include "kernel/cfg80211.h"
#include "kernel/crc_ccit.h"
#include "kernel/types.h"
#include "kernel/endian.h"
#include "kernel/etherdevice.h"
#include "kernel/mac80211.h"
#include "kernel/ieee80211.h"
#include "kernel/if_ether.h"
#include "kernel/kernel.h"

#include "rt2x00.h"
#include "rt2x00usb.h"
#include "rt2x00queue.h"
#include "rt2800.h"
#include "rt2800usb.h"
#include "rt2800lib.h"

/*
 * Register access.
 * All access to the CSR registers will go through the methods
 * rt2800_register_read and rt2800_register_write.
 * BBP and RF register require indirect register access,
 * and use the CSR registers BBPCSR and RFCSR to achieve this.
 * These indirect registers work with busy bits,
 * and we will try maximal REGISTER_BUSY_COUNT times to access
 * the register while taking a REGISTER_BUSY_DELAY us delay
 * between each attampt. When the busy bit is still set at that time,
 * the access attempt is considered to have failed,
 * and we will print an error.
 * The _lock versions must be used if you already hold the csr_mutex
 */
#define WAIT_FOR_BBP(__dev, __reg) \
    rt2800_regbusy_read((__dev), BBP_CSR_CFG, BBP_CSR_CFG_BUSY, (__reg))
#define WAIT_FOR_RFCSR(__dev, __reg) \
    rt2800_regbusy_read((__dev), RF_CSR_CFG, RF_CSR_CFG_BUSY, (__reg))
#define WAIT_FOR_RFCSR_MT7620(__dev, __reg) \
    rt2800_regbusy_read((__dev), RF_CSR_CFG, RF_CSR_CFG_BUSY_MT7620, \
            (__reg))
#define WAIT_FOR_RF(__dev, __reg) \
    rt2800_regbusy_read((__dev), RF_CSR_CFG0, RF_CSR_CFG0_BUSY, (__reg))
#define WAIT_FOR_MCU(__dev, __reg) \
    rt2800_regbusy_read((__dev), H2M_MAILBOX_CSR, \
            H2M_MAILBOX_CSR_OWNER, (__reg))

static inline bool rt2800_is_305x_soc(struct rt2x00_dev *rt2x00dev) {
    /* check for rt2872 on SoC */
    if (!rt2x00_is_soc(rt2x00dev) ||
            !rt2x00_rt(rt2x00dev, RT2872))
        return false;

    /* we know for sure that these rf chipsets are used on rt305x boards */
    if (rt2x00_rf(rt2x00dev, RF3020) ||
            rt2x00_rf(rt2x00dev, RF3021) ||
            rt2x00_rf(rt2x00dev, RF3022))
        return true;

    rt2x00_warn(rt2x00dev, "Unknown RF chipset on rt305x\n");
    return false;
}

static void rt2800_bbp_write(struct rt2x00_dev *rt2x00dev,
			     const unsigned int word, const uint8_t value)
{
	uint32_t reg;

	mutex_lock(&rt2x00dev->csr_mutex);

	/*
	 * Wait until the BBP becomes available, afterwards we
	 * can safely write the new data into the register.
	 */
	if (WAIT_FOR_BBP(rt2x00dev, &reg)) {
		reg = 0;
		rt2x00_set_field32(&reg, BBP_CSR_CFG_VALUE, value);
		rt2x00_set_field32(&reg, BBP_CSR_CFG_REGNUM, word);
		rt2x00_set_field32(&reg, BBP_CSR_CFG_BUSY, 1);
		rt2x00_set_field32(&reg, BBP_CSR_CFG_READ_CONTROL, 0);
		rt2x00_set_field32(&reg, BBP_CSR_CFG_BBP_RW_MODE, 1);

		rt2800_register_write_lock(rt2x00dev, BBP_CSR_CFG, reg);
	}

	mutex_unlock(&rt2x00dev->csr_mutex);
}

static uint8_t rt2800_bbp_read(struct rt2x00_dev *rt2x00dev, const unsigned int word)
{
	uint32_t reg = 0;
	uint8_t value;

	mutex_lock(&rt2x00dev->csr_mutex);

	/*
	 * Wait until the BBP becomes available, afterwards we
	 * can safely write the read request into the register.
	 * After the data has been written, we wait until hardware
	 * returns the correct value, if at any time the register
	 * doesn't become available in time, reg will be 0xffffffff
	 * which means we return 0xff to the caller.
	 */
	if (WAIT_FOR_BBP(rt2x00dev, &reg)) {
		reg = 0;
		rt2x00_set_field32(&reg, BBP_CSR_CFG_REGNUM, word);
		rt2x00_set_field32(&reg, BBP_CSR_CFG_BUSY, 1);
		rt2x00_set_field32(&reg, BBP_CSR_CFG_READ_CONTROL, 1);
		rt2x00_set_field32(&reg, BBP_CSR_CFG_BBP_RW_MODE, 1);

		rt2800_register_write_lock(rt2x00dev, BBP_CSR_CFG, reg);

		WAIT_FOR_BBP(rt2x00dev, &reg);
	}

	value = rt2x00_get_field32(reg, BBP_CSR_CFG_VALUE);

	mutex_unlock(&rt2x00dev->csr_mutex);

	return value;
}


static void rt2800_rfcsr_write(struct rt2x00_dev *rt2x00dev,
			       const unsigned int word, const uint8_t value)
{
	uint32_t reg;

	mutex_lock(&rt2x00dev->csr_mutex);

	/*
	 * Wait until the RFCSR becomes available, afterwards we
	 * can safely write the new data into the register.
	 */
	switch (rt2x00dev->chip.rt) {
	case RT6352:
		if (WAIT_FOR_RFCSR_MT7620(rt2x00dev, &reg)) {
			reg = 0;
			rt2x00_set_field32(&reg, RF_CSR_CFG_DATA_MT7620, value);
			rt2x00_set_field32(&reg, RF_CSR_CFG_REGNUM_MT7620,
					   word);
			rt2x00_set_field32(&reg, RF_CSR_CFG_WRITE_MT7620, 1);
			rt2x00_set_field32(&reg, RF_CSR_CFG_BUSY_MT7620, 1);

			rt2800_register_write_lock(rt2x00dev, RF_CSR_CFG, reg);
		}
		break;

	default:
		if (WAIT_FOR_RFCSR(rt2x00dev, &reg)) {
			reg = 0;
			rt2x00_set_field32(&reg, RF_CSR_CFG_DATA, value);
			rt2x00_set_field32(&reg, RF_CSR_CFG_REGNUM, word);
			rt2x00_set_field32(&reg, RF_CSR_CFG_WRITE, 1);
			rt2x00_set_field32(&reg, RF_CSR_CFG_BUSY, 1);

			rt2800_register_write_lock(rt2x00dev, RF_CSR_CFG, reg);
		}
		break;
	}

	mutex_unlock(&rt2x00dev->csr_mutex);
}

static void rt2800_rfcsr_write_bank(struct rt2x00_dev *rt2x00dev, const uint8_t bank,
				    const unsigned int reg, const uint8_t value)
{
	rt2800_rfcsr_write(rt2x00dev, (reg | (bank << 6)), value);
}

static void rt2800_rfcsr_write_chanreg(struct rt2x00_dev *rt2x00dev,
				       const unsigned int reg, const uint8_t value)
{
	rt2800_rfcsr_write_bank(rt2x00dev, 4, reg, value);
	rt2800_rfcsr_write_bank(rt2x00dev, 6, reg, value);
}

static void rt2800_rfcsr_write_dccal(struct rt2x00_dev *rt2x00dev,
				     const unsigned int reg, const uint8_t value)
{
	rt2800_rfcsr_write_bank(rt2x00dev, 5, reg, value);
	rt2800_rfcsr_write_bank(rt2x00dev, 7, reg, value);
}

static uint8_t rt2800_rfcsr_read(struct rt2x00_dev *rt2x00dev,
			    const unsigned int word)
{
	uint32_t reg;
	uint8_t value;

	mutex_lock(&rt2x00dev->csr_mutex);

	/*
	 * Wait until the RFCSR becomes available, afterwards we
	 * can safely write the read request into the register.
	 * After the data has been written, we wait until hardware
	 * returns the correct value, if at any time the register
	 * doesn't become available in time, reg will be 0xffffffff
	 * which means we return 0xff to the caller.
	 */
	switch (rt2x00dev->chip.rt) {
	case RT6352:
		if (WAIT_FOR_RFCSR_MT7620(rt2x00dev, &reg)) {
			reg = 0;
			rt2x00_set_field32(&reg, RF_CSR_CFG_REGNUM_MT7620,
					   word);
			rt2x00_set_field32(&reg, RF_CSR_CFG_WRITE_MT7620, 0);
			rt2x00_set_field32(&reg, RF_CSR_CFG_BUSY_MT7620, 1);

			rt2800_register_write_lock(rt2x00dev, RF_CSR_CFG, reg);

			WAIT_FOR_RFCSR_MT7620(rt2x00dev, &reg);
		}

		value = rt2x00_get_field32(reg, RF_CSR_CFG_DATA_MT7620);
		break;

	default:
		if (WAIT_FOR_RFCSR(rt2x00dev, &reg)) {
			reg = 0;
			rt2x00_set_field32(&reg, RF_CSR_CFG_REGNUM, word);
			rt2x00_set_field32(&reg, RF_CSR_CFG_WRITE, 0);
			rt2x00_set_field32(&reg, RF_CSR_CFG_BUSY, 1);

			rt2800_register_write_lock(rt2x00dev, RF_CSR_CFG, reg);

			WAIT_FOR_RFCSR(rt2x00dev, &reg);
		}

		value = rt2x00_get_field32(reg, RF_CSR_CFG_DATA);
		break;
	}

	mutex_unlock(&rt2x00dev->csr_mutex);

	return value;
}

static uint8_t rt2800_rfcsr_read_bank(struct rt2x00_dev *rt2x00dev, const uint8_t bank,
				 const unsigned int reg)
{
	return rt2800_rfcsr_read(rt2x00dev, (reg | (bank << 6)));
}

static void rt2800_rf_write(struct rt2x00_dev *rt2x00dev,
			    const unsigned int word, const uint32_t value)
{
	uint32_t reg;

	mutex_lock(&rt2x00dev->csr_mutex);

	/*
	 * Wait until the RF becomes available, afterwards we
	 * can safely write the new data into the register.
	 */
	if (WAIT_FOR_RF(rt2x00dev, &reg)) {
		reg = 0;
		rt2x00_set_field32(&reg, RF_CSR_CFG0_REG_VALUE_BW, value);
		rt2x00_set_field32(&reg, RF_CSR_CFG0_STANDBYMODE, 0);
		rt2x00_set_field32(&reg, RF_CSR_CFG0_SEL, 0);
		rt2x00_set_field32(&reg, RF_CSR_CFG0_BUSY, 1);

		rt2800_register_write_lock(rt2x00dev, RF_CSR_CFG0, reg);
		rt2x00_rf_write(rt2x00dev, word, value);
	}

	mutex_unlock(&rt2x00dev->csr_mutex);
}

static void rt2800_bbp_write_with_rx_chain(struct rt2x00_dev *rt2x00dev,
					   const unsigned int word,
					   const uint8_t value)
{
	uint8_t chain, reg;

	for (chain = 0; chain < rt2x00dev->default_ant.rx_chain_num; chain++) {
		reg = rt2800_bbp_read(rt2x00dev, 27);
		rt2x00_set_field8(&reg,  BBP27_RX_CHAIN_SEL, chain);
		rt2800_bbp_write(rt2x00dev, 27, reg);

		rt2800_bbp_write(rt2x00dev, word, value);
	}
}


static const unsigned int rt2800_eeprom_map[EEPROM_WORD_COUNT] = {
    [EEPROM_CHIP_ID]		= 0x0000,
    [EEPROM_VERSION]		= 0x0001,
    [EEPROM_MAC_ADDR_0]		= 0x0002,
    [EEPROM_MAC_ADDR_1]		= 0x0003,
    [EEPROM_MAC_ADDR_2]		= 0x0004,
    [EEPROM_NIC_CONF0]		= 0x001a,
    [EEPROM_NIC_CONF1]		= 0x001b,
    [EEPROM_FREQ]			= 0x001d,
    [EEPROM_LED_AG_CONF]		= 0x001e,
    [EEPROM_LED_ACT_CONF]		= 0x001f,
    [EEPROM_LED_POLARITY]		= 0x0020,
    [EEPROM_NIC_CONF2]		= 0x0021,
    [EEPROM_LNA]			= 0x0022,
    [EEPROM_RSSI_BG]		= 0x0023,
    [EEPROM_RSSI_BG2]		= 0x0024,
    [EEPROM_TXMIXER_GAIN_BG]	= 0x0024, /* overlaps with RSSI_BG2 */
    [EEPROM_RSSI_A]			= 0x0025,
    [EEPROM_RSSI_A2]		= 0x0026,
    [EEPROM_TXMIXER_GAIN_A]		= 0x0026, /* overlaps with RSSI_A2 */
    [EEPROM_EIRP_MAX_TX_POWER]	= 0x0027,
    [EEPROM_TXPOWER_DELTA]		= 0x0028,
    [EEPROM_TXPOWER_BG1]		= 0x0029,
    [EEPROM_TXPOWER_BG2]		= 0x0030,
    [EEPROM_TSSI_BOUND_BG1]		= 0x0037,
    [EEPROM_TSSI_BOUND_BG2]		= 0x0038,
    [EEPROM_TSSI_BOUND_BG3]		= 0x0039,
    [EEPROM_TSSI_BOUND_BG4]		= 0x003a,
    [EEPROM_TSSI_BOUND_BG5]		= 0x003b,
    [EEPROM_TXPOWER_A1]		= 0x003c,
    [EEPROM_TXPOWER_A2]		= 0x0053,
    [EEPROM_TXPOWER_INIT]		= 0x0068,
    [EEPROM_TSSI_BOUND_A1]		= 0x006a,
    [EEPROM_TSSI_BOUND_A2]		= 0x006b,
    [EEPROM_TSSI_BOUND_A3]		= 0x006c,
    [EEPROM_TSSI_BOUND_A4]		= 0x006d,
    [EEPROM_TSSI_BOUND_A5]		= 0x006e,
    [EEPROM_TXPOWER_BYRATE]		= 0x006f,
    [EEPROM_BBP_START]		= 0x0078,
};

static const unsigned int rt2800_eeprom_map_ext[EEPROM_WORD_COUNT] = {
    [EEPROM_CHIP_ID]		= 0x0000,
    [EEPROM_VERSION]		= 0x0001,
    [EEPROM_MAC_ADDR_0]		= 0x0002,
    [EEPROM_MAC_ADDR_1]		= 0x0003,
    [EEPROM_MAC_ADDR_2]		= 0x0004,
    [EEPROM_NIC_CONF0]		= 0x001a,
    [EEPROM_NIC_CONF1]		= 0x001b,
    [EEPROM_NIC_CONF2]		= 0x001c,
    [EEPROM_EIRP_MAX_TX_POWER]	= 0x0020,
    [EEPROM_FREQ]			= 0x0022,
    [EEPROM_LED_AG_CONF]		= 0x0023,
    [EEPROM_LED_ACT_CONF]		= 0x0024,
    [EEPROM_LED_POLARITY]		= 0x0025,
    [EEPROM_LNA]			= 0x0026,
    [EEPROM_EXT_LNA2]		= 0x0027,
    [EEPROM_RSSI_BG]		= 0x0028,
    [EEPROM_RSSI_BG2]		= 0x0029,
    [EEPROM_RSSI_A]			= 0x002a,
    [EEPROM_RSSI_A2]		= 0x002b,
    [EEPROM_TXPOWER_BG1]		= 0x0030,
    [EEPROM_TXPOWER_BG2]		= 0x0037,
    [EEPROM_EXT_TXPOWER_BG3]	= 0x003e,
    [EEPROM_TSSI_BOUND_BG1]		= 0x0045,
    [EEPROM_TSSI_BOUND_BG2]		= 0x0046,
    [EEPROM_TSSI_BOUND_BG3]		= 0x0047,
    [EEPROM_TSSI_BOUND_BG4]		= 0x0048,
    [EEPROM_TSSI_BOUND_BG5]		= 0x0049,
    [EEPROM_TXPOWER_A1]		= 0x004b,
    [EEPROM_TXPOWER_A2]		= 0x0065,
    [EEPROM_EXT_TXPOWER_A3]		= 0x007f,
    [EEPROM_TSSI_BOUND_A1]		= 0x009a,
    [EEPROM_TSSI_BOUND_A2]		= 0x009b,
    [EEPROM_TSSI_BOUND_A3]		= 0x009c,
    [EEPROM_TSSI_BOUND_A4]		= 0x009d,
    [EEPROM_TSSI_BOUND_A5]		= 0x009e,
    [EEPROM_TXPOWER_BYRATE]		= 0x00a0,
};

static unsigned int rt2800_eeprom_word_index(struct rt2x00_dev *rt2x00dev,
        const enum rt2800_eeprom_word word) {
    const unsigned int *map;
    unsigned int index;

    if (word >= EEPROM_WORD_COUNT) {
        rt2x00_err(rt2x00dev, "invalid EEPROM word %d\n", word);
        return 0;
    }

    if (rt2x00_rt(rt2x00dev, RT3593) ||
            rt2x00_rt(rt2x00dev, RT3883))
        map = rt2800_eeprom_map_ext;
    else
        map = rt2800_eeprom_map;

    index = map[word];

    /* Index 0 is valid only for EEPROM_CHIP_ID.
     * Otherwise it means that the offset of the
     * given word is not initialized in the map,
     * or that the field is not usable on the
     * actual chipset.
     */
    if (word != EEPROM_CHIP_ID && index == 0) {
        rt2x00_err(rt2x00dev, "invalid access of EEPROM word %d", word);
    }

    return index;
}

static void *rt2800_eeprom_addr(struct rt2x00_dev *rt2x00dev,
				const enum rt2800_eeprom_word word)
{
	unsigned int index;

	index = rt2800_eeprom_word_index(rt2x00dev, word);
	return rt2x00_eeprom_addr(rt2x00dev, index);
}

static uint16_t rt2800_eeprom_read(struct rt2x00_dev *rt2x00dev,
        const enum rt2800_eeprom_word word) {
    unsigned int index;

    index = rt2800_eeprom_word_index(rt2x00dev, word);
    return rt2x00_eeprom_read(rt2x00dev, index);
}

static void rt2800_eeprom_write(struct rt2x00_dev *rt2x00dev,
        const enum rt2800_eeprom_word word, uint16_t data) {
    unsigned int index;

    index = rt2800_eeprom_word_index(rt2x00dev, word);
    rt2x00_eeprom_write(rt2x00dev, index, data);
}

static uint16_t rt2800_eeprom_read_from_array(struct rt2x00_dev *rt2x00dev,
        const enum rt2800_eeprom_word array,
        unsigned int offset) {
    unsigned int index;

    index = rt2800_eeprom_word_index(rt2x00dev, array);
    return rt2x00_eeprom_read(rt2x00dev, index + offset);
}

static int rt2800_enable_wlan_rt3290(struct rt2x00_dev *rt2x00dev) {
    uint32_t reg;
    int i, count;

    reg = rt2800_register_read(rt2x00dev, WLAN_FUN_CTRL);
    rt2x00_set_field32(&reg, WLAN_GPIO_OUT_OE_BIT_ALL, 0xff);
    rt2x00_set_field32(&reg, FRC_WL_ANT_SET, 1);
    rt2x00_set_field32(&reg, WLAN_CLK_EN, 0);
    rt2x00_set_field32(&reg, WLAN_EN, 1);
    rt2800_register_write(rt2x00dev, WLAN_FUN_CTRL, reg);

    usleep(REGISTER_BUSY_DELAY);

    count = 0;
    do {
        /*
         * Check PLL_LD & XTAL_RDY.
         */
        for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
            reg = rt2800_register_read(rt2x00dev, CMB_CTRL);
            if (rt2x00_get_field32(reg, PLL_LD) &&
                    rt2x00_get_field32(reg, XTAL_RDY))
                break;
            usleep(REGISTER_BUSY_DELAY);
        }

        if (i >= REGISTER_BUSY_COUNT) {

            if (count >= 10)
                return -EIO;

            rt2800_register_write(rt2x00dev, 0x58, 0x018);
            usleep(REGISTER_BUSY_DELAY);
            rt2800_register_write(rt2x00dev, 0x58, 0x418);
            usleep(REGISTER_BUSY_DELAY);
            rt2800_register_write(rt2x00dev, 0x58, 0x618);
            usleep(REGISTER_BUSY_DELAY);
            count++;
        } else {
            count = 0;
        }

        reg = rt2800_register_read(rt2x00dev, WLAN_FUN_CTRL);
        rt2x00_set_field32(&reg, PCIE_APP0_CLK_REQ, 0);
        rt2x00_set_field32(&reg, WLAN_CLK_EN, 1);
        rt2x00_set_field32(&reg, WLAN_RESET, 1);
        rt2800_register_write(rt2x00dev, WLAN_FUN_CTRL, reg);
        usleep(10);
        rt2x00_set_field32(&reg, WLAN_RESET, 0);
        rt2800_register_write(rt2x00dev, WLAN_FUN_CTRL, reg);
        usleep(10);
        rt2800_register_write(rt2x00dev, INT_SOURCE_CSR, 0x7fffffff);
    } while (count != 0);

    return 0;
}

void rt2800_mcu_request(struct rt2x00_dev *rt2x00dev,
        const uint8_t command, const uint8_t token,
        const uint8_t arg0, const uint8_t arg1) {
    uint32_t reg;

	mutex_lock(&rt2x00dev->csr_mutex);

    /*
     * Wait until the MCU becomes available, afterwards we
     * can safely write the new data into the register.
     */
    if (WAIT_FOR_MCU(rt2x00dev, &reg)) {
        rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_OWNER, 1);
        rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_CMD_TOKEN, token);
        rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_ARG0, arg0);
        rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_ARG1, arg1);
        rt2800_register_write_lock(rt2x00dev, H2M_MAILBOX_CSR, reg);

        reg = 0;
        rt2x00_set_field32(&reg, HOST_CMD_CSR_HOST_COMMAND, command);
        rt2800_register_write_lock(rt2x00dev, HOST_CMD_CSR, reg);
    }

	mutex_unlock(&rt2x00dev->csr_mutex);
}

int rt2800_wait_csr_ready(struct rt2x00_dev *rt2x00dev) {
    unsigned int i = 0;
    uint32_t reg;

    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        reg = rt2800_register_read(rt2x00dev, MAC_CSR0);
        if (reg && reg != ~0U)
            return 0;
        usleep(1);
    }

    rt2x00_err(rt2x00dev, "Unstable hardware\n");
    return -EBUSY;
}

int rt2800_wait_wpdma_ready(struct rt2x00_dev *rt2x00dev) {
    unsigned int i;
    uint32_t reg;

    /*
     * Some devices are really slow to respond here. Wait a whole second
     * before timing out.
     */
    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        reg = rt2800_register_read(rt2x00dev, WPDMA_GLO_CFG);
        if (!rt2x00_get_field32(reg, WPDMA_GLO_CFG_TX_DMA_BUSY) &&
                !rt2x00_get_field32(reg, WPDMA_GLO_CFG_RX_DMA_BUSY))
            return 0;

        usleep(10);
    }

    rt2x00_err(rt2x00dev, "WPDMA TX/RX busy [0x%08x]\n", reg);
    return -EACCES;
}

int rt2800_efuse_detect(struct rt2x00_dev *rt2x00dev) {
    uint32_t reg;
    uint16_t efuse_ctrl_reg;

    if (rt2x00_rt(rt2x00dev, RT3290))
        efuse_ctrl_reg = EFUSE_CTRL_3290;
    else
        efuse_ctrl_reg = EFUSE_CTRL;

    reg = rt2800_register_read(rt2x00dev, efuse_ctrl_reg);
    return rt2x00_get_field32(reg, EFUSE_CTRL_PRESENT);
}

static void rt2800_efuse_read(struct rt2x00_dev *rt2x00dev, unsigned int i) {
    uint32_t reg;
    uint16_t efuse_ctrl_reg;
    uint16_t efuse_data0_reg;
    uint16_t efuse_data1_reg;
    uint16_t efuse_data2_reg;
    uint16_t efuse_data3_reg;

    if (rt2x00_rt(rt2x00dev, RT3290)) {
        efuse_ctrl_reg = EFUSE_CTRL_3290;
        efuse_data0_reg = EFUSE_DATA0_3290;
        efuse_data1_reg = EFUSE_DATA1_3290;
        efuse_data2_reg = EFUSE_DATA2_3290;
        efuse_data3_reg = EFUSE_DATA3_3290;
    } else {
        efuse_ctrl_reg = EFUSE_CTRL;
        efuse_data0_reg = EFUSE_DATA0;
        efuse_data1_reg = EFUSE_DATA1;
        efuse_data2_reg = EFUSE_DATA2;
        efuse_data3_reg = EFUSE_DATA3;
    }

    reg = rt2800_register_read_lock(rt2x00dev, efuse_ctrl_reg);
    rt2x00_set_field32(&reg, EFUSE_CTRL_ADDRESS_IN, i);
    rt2x00_set_field32(&reg, EFUSE_CTRL_MODE, 0);
    rt2x00_set_field32(&reg, EFUSE_CTRL_KICK, 1);
    rt2800_register_write_lock(rt2x00dev, efuse_ctrl_reg, reg);

    /* Wait until the EEPROM has been loaded */
    rt2800_regbusy_read(rt2x00dev, efuse_ctrl_reg, EFUSE_CTRL_KICK, &reg);
    /* Apparently the data is read from end to start */
    reg = rt2800_register_read_lock(rt2x00dev, efuse_data3_reg);
    /* The returned value is in CPU order, but eeprom is le */
    *(uint32_t *)&rt2x00dev->eeprom[i] = cpu_to_le32(reg);
    reg = rt2800_register_read_lock(rt2x00dev, efuse_data2_reg);
    *(uint32_t *)&rt2x00dev->eeprom[i + 2] = cpu_to_le32(reg);
    reg = rt2800_register_read_lock(rt2x00dev, efuse_data1_reg);
    *(uint32_t *)&rt2x00dev->eeprom[i + 4] = cpu_to_le32(reg);
    reg = rt2800_register_read_lock(rt2x00dev, efuse_data0_reg);
    *(uint32_t *)&rt2x00dev->eeprom[i + 6] = cpu_to_le32(reg);
}

int rt2800_read_eeprom_efuse(struct rt2x00_dev *rt2x00dev) {
    unsigned int i;

    for (i = 0; i < EEPROM_SIZE / sizeof(uint16_t); i += 8)
        rt2800_efuse_read(rt2x00dev, i);

    return 0;
}

/*
 * Configuration handlers.
 */
static void rt2800_config_wcid(struct rt2x00_dev *rt2x00dev,
        const uint8_t *address,
        int wcid) {
    struct mac_wcid_entry wcid_entry;
    uint32_t offset;

    offset = MAC_WCID_ENTRY(wcid);

    memset(&wcid_entry, 0xff, sizeof(wcid_entry));
    if (address)
        memcpy(wcid_entry.mac, address, ETH_ALEN);

    rt2800_register_multiwrite(rt2x00dev, offset,
            &wcid_entry, sizeof(wcid_entry));
}

static void rt2800_delete_wcid_attr(struct rt2x00_dev *rt2x00dev, int wcid) {
    uint32_t offset;
    offset = MAC_WCID_ATTR_ENTRY(wcid);
    rt2800_register_write(rt2x00dev, offset, 0);
}

#if 0
static void rt2800_config_wcid_attr_bssidx(struct rt2x00_dev *rt2x00dev,
        int wcid, uint32_t bssidx) {
    uint32_t offset = MAC_WCID_ATTR_ENTRY(wcid);
    uint32_t reg;

    /*
     * The BSS Idx numbers is split in a main value of 3 bits,
     * and a extended field for adding one additional bit to the value.
     */
    reg = rt2800_register_read(rt2x00dev, offset);
    rt2x00_set_field32(&reg, MAC_WCID_ATTRIBUTE_BSS_IDX, (bssidx & 0x7));
    rt2x00_set_field32(&reg, MAC_WCID_ATTRIBUTE_BSS_IDX_EXT,
            (bssidx & 0x8) >> 3);
    rt2800_register_write(rt2x00dev, offset, reg);
}

static void rt2800_config_wcid_attr_cipher(struct rt2x00_dev *rt2x00dev,
        struct rt2x00lib_crypto *crypto,
        struct ieee80211_key_conf *key) {
    struct mac_iveiv_entry iveiv_entry;
    uint32_t offset;
    uint32_t reg;

    /* 
     * Gutted to simply clear the keys, we never set it since we
     * don't tx from userspace
     */

    offset = MAC_WCID_ATTR_ENTRY(key->hw_key_idx);

    /* Delete the cipher without touching the bssidx */
    reg = rt2800_register_read(rt2x00dev, offset);
    rt2x00_set_field32(&reg, MAC_WCID_ATTRIBUTE_KEYTAB, 0);
    rt2x00_set_field32(&reg, MAC_WCID_ATTRIBUTE_CIPHER, 0);
    rt2x00_set_field32(&reg, MAC_WCID_ATTRIBUTE_CIPHER_EXT, 0);
    rt2x00_set_field32(&reg, MAC_WCID_ATTRIBUTE_RX_WIUDF, 0);
    rt2800_register_write(rt2x00dev, offset, reg);

    offset = MAC_IVEIV_ENTRY(key->hw_key_idx);

    /* 
     * Similarly gutted of the key type setting
     */
    memset(&iveiv_entry, 0, sizeof(iveiv_entry));
    iveiv_entry.iv[3] |= key->keyidx << 6;
    rt2800_register_multiwrite(rt2x00dev, offset,
            &iveiv_entry, sizeof(iveiv_entry));
}
#endif

/*
 * We don't allow userspace to tx so we don't have to do anything here
 */
static inline void rt2800_clear_beacon_register(struct rt2x00_dev *rt2x00dev,
        unsigned int index) { 

}

static void rt2800_config_3572bt_ant(struct rt2x00_dev *rt2x00dev)
{
	uint32_t reg;
	uint16_t eeprom;
	uint8_t led_ctrl, led_g_mode, led_r_mode;

	reg = rt2800_register_read(rt2x00dev, GPIO_SWITCH);
	if (rt2x00dev->curr_band == NL80211_BAND_5GHZ) {
		rt2x00_set_field32(&reg, GPIO_SWITCH_0, 1);
		rt2x00_set_field32(&reg, GPIO_SWITCH_1, 1);
	} else {
		rt2x00_set_field32(&reg, GPIO_SWITCH_0, 0);
		rt2x00_set_field32(&reg, GPIO_SWITCH_1, 0);
	}
	rt2800_register_write(rt2x00dev, GPIO_SWITCH, reg);

	reg = rt2800_register_read(rt2x00dev, LED_CFG);
	led_g_mode = rt2x00_get_field32(reg, LED_CFG_LED_POLAR) ? 3 : 0;
	led_r_mode = rt2x00_get_field32(reg, LED_CFG_LED_POLAR) ? 0 : 3;
	if (led_g_mode != rt2x00_get_field32(reg, LED_CFG_G_LED_MODE) ||
	    led_r_mode != rt2x00_get_field32(reg, LED_CFG_R_LED_MODE)) {
		eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_FREQ);
		led_ctrl = rt2x00_get_field16(eeprom, EEPROM_FREQ_LED_MODE);
		if (led_ctrl == 0 || led_ctrl > 0x40) {
			rt2x00_set_field32(&reg, LED_CFG_G_LED_MODE, led_g_mode);
			rt2x00_set_field32(&reg, LED_CFG_R_LED_MODE, led_r_mode);
			rt2800_register_write(rt2x00dev, LED_CFG, reg);
		} else {
			rt2800_mcu_request(rt2x00dev, MCU_BAND_SELECT, 0xff,
					   (led_g_mode << 2) | led_r_mode, 1);
		}
	}
}

static void rt2800_set_ant_diversity(struct rt2x00_dev *rt2x00dev,
				     enum antenna ant)
{
	uint32_t reg;
	uint8_t eesk_pin = (ant == ANTENNA_A) ? 1 : 0;
	uint8_t gpio_bit3 = (ant == ANTENNA_A) ? 0 : 1;

	if (rt2x00_is_pci(rt2x00dev)) {
		reg = rt2800_register_read(rt2x00dev, E2PROM_CSR);
		rt2x00_set_field32(&reg, E2PROM_CSR_DATA_CLOCK, eesk_pin);
		rt2800_register_write(rt2x00dev, E2PROM_CSR, reg);
	} else if (rt2x00_is_usb(rt2x00dev))
		rt2800_mcu_request(rt2x00dev, MCU_ANT_SELECT, 0xff,
				   eesk_pin, 0);

	reg = rt2800_register_read(rt2x00dev, GPIO_CTRL);
	rt2x00_set_field32(&reg, GPIO_CTRL_DIR3, 0);
	rt2x00_set_field32(&reg, GPIO_CTRL_VAL3, gpio_bit3);
	rt2800_register_write(rt2x00dev, GPIO_CTRL, reg);
}

void rt2800_config_ant(struct rt2x00_dev *rt2x00dev, struct antenna_setup *ant)
{
	uint8_t r1;
	uint8_t r3;
	uint16_t eeprom;

    rt2x00_info(rt2x00dev, "Configuring rt2800 antenna\n");

	r1 = rt2800_bbp_read(rt2x00dev, 1);
	r3 = rt2800_bbp_read(rt2x00dev, 3);

	if (rt2x00_rt(rt2x00dev, RT3572) &&
	    rt2x00_has_cap_bt_coexist(rt2x00dev))
		rt2800_config_3572bt_ant(rt2x00dev);

	/*
	 * Configure the TX antenna.
	 */
	switch (ant->tx_chain_num) {
	case 1:
		rt2x00_set_field8(&r1, BBP1_TX_ANTENNA, 0);
		break;
	case 2:
		if (rt2x00_rt(rt2x00dev, RT3572) &&
		    rt2x00_has_cap_bt_coexist(rt2x00dev))
			rt2x00_set_field8(&r1, BBP1_TX_ANTENNA, 1);
		else
			rt2x00_set_field8(&r1, BBP1_TX_ANTENNA, 2);
		break;
	case 3:
		rt2x00_set_field8(&r1, BBP1_TX_ANTENNA, 2);
		break;
	}

	/*
	 * Configure the RX antenna.
	 */
	switch (ant->rx_chain_num) {
	case 1:
		if (rt2x00_rt(rt2x00dev, RT3070) ||
		    rt2x00_rt(rt2x00dev, RT3090) ||
		    rt2x00_rt(rt2x00dev, RT3352) ||
		    rt2x00_rt(rt2x00dev, RT3390)) {
			eeprom = rt2800_eeprom_read(rt2x00dev,
						    EEPROM_NIC_CONF1);
			if (rt2x00_get_field16(eeprom,
						EEPROM_NIC_CONF1_ANT_DIVERSITY))
				rt2800_set_ant_diversity(rt2x00dev,
						rt2x00dev->default_ant.rx);
		}
		rt2x00_set_field8(&r3, BBP3_RX_ANTENNA, 0);
		break;
	case 2:
		if (rt2x00_rt(rt2x00dev, RT3572) &&
		    rt2x00_has_cap_bt_coexist(rt2x00dev)) {
			rt2x00_set_field8(&r3, BBP3_RX_ADC, 1);
			rt2x00_set_field8(&r3, BBP3_RX_ANTENNA,
				rt2x00dev->curr_band == NL80211_BAND_5GHZ);
			rt2800_set_ant_diversity(rt2x00dev, ANTENNA_B);
		} else {
			rt2x00_set_field8(&r3, BBP3_RX_ANTENNA, 1);
		}
		break;
	case 3:
		rt2x00_set_field8(&r3, BBP3_RX_ANTENNA, 2);
		break;
	}

	rt2800_bbp_write(rt2x00dev, 3, r3);
	rt2800_bbp_write(rt2x00dev, 1, r1);

	if (rt2x00_rt(rt2x00dev, RT3593) ||
	    rt2x00_rt(rt2x00dev, RT3883)) {
		if (ant->rx_chain_num == 1)
			rt2800_bbp_write(rt2x00dev, 86, 0x00);
		else
			rt2800_bbp_write(rt2x00dev, 86, 0x46);
	}
}
EXPORT_SYMBOL_GPL(rt2800_config_ant);

static void rt2800_config_lna_gain(struct rt2x00_dev *rt2x00dev,
				   struct rt2x00lib_conf *libconf)
{
	uint16_t eeprom;
	short lna_gain;

	if (libconf->rf.channel <= 14) {
		eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_LNA);
		lna_gain = rt2x00_get_field16(eeprom, EEPROM_LNA_BG);
	} else if (libconf->rf.channel <= 64) {
		eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_LNA);
		lna_gain = rt2x00_get_field16(eeprom, EEPROM_LNA_A0);
	} else if (libconf->rf.channel <= 128) {
		if (rt2x00_rt(rt2x00dev, RT3593) ||
		    rt2x00_rt(rt2x00dev, RT3883)) {
			eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_EXT_LNA2);
			lna_gain = rt2x00_get_field16(eeprom,
						      EEPROM_EXT_LNA2_A1);
		} else {
			eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_RSSI_BG2);
			lna_gain = rt2x00_get_field16(eeprom,
						      EEPROM_RSSI_BG2_LNA_A1);
		}
	} else {
		if (rt2x00_rt(rt2x00dev, RT3593) ||
		    rt2x00_rt(rt2x00dev, RT3883)) {
			eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_EXT_LNA2);
			lna_gain = rt2x00_get_field16(eeprom,
						      EEPROM_EXT_LNA2_A2);
		} else {
			eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_RSSI_A2);
			lna_gain = rt2x00_get_field16(eeprom,
						      EEPROM_RSSI_A2_LNA_A2);
		}
	}

	rt2x00dev->lna_gain = lna_gain;
}

#define FREQ_OFFSET_BOUND	0x5f

static void rt2800_freq_cal_mode1(struct rt2x00_dev *rt2x00dev)
{
	uint8_t freq_offset, prev_freq_offset;
	uint8_t rfcsr, prev_rfcsr;

	freq_offset = rt2x00_get_field8(rt2x00dev->freq_offset, RFCSR17_CODE);
	freq_offset = min_t(uint8_t, freq_offset, FREQ_OFFSET_BOUND);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 17);
	prev_rfcsr = rfcsr;

	rt2x00_set_field8(&rfcsr, RFCSR17_CODE, freq_offset);
	if (rfcsr == prev_rfcsr)
		return;

	if (rt2x00_is_usb(rt2x00dev)) {
		rt2800_mcu_request(rt2x00dev, MCU_FREQ_OFFSET, 0xff,
				   freq_offset, prev_rfcsr);
		return;
	}

	prev_freq_offset = rt2x00_get_field8(prev_rfcsr, RFCSR17_CODE);
	while (prev_freq_offset != freq_offset) {
		if (prev_freq_offset < freq_offset)
			prev_freq_offset++;
		else
			prev_freq_offset--;

		rt2x00_set_field8(&rfcsr, RFCSR17_CODE, prev_freq_offset);
		rt2800_rfcsr_write(rt2x00dev, 17, rfcsr);

		usleep_range(1000, 1500);
	}
}

static void rt2800_config_channel_rf2xxx(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	rt2x00_set_field32(&rf->rf4, RF4_FREQ_OFFSET, rt2x00dev->freq_offset);

	if (rt2x00dev->default_ant.tx_chain_num == 1)
		rt2x00_set_field32(&rf->rf2, RF2_ANTENNA_TX1, 1);

	if (rt2x00dev->default_ant.rx_chain_num == 1) {
		rt2x00_set_field32(&rf->rf2, RF2_ANTENNA_RX1, 1);
		rt2x00_set_field32(&rf->rf2, RF2_ANTENNA_RX2, 1);
	} else if (rt2x00dev->default_ant.rx_chain_num == 2)
		rt2x00_set_field32(&rf->rf2, RF2_ANTENNA_RX2, 1);

	if (rf->channel > 14) {
		/*
		 * When TX power is below 0, we should increase it by 7 to
		 * make it a positive value (Minimum value is -7).
		 * However this means that values between 0 and 7 have
		 * double meaning, and we should set a 7DBm boost flag.
		 */
		rt2x00_set_field32(&rf->rf3, RF3_TXPOWER_A_7DBM_BOOST,
				   (info->default_power1 >= 0));

		if (info->default_power1 < 0)
			info->default_power1 += 7;

		rt2x00_set_field32(&rf->rf3, RF3_TXPOWER_A, info->default_power1);

		rt2x00_set_field32(&rf->rf4, RF4_TXPOWER_A_7DBM_BOOST,
				   (info->default_power2 >= 0));

		if (info->default_power2 < 0)
			info->default_power2 += 7;

		rt2x00_set_field32(&rf->rf4, RF4_TXPOWER_A, info->default_power2);
	} else {
		rt2x00_set_field32(&rf->rf3, RF3_TXPOWER_G, info->default_power1);
		rt2x00_set_field32(&rf->rf4, RF4_TXPOWER_G, info->default_power2);
	}

	rt2x00_set_field32(&rf->rf4, RF4_HT40, conf_is_ht40(conf));

	rt2800_rf_write(rt2x00dev, 1, rf->rf1);
	rt2800_rf_write(rt2x00dev, 2, rf->rf2);
	rt2800_rf_write(rt2x00dev, 3, rf->rf3 & ~0x00000004);
	rt2800_rf_write(rt2x00dev, 4, rf->rf4);

	udelay(200);

	rt2800_rf_write(rt2x00dev, 1, rf->rf1);
	rt2800_rf_write(rt2x00dev, 2, rf->rf2);
	rt2800_rf_write(rt2x00dev, 3, rf->rf3 | 0x00000004);
	rt2800_rf_write(rt2x00dev, 4, rf->rf4);

	udelay(200);

	rt2800_rf_write(rt2x00dev, 1, rf->rf1);
	rt2800_rf_write(rt2x00dev, 2, rf->rf2);
	rt2800_rf_write(rt2x00dev, 3, rf->rf3 & ~0x00000004);
	rt2800_rf_write(rt2x00dev, 4, rf->rf4);
}

static void rt2800_config_channel_rf3xxx(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	struct rt2800_drv_data *drv_data = (struct rt2800_drv_data *) rt2x00dev->drv_data;
	uint8_t rfcsr, calib_tx, calib_rx;

	rt2800_rfcsr_write(rt2x00dev, 2, rf->rf1);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 3);
	rt2x00_set_field8(&rfcsr, RFCSR3_K, rf->rf3);
	rt2800_rfcsr_write(rt2x00dev, 3, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 6);
	rt2x00_set_field8(&rfcsr, RFCSR6_R1, rf->rf2);
	rt2800_rfcsr_write(rt2x00dev, 6, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 12);
	rt2x00_set_field8(&rfcsr, RFCSR12_TX_POWER, info->default_power1);
	rt2800_rfcsr_write(rt2x00dev, 12, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 13);
	rt2x00_set_field8(&rfcsr, RFCSR13_TX_POWER, info->default_power2);
	rt2800_rfcsr_write(rt2x00dev, 13, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD,
			  rt2x00dev->default_ant.rx_chain_num <= 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD,
			  rt2x00dev->default_ant.rx_chain_num <= 2);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD,
			  rt2x00dev->default_ant.tx_chain_num <= 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD,
			  rt2x00dev->default_ant.tx_chain_num <= 2);
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 23);
	rt2x00_set_field8(&rfcsr, RFCSR23_FREQ_OFFSET, rt2x00dev->freq_offset);
	rt2800_rfcsr_write(rt2x00dev, 23, rfcsr);

	if (rt2x00_rt(rt2x00dev, RT3390)) {
		calib_tx = conf_is_ht40(conf) ? 0x68 : 0x4f;
		calib_rx = conf_is_ht40(conf) ? 0x6f : 0x4f;
	} else {
		if (conf_is_ht40(conf)) {
			calib_tx = drv_data->calibration_bw40;
			calib_rx = drv_data->calibration_bw40;
		} else {
			calib_tx = drv_data->calibration_bw20;
			calib_rx = drv_data->calibration_bw20;
		}
	}

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 24);
	rt2x00_set_field8(&rfcsr, RFCSR24_TX_CALIB, calib_tx);
	rt2800_rfcsr_write(rt2x00dev, 24, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 31);
	rt2x00_set_field8(&rfcsr, RFCSR31_RX_CALIB, calib_rx);
	rt2800_rfcsr_write(rt2x00dev, 31, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 7);
	rt2x00_set_field8(&rfcsr, RFCSR7_RF_TUNING, 1);
	rt2800_rfcsr_write(rt2x00dev, 7, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 30);
	rt2x00_set_field8(&rfcsr, RFCSR30_RF_CALIBRATION, 1);
	rt2800_rfcsr_write(rt2x00dev, 30, rfcsr);

	usleep_range(1000, 1500);

	rt2x00_set_field8(&rfcsr, RFCSR30_RF_CALIBRATION, 0);
	rt2800_rfcsr_write(rt2x00dev, 30, rfcsr);
}

static void rt2800_config_channel_rf3052(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	struct rt2800_drv_data *drv_data = (struct rt2800_drv_data *) rt2x00dev->drv_data;
	uint8_t rfcsr;
	uint32_t reg;

	if (rf->channel <= 14) {
		rt2800_bbp_write(rt2x00dev, 25, drv_data->bbp25);
		rt2800_bbp_write(rt2x00dev, 26, drv_data->bbp26);
	} else {
		rt2800_bbp_write(rt2x00dev, 25, 0x09);
		rt2800_bbp_write(rt2x00dev, 26, 0xff);
	}

	rt2800_rfcsr_write(rt2x00dev, 2, rf->rf1);
	rt2800_rfcsr_write(rt2x00dev, 3, rf->rf3);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 6);
	rt2x00_set_field8(&rfcsr, RFCSR6_R1, rf->rf2);
	if (rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR6_TXDIV, 2);
	else
		rt2x00_set_field8(&rfcsr, RFCSR6_TXDIV, 1);
	rt2800_rfcsr_write(rt2x00dev, 6, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 5);
	if (rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR5_R1, 1);
	else
		rt2x00_set_field8(&rfcsr, RFCSR5_R1, 2);
	rt2800_rfcsr_write(rt2x00dev, 5, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 12);
	if (rf->channel <= 14) {
		rt2x00_set_field8(&rfcsr, RFCSR12_DR0, 3);
		rt2x00_set_field8(&rfcsr, RFCSR12_TX_POWER,
				  info->default_power1);
	} else {
		rt2x00_set_field8(&rfcsr, RFCSR12_DR0, 7);
		rt2x00_set_field8(&rfcsr, RFCSR12_TX_POWER,
				(info->default_power1 & 0x3) |
				((info->default_power1 & 0xC) << 1));
	}
	rt2800_rfcsr_write(rt2x00dev, 12, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 13);
	if (rf->channel <= 14) {
		rt2x00_set_field8(&rfcsr, RFCSR13_DR0, 3);
		rt2x00_set_field8(&rfcsr, RFCSR13_TX_POWER,
				  info->default_power2);
	} else {
		rt2x00_set_field8(&rfcsr, RFCSR13_DR0, 7);
		rt2x00_set_field8(&rfcsr, RFCSR13_TX_POWER,
				(info->default_power2 & 0x3) |
				((info->default_power2 & 0xC) << 1));
	}
	rt2800_rfcsr_write(rt2x00dev, 13, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 0);
	if (rt2x00_has_cap_bt_coexist(rt2x00dev)) {
		if (rf->channel <= 14) {
			rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 1);
			rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 1);
		}
		rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 1);
		rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 1);
	} else {
		switch (rt2x00dev->default_ant.tx_chain_num) {
		case 1:
			rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 1);
			/* fall through */
		case 2:
			rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 1);
			break;
		}

		switch (rt2x00dev->default_ant.rx_chain_num) {
		case 1:
			rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 1);
			/* fall through */
		case 2:
			rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 1);
			break;
		}
	}
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 23);
	rt2x00_set_field8(&rfcsr, RFCSR23_FREQ_OFFSET, rt2x00dev->freq_offset);
	rt2800_rfcsr_write(rt2x00dev, 23, rfcsr);

	if (conf_is_ht40(conf)) {
		rt2800_rfcsr_write(rt2x00dev, 24, drv_data->calibration_bw40);
		rt2800_rfcsr_write(rt2x00dev, 31, drv_data->calibration_bw40);
	} else {
		rt2800_rfcsr_write(rt2x00dev, 24, drv_data->calibration_bw20);
		rt2800_rfcsr_write(rt2x00dev, 31, drv_data->calibration_bw20);
	}

	if (rf->channel <= 14) {
		rt2800_rfcsr_write(rt2x00dev, 7, 0xd8);
		rt2800_rfcsr_write(rt2x00dev, 9, 0xc3);
		rt2800_rfcsr_write(rt2x00dev, 10, 0xf1);
		rt2800_rfcsr_write(rt2x00dev, 11, 0xb9);
		rt2800_rfcsr_write(rt2x00dev, 15, 0x53);
		rfcsr = 0x4c;
		rt2x00_set_field8(&rfcsr, RFCSR16_TXMIXER_GAIN,
				  drv_data->txmixer_gain_24g);
		rt2800_rfcsr_write(rt2x00dev, 16, rfcsr);
		rt2800_rfcsr_write(rt2x00dev, 17, 0x23);
		rt2800_rfcsr_write(rt2x00dev, 19, 0x93);
		rt2800_rfcsr_write(rt2x00dev, 20, 0xb3);
		rt2800_rfcsr_write(rt2x00dev, 25, 0x15);
		rt2800_rfcsr_write(rt2x00dev, 26, 0x85);
		rt2800_rfcsr_write(rt2x00dev, 27, 0x00);
		rt2800_rfcsr_write(rt2x00dev, 29, 0x9b);
	} else {
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 7);
		rt2x00_set_field8(&rfcsr, RFCSR7_BIT2, 1);
		rt2x00_set_field8(&rfcsr, RFCSR7_BIT3, 0);
		rt2x00_set_field8(&rfcsr, RFCSR7_BIT4, 1);
		rt2x00_set_field8(&rfcsr, RFCSR7_BITS67, 0);
		rt2800_rfcsr_write(rt2x00dev, 7, rfcsr);
		rt2800_rfcsr_write(rt2x00dev, 9, 0xc0);
		rt2800_rfcsr_write(rt2x00dev, 10, 0xf1);
		rt2800_rfcsr_write(rt2x00dev, 11, 0x00);
		rt2800_rfcsr_write(rt2x00dev, 15, 0x43);
		rfcsr = 0x7a;
		rt2x00_set_field8(&rfcsr, RFCSR16_TXMIXER_GAIN,
				  drv_data->txmixer_gain_5g);
		rt2800_rfcsr_write(rt2x00dev, 16, rfcsr);
		rt2800_rfcsr_write(rt2x00dev, 17, 0x23);
		if (rf->channel <= 64) {
			rt2800_rfcsr_write(rt2x00dev, 19, 0xb7);
			rt2800_rfcsr_write(rt2x00dev, 20, 0xf6);
			rt2800_rfcsr_write(rt2x00dev, 25, 0x3d);
		} else if (rf->channel <= 128) {
			rt2800_rfcsr_write(rt2x00dev, 19, 0x74);
			rt2800_rfcsr_write(rt2x00dev, 20, 0xf4);
			rt2800_rfcsr_write(rt2x00dev, 25, 0x01);
		} else {
			rt2800_rfcsr_write(rt2x00dev, 19, 0x72);
			rt2800_rfcsr_write(rt2x00dev, 20, 0xf3);
			rt2800_rfcsr_write(rt2x00dev, 25, 0x01);
		}
		rt2800_rfcsr_write(rt2x00dev, 26, 0x87);
		rt2800_rfcsr_write(rt2x00dev, 27, 0x01);
		rt2800_rfcsr_write(rt2x00dev, 29, 0x9f);
	}

	reg = rt2800_register_read(rt2x00dev, GPIO_CTRL);
	rt2x00_set_field32(&reg, GPIO_CTRL_DIR7, 0);
	if (rf->channel <= 14)
		rt2x00_set_field32(&reg, GPIO_CTRL_VAL7, 1);
	else
		rt2x00_set_field32(&reg, GPIO_CTRL_VAL7, 0);
	rt2800_register_write(rt2x00dev, GPIO_CTRL, reg);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 7);
	rt2x00_set_field8(&rfcsr, RFCSR7_RF_TUNING, 1);
	rt2800_rfcsr_write(rt2x00dev, 7, rfcsr);
}

static void rt2800_config_channel_rf3053(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	struct rt2800_drv_data *drv_data = (struct rt2800_drv_data *) rt2x00dev->drv_data;
	uint8_t txrx_agc_fc;
	uint8_t txrx_h20m;
	uint8_t rfcsr;
	uint8_t bbp;
	const bool txbf_enabled = false; /* TODO */

	/* TODO: use TX{0,1,2}FinePowerControl values from EEPROM */
	bbp = rt2800_bbp_read(rt2x00dev, 109);
	rt2x00_set_field8(&bbp, BBP109_TX0_POWER, 0);
	rt2x00_set_field8(&bbp, BBP109_TX1_POWER, 0);
	rt2800_bbp_write(rt2x00dev, 109, bbp);

	bbp = rt2800_bbp_read(rt2x00dev, 110);
	rt2x00_set_field8(&bbp, BBP110_TX2_POWER, 0);
	rt2800_bbp_write(rt2x00dev, 110, bbp);

	if (rf->channel <= 14) {
		/* Restore BBP 25 & 26 for 2.4 GHz */
		rt2800_bbp_write(rt2x00dev, 25, drv_data->bbp25);
		rt2800_bbp_write(rt2x00dev, 26, drv_data->bbp26);
	} else {
		/* Hard code BBP 25 & 26 for 5GHz */

		/* Enable IQ Phase correction */
		rt2800_bbp_write(rt2x00dev, 25, 0x09);
		/* Setup IQ Phase correction value */
		rt2800_bbp_write(rt2x00dev, 26, 0xff);
	}

	rt2800_rfcsr_write(rt2x00dev, 8, rf->rf1);
	rt2800_rfcsr_write(rt2x00dev, 9, rf->rf3 & 0xf);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 11);
	rt2x00_set_field8(&rfcsr, RFCSR11_R, (rf->rf2 & 0x3));
	rt2800_rfcsr_write(rt2x00dev, 11, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 11);
	rt2x00_set_field8(&rfcsr, RFCSR11_PLL_IDOH, 1);
	if (rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR11_PLL_MOD, 1);
	else
		rt2x00_set_field8(&rfcsr, RFCSR11_PLL_MOD, 2);
	rt2800_rfcsr_write(rt2x00dev, 11, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 53);
	if (rf->channel <= 14) {
		rfcsr = 0;
		rt2x00_set_field8(&rfcsr, RFCSR53_TX_POWER,
				  info->default_power1 & 0x1f);
	} else {
		if (rt2x00_is_usb(rt2x00dev))
			rfcsr = 0x40;

		rt2x00_set_field8(&rfcsr, RFCSR53_TX_POWER,
				  ((info->default_power1 & 0x18) << 1) |
				  (info->default_power1 & 7));
	}
	rt2800_rfcsr_write(rt2x00dev, 53, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 55);
	if (rf->channel <= 14) {
		rfcsr = 0;
		rt2x00_set_field8(&rfcsr, RFCSR55_TX_POWER,
				  info->default_power2 & 0x1f);
	} else {
		if (rt2x00_is_usb(rt2x00dev))
			rfcsr = 0x40;

		rt2x00_set_field8(&rfcsr, RFCSR55_TX_POWER,
				  ((info->default_power2 & 0x18) << 1) |
				  (info->default_power2 & 7));
	}
	rt2800_rfcsr_write(rt2x00dev, 55, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 54);
	if (rf->channel <= 14) {
		rfcsr = 0;
		rt2x00_set_field8(&rfcsr, RFCSR54_TX_POWER,
				  info->default_power3 & 0x1f);
	} else {
		if (rt2x00_is_usb(rt2x00dev))
			rfcsr = 0x40;

		rt2x00_set_field8(&rfcsr, RFCSR54_TX_POWER,
				  ((info->default_power3 & 0x18) << 1) |
				  (info->default_power3 & 7));
	}
	rt2800_rfcsr_write(rt2x00dev, 54, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RF_BLOCK_EN, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_PLL_PD, 1);

	switch (rt2x00dev->default_ant.tx_chain_num) {
	case 3:
		rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 1);
		/* fallthrough */
	case 2:
		rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 1);
		/* fallthrough */
	case 1:
		rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 1);
		break;
	}

	switch (rt2x00dev->default_ant.rx_chain_num) {
	case 3:
		rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 1);
		/* fallthrough */
	case 2:
		rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 1);
		/* fallthrough */
	case 1:
		rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 1);
		break;
	}
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rt2800_freq_cal_mode1(rt2x00dev);

	if (conf_is_ht40(conf)) {
		txrx_agc_fc = rt2x00_get_field8(drv_data->calibration_bw40,
						RFCSR24_TX_AGC_FC);
		txrx_h20m = rt2x00_get_field8(drv_data->calibration_bw40,
					      RFCSR24_TX_H20M);
	} else {
		txrx_agc_fc = rt2x00_get_field8(drv_data->calibration_bw20,
						RFCSR24_TX_AGC_FC);
		txrx_h20m = rt2x00_get_field8(drv_data->calibration_bw20,
					      RFCSR24_TX_H20M);
	}

	/* NOTE: the reference driver does not writes the new value
	 * back to RFCSR 32
	 */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 32);
	rt2x00_set_field8(&rfcsr, RFCSR32_TX_AGC_FC, txrx_agc_fc);

	if (rf->channel <= 14)
		rfcsr = 0xa0;
	else
		rfcsr = 0x80;
	rt2800_rfcsr_write(rt2x00dev, 31, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 30);
	rt2x00_set_field8(&rfcsr, RFCSR30_TX_H20M, txrx_h20m);
	rt2x00_set_field8(&rfcsr, RFCSR30_RX_H20M, txrx_h20m);
	rt2800_rfcsr_write(rt2x00dev, 30, rfcsr);

	/* Band selection */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 36);
	if (rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR36_RF_BS, 1);
	else
		rt2x00_set_field8(&rfcsr, RFCSR36_RF_BS, 0);
	rt2800_rfcsr_write(rt2x00dev, 36, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 34);
	if (rf->channel <= 14)
		rfcsr = 0x3c;
	else
		rfcsr = 0x20;
	rt2800_rfcsr_write(rt2x00dev, 34, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 12);
	if (rf->channel <= 14)
		rfcsr = 0x1a;
	else
		rfcsr = 0x12;
	rt2800_rfcsr_write(rt2x00dev, 12, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 6);
	if (rf->channel >= 1 && rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR6_VCO_IC, 1);
	else if (rf->channel >= 36 && rf->channel <= 64)
		rt2x00_set_field8(&rfcsr, RFCSR6_VCO_IC, 2);
	else if (rf->channel >= 100 && rf->channel <= 128)
		rt2x00_set_field8(&rfcsr, RFCSR6_VCO_IC, 2);
	else
		rt2x00_set_field8(&rfcsr, RFCSR6_VCO_IC, 1);
	rt2800_rfcsr_write(rt2x00dev, 6, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 30);
	rt2x00_set_field8(&rfcsr, RFCSR30_RX_VCM, 2);
	rt2800_rfcsr_write(rt2x00dev, 30, rfcsr);

	rt2800_rfcsr_write(rt2x00dev, 46, 0x60);

	if (rf->channel <= 14) {
		rt2800_rfcsr_write(rt2x00dev, 10, 0xd3);
		rt2800_rfcsr_write(rt2x00dev, 13, 0x12);
	} else {
		rt2800_rfcsr_write(rt2x00dev, 10, 0xd8);
		rt2800_rfcsr_write(rt2x00dev, 13, 0x23);
	}

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 51);
	rt2x00_set_field8(&rfcsr, RFCSR51_BITS01, 1);
	rt2800_rfcsr_write(rt2x00dev, 51, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 51);
	if (rf->channel <= 14) {
		rt2x00_set_field8(&rfcsr, RFCSR51_BITS24, 5);
		rt2x00_set_field8(&rfcsr, RFCSR51_BITS57, 3);
	} else {
		rt2x00_set_field8(&rfcsr, RFCSR51_BITS24, 4);
		rt2x00_set_field8(&rfcsr, RFCSR51_BITS57, 2);
	}
	rt2800_rfcsr_write(rt2x00dev, 51, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 49);
	if (rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR49_TX_LO1_IC, 3);
	else
		rt2x00_set_field8(&rfcsr, RFCSR49_TX_LO1_IC, 2);

	if (txbf_enabled)
		rt2x00_set_field8(&rfcsr, RFCSR49_TX_DIV, 1);

	rt2800_rfcsr_write(rt2x00dev, 49, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 50);
	rt2x00_set_field8(&rfcsr, RFCSR50_TX_LO1_EN, 0);
	rt2800_rfcsr_write(rt2x00dev, 50, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 57);
	if (rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR57_DRV_CC, 0x1b);
	else
		rt2x00_set_field8(&rfcsr, RFCSR57_DRV_CC, 0x0f);
	rt2800_rfcsr_write(rt2x00dev, 57, rfcsr);

	if (rf->channel <= 14) {
		rt2800_rfcsr_write(rt2x00dev, 44, 0x93);
		rt2800_rfcsr_write(rt2x00dev, 52, 0x45);
	} else {
		rt2800_rfcsr_write(rt2x00dev, 44, 0x9b);
		rt2800_rfcsr_write(rt2x00dev, 52, 0x05);
	}

	/* Initiate VCO calibration */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 3);
	if (rf->channel <= 14) {
		rt2x00_set_field8(&rfcsr, RFCSR3_VCOCAL_EN, 1);
	} else {
		rt2x00_set_field8(&rfcsr, RFCSR3_BIT1, 1);
		rt2x00_set_field8(&rfcsr, RFCSR3_BIT2, 1);
		rt2x00_set_field8(&rfcsr, RFCSR3_BIT3, 1);
		rt2x00_set_field8(&rfcsr, RFCSR3_BIT4, 1);
		rt2x00_set_field8(&rfcsr, RFCSR3_BIT5, 1);
		rt2x00_set_field8(&rfcsr, RFCSR3_VCOCAL_EN, 1);
	}
	rt2800_rfcsr_write(rt2x00dev, 3, rfcsr);

	if (rf->channel >= 1 && rf->channel <= 14) {
		rfcsr = 0x23;
		if (txbf_enabled)
			rt2x00_set_field8(&rfcsr, RFCSR39_RX_DIV, 1);
		rt2800_rfcsr_write(rt2x00dev, 39, rfcsr);

		rt2800_rfcsr_write(rt2x00dev, 45, 0xbb);
	} else if (rf->channel >= 36 && rf->channel <= 64) {
		rfcsr = 0x36;
		if (txbf_enabled)
			rt2x00_set_field8(&rfcsr, RFCSR39_RX_DIV, 1);
		rt2800_rfcsr_write(rt2x00dev, 39, 0x36);

		rt2800_rfcsr_write(rt2x00dev, 45, 0xeb);
	} else if (rf->channel >= 100 && rf->channel <= 128) {
		rfcsr = 0x32;
		if (txbf_enabled)
			rt2x00_set_field8(&rfcsr, RFCSR39_RX_DIV, 1);
		rt2800_rfcsr_write(rt2x00dev, 39, rfcsr);

		rt2800_rfcsr_write(rt2x00dev, 45, 0xb3);
	} else {
		rfcsr = 0x30;
		if (txbf_enabled)
			rt2x00_set_field8(&rfcsr, RFCSR39_RX_DIV, 1);
		rt2800_rfcsr_write(rt2x00dev, 39, rfcsr);

		rt2800_rfcsr_write(rt2x00dev, 45, 0x9b);
	}
}

static void rt2800_config_channel_rf3853(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	uint8_t rfcsr;
	uint8_t bbp;
	uint8_t pwr1, pwr2, pwr3;

	const bool txbf_enabled = false; /* TODO */

	/* TODO: add band selection */

	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 6, 0x40);
	else if (rf->channel < 132)
		rt2800_rfcsr_write(rt2x00dev, 6, 0x80);
	else
		rt2800_rfcsr_write(rt2x00dev, 6, 0x40);

	rt2800_rfcsr_write(rt2x00dev, 8, rf->rf1);
	rt2800_rfcsr_write(rt2x00dev, 9, rf->rf3);

	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 11, 0x46);
	else
		rt2800_rfcsr_write(rt2x00dev, 11, 0x48);

	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 12, 0x1a);
	else
		rt2800_rfcsr_write(rt2x00dev, 12, 0x52);

	rt2800_rfcsr_write(rt2x00dev, 13, 0x12);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_RF_BLOCK_EN, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_PLL_PD, 1);

	switch (rt2x00dev->default_ant.tx_chain_num) {
	case 3:
		rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 1);
		/* fallthrough */
	case 2:
		rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 1);
		/* fallthrough */
	case 1:
		rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 1);
		break;
	}

	switch (rt2x00dev->default_ant.rx_chain_num) {
	case 3:
		rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 1);
		/* fallthrough */
	case 2:
		rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 1);
		/* fallthrough */
	case 1:
		rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 1);
		break;
	}
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rt2800_freq_cal_mode1(rt2x00dev);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 30);
	if (!conf_is_ht40(conf))
		rfcsr &= ~(0x06);
	else
		rfcsr |= 0x06;
	rt2800_rfcsr_write(rt2x00dev, 30, rfcsr);

	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 31, 0xa0);
	else
		rt2800_rfcsr_write(rt2x00dev, 31, 0x80);

	if (conf_is_ht40(conf))
		rt2800_rfcsr_write(rt2x00dev, 32, 0x80);
	else
		rt2800_rfcsr_write(rt2x00dev, 32, 0xd8);

	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 34, 0x3c);
	else
		rt2800_rfcsr_write(rt2x00dev, 34, 0x20);

	/* loopback RF_BS */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 36);
	if (rf->channel <= 14)
		rt2x00_set_field8(&rfcsr, RFCSR36_RF_BS, 1);
	else
		rt2x00_set_field8(&rfcsr, RFCSR36_RF_BS, 0);
	rt2800_rfcsr_write(rt2x00dev, 36, rfcsr);

	if (rf->channel <= 14)
		rfcsr = 0x23;
	else if (rf->channel < 100)
		rfcsr = 0x36;
	else if (rf->channel < 132)
		rfcsr = 0x32;
	else
		rfcsr = 0x30;

	if (txbf_enabled)
		rfcsr |= 0x40;

	rt2800_rfcsr_write(rt2x00dev, 39, rfcsr);

	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 44, 0x93);
	else
		rt2800_rfcsr_write(rt2x00dev, 44, 0x9b);

	if (rf->channel <= 14)
		rfcsr = 0xbb;
	else if (rf->channel < 100)
		rfcsr = 0xeb;
	else if (rf->channel < 132)
		rfcsr = 0xb3;
	else
		rfcsr = 0x9b;
	rt2800_rfcsr_write(rt2x00dev, 45, rfcsr);

	if (rf->channel <= 14)
		rfcsr = 0x8e;
	else
		rfcsr = 0x8a;

	if (txbf_enabled)
		rfcsr |= 0x20;

	rt2800_rfcsr_write(rt2x00dev, 49, rfcsr);

	rt2800_rfcsr_write(rt2x00dev, 50, 0x86);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 51);
	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 51, 0x75);
	else
		rt2800_rfcsr_write(rt2x00dev, 51, 0x51);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 52);
	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 52, 0x45);
	else
		rt2800_rfcsr_write(rt2x00dev, 52, 0x05);

	if (rf->channel <= 14) {
		pwr1 = info->default_power1 & 0x1f;
		pwr2 = info->default_power2 & 0x1f;
		pwr3 = info->default_power3 & 0x1f;
	} else {
		pwr1 = 0x48 | ((info->default_power1 & 0x18) << 1) |
			(info->default_power1 & 0x7);
		pwr2 = 0x48 | ((info->default_power2 & 0x18) << 1) |
			(info->default_power2 & 0x7);
		pwr3 = 0x48 | ((info->default_power3 & 0x18) << 1) |
			(info->default_power3 & 0x7);
	}

	rt2800_rfcsr_write(rt2x00dev, 53, pwr1);
	rt2800_rfcsr_write(rt2x00dev, 54, pwr2);
	rt2800_rfcsr_write(rt2x00dev, 55, pwr3);

	rt2x00_dbg(rt2x00dev, "Channel:%d, pwr1:%02x, pwr2:%02x, pwr3:%02x\n",
		   rf->channel, pwr1, pwr2, pwr3);

	bbp = (info->default_power1 >> 5) |
	      ((info->default_power2 & 0xe0) >> 1);
	rt2800_bbp_write(rt2x00dev, 109, bbp);

	bbp = rt2800_bbp_read(rt2x00dev, 110);
	bbp &= 0x0f;
	bbp |= (info->default_power3 & 0xe0) >> 1;
	rt2800_bbp_write(rt2x00dev, 110, bbp);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 57);
	if (rf->channel <= 14)
		rt2800_rfcsr_write(rt2x00dev, 57, 0x6e);
	else
		rt2800_rfcsr_write(rt2x00dev, 57, 0x3e);

	/* Enable RF tuning */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 3);
	rt2x00_set_field8(&rfcsr, RFCSR3_VCOCAL_EN, 1);
	rt2800_rfcsr_write(rt2x00dev, 3, rfcsr);

	udelay(2000);

	bbp = rt2800_bbp_read(rt2x00dev, 49);
	/* clear update flag */
	rt2800_bbp_write(rt2x00dev, 49, bbp & 0xfe);
	rt2800_bbp_write(rt2x00dev, 49, bbp);

	/* TODO: add calibration for TxBF */
}

#define POWER_BOUND		0x27
#define POWER_BOUND_5G		0x2b

static void rt2800_config_channel_rf3290(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	uint8_t rfcsr;

	rt2800_rfcsr_write(rt2x00dev, 8, rf->rf1);
	rt2800_rfcsr_write(rt2x00dev, 9, rf->rf3);
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 11);
	rt2x00_set_field8(&rfcsr, RFCSR11_R, rf->rf2);
	rt2800_rfcsr_write(rt2x00dev, 11, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 49);
	if (info->default_power1 > POWER_BOUND)
		rt2x00_set_field8(&rfcsr, RFCSR49_TX, POWER_BOUND);
	else
		rt2x00_set_field8(&rfcsr, RFCSR49_TX, info->default_power1);
	rt2800_rfcsr_write(rt2x00dev, 49, rfcsr);

	rt2800_freq_cal_mode1(rt2x00dev);

	if (rf->channel <= 14) {
		if (rf->channel == 6)
			rt2800_bbp_write(rt2x00dev, 68, 0x0c);
		else
			rt2800_bbp_write(rt2x00dev, 68, 0x0b);

		if (rf->channel >= 1 && rf->channel <= 6)
			rt2800_bbp_write(rt2x00dev, 59, 0x0f);
		else if (rf->channel >= 7 && rf->channel <= 11)
			rt2800_bbp_write(rt2x00dev, 59, 0x0e);
		else if (rf->channel >= 12 && rf->channel <= 14)
			rt2800_bbp_write(rt2x00dev, 59, 0x0d);
	}
}

static void rt2800_config_channel_rf3322(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	uint8_t rfcsr;

	rt2800_rfcsr_write(rt2x00dev, 8, rf->rf1);
	rt2800_rfcsr_write(rt2x00dev, 9, rf->rf3);

	rt2800_rfcsr_write(rt2x00dev, 11, 0x42);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x1c);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x00);

	if (info->default_power1 > POWER_BOUND)
		rt2800_rfcsr_write(rt2x00dev, 47, POWER_BOUND);
	else
		rt2800_rfcsr_write(rt2x00dev, 47, info->default_power1);

	if (info->default_power2 > POWER_BOUND)
		rt2800_rfcsr_write(rt2x00dev, 48, POWER_BOUND);
	else
		rt2800_rfcsr_write(rt2x00dev, 48, info->default_power2);

	rt2800_freq_cal_mode1(rt2x00dev);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 1);

	if ( rt2x00dev->default_ant.tx_chain_num == 2 )
		rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 1);
	else
		rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 0);

	if ( rt2x00dev->default_ant.rx_chain_num == 2 )
		rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 1);
	else
		rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 0);

	rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 0);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 0);

	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rt2800_rfcsr_write(rt2x00dev, 31, 80);
}

static void rt2800_config_channel_rf53xx(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	uint8_t rfcsr;
	int idx = rf->channel-1;

	rt2800_rfcsr_write(rt2x00dev, 8, rf->rf1);
	rt2800_rfcsr_write(rt2x00dev, 9, rf->rf3);
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 11);
	rt2x00_set_field8(&rfcsr, RFCSR11_R, rf->rf2);
	rt2800_rfcsr_write(rt2x00dev, 11, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 49);
	if (info->default_power1 > POWER_BOUND)
		rt2x00_set_field8(&rfcsr, RFCSR49_TX, POWER_BOUND);
	else
		rt2x00_set_field8(&rfcsr, RFCSR49_TX, info->default_power1);
	rt2800_rfcsr_write(rt2x00dev, 49, rfcsr);

	if (rt2x00_rt(rt2x00dev, RT5392)) {
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 50);
		if (info->default_power2 > POWER_BOUND)
			rt2x00_set_field8(&rfcsr, RFCSR50_TX, POWER_BOUND);
		else
			rt2x00_set_field8(&rfcsr, RFCSR50_TX,
					  info->default_power2);
		rt2800_rfcsr_write(rt2x00dev, 50, rfcsr);
	}

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	if (rt2x00_rt(rt2x00dev, RT5392)) {
		rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 1);
		rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 1);
	}
	rt2x00_set_field8(&rfcsr, RFCSR1_RF_BLOCK_EN, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_PLL_PD, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 1);
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rt2800_freq_cal_mode1(rt2x00dev);

	if (rt2x00_has_cap_bt_coexist(rt2x00dev)) {
		if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F)) {
			/* r55/r59 value array of channel 1~14 */
			static const char r55_bt_rev[] = {0x83, 0x83,
				0x83, 0x73, 0x73, 0x63, 0x53, 0x53,
				0x53, 0x43, 0x43, 0x43, 0x43, 0x43};
			static const char r59_bt_rev[] = {0x0e, 0x0e,
				0x0e, 0x0e, 0x0e, 0x0b, 0x0a, 0x09,
				0x07, 0x07, 0x07, 0x07, 0x07, 0x07};

			rt2800_rfcsr_write(rt2x00dev, 55,
					   r55_bt_rev[idx]);
			rt2800_rfcsr_write(rt2x00dev, 59,
					   r59_bt_rev[idx]);
		} else {
			static const char r59_bt[] = {0x8b, 0x8b, 0x8b,
				0x8b, 0x8b, 0x8b, 0x8b, 0x8a, 0x89,
				0x88, 0x88, 0x86, 0x85, 0x84};

			rt2800_rfcsr_write(rt2x00dev, 59, r59_bt[idx]);
		}
	} else {
		if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F)) {
			static const char r55_nonbt_rev[] = {0x23, 0x23,
				0x23, 0x23, 0x13, 0x13, 0x03, 0x03,
				0x03, 0x03, 0x03, 0x03, 0x03, 0x03};
			static const char r59_nonbt_rev[] = {0x07, 0x07,
				0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
				0x07, 0x07, 0x06, 0x05, 0x04, 0x04};

			rt2800_rfcsr_write(rt2x00dev, 55,
					   r55_nonbt_rev[idx]);
			rt2800_rfcsr_write(rt2x00dev, 59,
					   r59_nonbt_rev[idx]);
		} else if (rt2x00_rt(rt2x00dev, RT5390) ||
			   rt2x00_rt(rt2x00dev, RT5392) ||
			   rt2x00_rt(rt2x00dev, RT6352)) {
			static const char r59_non_bt[] = {0x8f, 0x8f,
				0x8f, 0x8f, 0x8f, 0x8f, 0x8f, 0x8d,
				0x8a, 0x88, 0x88, 0x87, 0x87, 0x86};

			rt2800_rfcsr_write(rt2x00dev, 59,
					   r59_non_bt[idx]);
		} else if (rt2x00_rt(rt2x00dev, RT5350)) {
			static const char r59_non_bt[] = {0x0b, 0x0b,
				0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0a,
				0x0a, 0x09, 0x08, 0x07, 0x07, 0x06};

			rt2800_rfcsr_write(rt2x00dev, 59,
					   r59_non_bt[idx]);
		}
	}
}

static void rt2800_config_channel_rf55xx(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	uint8_t rfcsr, ep_reg;
	uint32_t reg;
	int power_bound;

    rt2x00_info(rt2x00dev, "channel_rf55xx config channel %u\n", rf->channel);

	/* TODO */
	const bool is_11b = false;
	const bool is_type_ep = false;

	reg = rt2800_register_read(rt2x00dev, LDO_CFG0);
	rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL,
			   (rf->channel > 14 || conf_is_ht40(conf)) ? 5 : 0);
	rt2800_register_write(rt2x00dev, LDO_CFG0, reg);

	/* Order of values on rf_channel entry: N, K, mod, R */
	rt2800_rfcsr_write(rt2x00dev, 8, rf->rf1 & 0xff);

	rfcsr = rt2800_rfcsr_read(rt2x00dev,  9);
	rt2x00_set_field8(&rfcsr, RFCSR9_K, rf->rf2 & 0xf);
	rt2x00_set_field8(&rfcsr, RFCSR9_N, (rf->rf1 & 0x100) >> 8);
	rt2x00_set_field8(&rfcsr, RFCSR9_MOD, ((rf->rf3 - 8) & 0x4) >> 2);
	rt2800_rfcsr_write(rt2x00dev, 9, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 11);
	rt2x00_set_field8(&rfcsr, RFCSR11_R, rf->rf4 - 1);
	rt2x00_set_field8(&rfcsr, RFCSR11_MOD, (rf->rf3 - 8) & 0x3);
	rt2800_rfcsr_write(rt2x00dev, 11, rfcsr);

	if (rf->channel <= 14) {
		rt2800_rfcsr_write(rt2x00dev, 10, 0x90);
		/* FIXME: RF11 owerwrite ? */
		rt2800_rfcsr_write(rt2x00dev, 11, 0x4A);
		rt2800_rfcsr_write(rt2x00dev, 12, 0x52);
		rt2800_rfcsr_write(rt2x00dev, 13, 0x42);
		rt2800_rfcsr_write(rt2x00dev, 22, 0x40);
		rt2800_rfcsr_write(rt2x00dev, 24, 0x4A);
		rt2800_rfcsr_write(rt2x00dev, 25, 0x80);
		rt2800_rfcsr_write(rt2x00dev, 27, 0x42);
		rt2800_rfcsr_write(rt2x00dev, 36, 0x80);
		rt2800_rfcsr_write(rt2x00dev, 37, 0x08);
		rt2800_rfcsr_write(rt2x00dev, 38, 0x89);
		rt2800_rfcsr_write(rt2x00dev, 39, 0x1B);
		rt2800_rfcsr_write(rt2x00dev, 40, 0x0D);
		rt2800_rfcsr_write(rt2x00dev, 41, 0x9B);
		rt2800_rfcsr_write(rt2x00dev, 42, 0xD5);
		rt2800_rfcsr_write(rt2x00dev, 43, 0x72);
		rt2800_rfcsr_write(rt2x00dev, 44, 0x0E);
		rt2800_rfcsr_write(rt2x00dev, 45, 0xA2);
		rt2800_rfcsr_write(rt2x00dev, 46, 0x6B);
		rt2800_rfcsr_write(rt2x00dev, 48, 0x10);
		rt2800_rfcsr_write(rt2x00dev, 51, 0x3E);
		rt2800_rfcsr_write(rt2x00dev, 52, 0x48);
		rt2800_rfcsr_write(rt2x00dev, 54, 0x38);
		rt2800_rfcsr_write(rt2x00dev, 56, 0xA1);
		rt2800_rfcsr_write(rt2x00dev, 57, 0x00);
		rt2800_rfcsr_write(rt2x00dev, 58, 0x39);
		rt2800_rfcsr_write(rt2x00dev, 60, 0x45);
		rt2800_rfcsr_write(rt2x00dev, 61, 0x91);
		rt2800_rfcsr_write(rt2x00dev, 62, 0x39);

		/* TODO RF27 <- tssi */

		rfcsr = rf->channel <= 10 ? 0x07 : 0x06;
		rt2800_rfcsr_write(rt2x00dev, 23, rfcsr);
		rt2800_rfcsr_write(rt2x00dev, 59, rfcsr);

		if (is_11b) {
			/* CCK */
			rt2800_rfcsr_write(rt2x00dev, 31, 0xF8);
			rt2800_rfcsr_write(rt2x00dev, 32, 0xC0);
			if (is_type_ep)
				rt2800_rfcsr_write(rt2x00dev, 55, 0x06);
			else
				rt2800_rfcsr_write(rt2x00dev, 55, 0x47);
		} else {
			/* OFDM */
			if (is_type_ep)
				rt2800_rfcsr_write(rt2x00dev, 55, 0x03);
			else
				rt2800_rfcsr_write(rt2x00dev, 55, 0x43);
		}

		power_bound = POWER_BOUND;
		ep_reg = 0x2;
	} else {
		rt2800_rfcsr_write(rt2x00dev, 10, 0x97);
		/* FIMXE: RF11 overwrite */
		rt2800_rfcsr_write(rt2x00dev, 11, 0x40);
		rt2800_rfcsr_write(rt2x00dev, 25, 0xBF);
		rt2800_rfcsr_write(rt2x00dev, 27, 0x42);
		rt2800_rfcsr_write(rt2x00dev, 36, 0x00);
		rt2800_rfcsr_write(rt2x00dev, 37, 0x04);
		rt2800_rfcsr_write(rt2x00dev, 38, 0x85);
		rt2800_rfcsr_write(rt2x00dev, 40, 0x42);
		rt2800_rfcsr_write(rt2x00dev, 41, 0xBB);
		rt2800_rfcsr_write(rt2x00dev, 42, 0xD7);
		rt2800_rfcsr_write(rt2x00dev, 45, 0x41);
		rt2800_rfcsr_write(rt2x00dev, 48, 0x00);
		rt2800_rfcsr_write(rt2x00dev, 57, 0x77);
		rt2800_rfcsr_write(rt2x00dev, 60, 0x05);
		rt2800_rfcsr_write(rt2x00dev, 61, 0x01);

		/* TODO RF27 <- tssi */

		if (rf->channel >= 36 && rf->channel <= 64) {

			rt2800_rfcsr_write(rt2x00dev, 12, 0x2E);
			rt2800_rfcsr_write(rt2x00dev, 13, 0x22);
			rt2800_rfcsr_write(rt2x00dev, 22, 0x60);
			rt2800_rfcsr_write(rt2x00dev, 23, 0x7F);
			if (rf->channel <= 50)
				rt2800_rfcsr_write(rt2x00dev, 24, 0x09);
			else if (rf->channel >= 52)
				rt2800_rfcsr_write(rt2x00dev, 24, 0x07);
			rt2800_rfcsr_write(rt2x00dev, 39, 0x1C);
			rt2800_rfcsr_write(rt2x00dev, 43, 0x5B);
			rt2800_rfcsr_write(rt2x00dev, 44, 0X40);
			rt2800_rfcsr_write(rt2x00dev, 46, 0X00);
			rt2800_rfcsr_write(rt2x00dev, 51, 0xFE);
			rt2800_rfcsr_write(rt2x00dev, 52, 0x0C);
			rt2800_rfcsr_write(rt2x00dev, 54, 0xF8);
			if (rf->channel <= 50) {
				rt2800_rfcsr_write(rt2x00dev, 55, 0x06),
				rt2800_rfcsr_write(rt2x00dev, 56, 0xD3);
			} else if (rf->channel >= 52) {
				rt2800_rfcsr_write(rt2x00dev, 55, 0x04);
				rt2800_rfcsr_write(rt2x00dev, 56, 0xBB);
			}

			rt2800_rfcsr_write(rt2x00dev, 58, 0x15);
			rt2800_rfcsr_write(rt2x00dev, 59, 0x7F);
			rt2800_rfcsr_write(rt2x00dev, 62, 0x15);

		} else if (rf->channel >= 100 && rf->channel <= 165) {

			rt2800_rfcsr_write(rt2x00dev, 12, 0x0E);
			rt2800_rfcsr_write(rt2x00dev, 13, 0x42);
			rt2800_rfcsr_write(rt2x00dev, 22, 0x40);
			if (rf->channel <= 153) {
				rt2800_rfcsr_write(rt2x00dev, 23, 0x3C);
				rt2800_rfcsr_write(rt2x00dev, 24, 0x06);
			} else if (rf->channel >= 155) {
				rt2800_rfcsr_write(rt2x00dev, 23, 0x38);
				rt2800_rfcsr_write(rt2x00dev, 24, 0x05);
			}
			if (rf->channel <= 138) {
				rt2800_rfcsr_write(rt2x00dev, 39, 0x1A);
				rt2800_rfcsr_write(rt2x00dev, 43, 0x3B);
				rt2800_rfcsr_write(rt2x00dev, 44, 0x20);
				rt2800_rfcsr_write(rt2x00dev, 46, 0x18);
			} else if (rf->channel >= 140) {
				rt2800_rfcsr_write(rt2x00dev, 39, 0x18);
				rt2800_rfcsr_write(rt2x00dev, 43, 0x1B);
				rt2800_rfcsr_write(rt2x00dev, 44, 0x10);
				rt2800_rfcsr_write(rt2x00dev, 46, 0X08);
			}
			if (rf->channel <= 124)
				rt2800_rfcsr_write(rt2x00dev, 51, 0xFC);
			else if (rf->channel >= 126)
				rt2800_rfcsr_write(rt2x00dev, 51, 0xEC);
			if (rf->channel <= 138)
				rt2800_rfcsr_write(rt2x00dev, 52, 0x06);
			else if (rf->channel >= 140)
				rt2800_rfcsr_write(rt2x00dev, 52, 0x06);
			rt2800_rfcsr_write(rt2x00dev, 54, 0xEB);
			if (rf->channel <= 138)
				rt2800_rfcsr_write(rt2x00dev, 55, 0x01);
			else if (rf->channel >= 140)
				rt2800_rfcsr_write(rt2x00dev, 55, 0x00);
			if (rf->channel <= 128)
				rt2800_rfcsr_write(rt2x00dev, 56, 0xBB);
			else if (rf->channel >= 130)
				rt2800_rfcsr_write(rt2x00dev, 56, 0xAB);
			if (rf->channel <= 116)
				rt2800_rfcsr_write(rt2x00dev, 58, 0x1D);
			else if (rf->channel >= 118)
				rt2800_rfcsr_write(rt2x00dev, 58, 0x15);
			if (rf->channel <= 138)
				rt2800_rfcsr_write(rt2x00dev, 59, 0x3F);
			else if (rf->channel >= 140)
				rt2800_rfcsr_write(rt2x00dev, 59, 0x7C);
			if (rf->channel <= 116)
				rt2800_rfcsr_write(rt2x00dev, 62, 0x1D);
			else if (rf->channel >= 118)
				rt2800_rfcsr_write(rt2x00dev, 62, 0x15);
		}

		power_bound = POWER_BOUND_5G;
		ep_reg = 0x3;
	}

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 49);
	if (info->default_power1 > power_bound)
		rt2x00_set_field8(&rfcsr, RFCSR49_TX, power_bound);
	else
		rt2x00_set_field8(&rfcsr, RFCSR49_TX, info->default_power1);
	if (is_type_ep)
		rt2x00_set_field8(&rfcsr, RFCSR49_EP, ep_reg);
	rt2800_rfcsr_write(rt2x00dev, 49, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 50);
	if (info->default_power2 > power_bound)
		rt2x00_set_field8(&rfcsr, RFCSR50_TX, power_bound);
	else
		rt2x00_set_field8(&rfcsr, RFCSR50_TX, info->default_power2);
	if (is_type_ep)
		rt2x00_set_field8(&rfcsr, RFCSR50_EP, ep_reg);
	rt2800_rfcsr_write(rt2x00dev, 50, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RF_BLOCK_EN, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_PLL_PD, 1);

	rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD,
			  rt2x00dev->default_ant.tx_chain_num >= 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD,
			  rt2x00dev->default_ant.tx_chain_num == 2);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, 0);

	rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD,
			  rt2x00dev->default_ant.rx_chain_num >= 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD,
			  rt2x00dev->default_ant.rx_chain_num == 2);
	rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, 0);

	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);
	rt2800_rfcsr_write(rt2x00dev, 6, 0xe4);

	if (conf_is_ht40(conf))
		rt2800_rfcsr_write(rt2x00dev, 30, 0x16);
	else
		rt2800_rfcsr_write(rt2x00dev, 30, 0x10);

	if (!is_11b) {
		rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
		rt2800_rfcsr_write(rt2x00dev, 32, 0x80);
	}

	/* TODO proper frequency adjustment */
	rt2800_freq_cal_mode1(rt2x00dev);

	/* TODO merge with others */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 3);
	rt2x00_set_field8(&rfcsr, RFCSR3_VCOCAL_EN, 1);
	rt2800_rfcsr_write(rt2x00dev, 3, rfcsr);

	/* BBP settings */
	rt2800_bbp_write(rt2x00dev, 62, 0x37 - rt2x00dev->lna_gain);
	rt2800_bbp_write(rt2x00dev, 63, 0x37 - rt2x00dev->lna_gain);
	rt2800_bbp_write(rt2x00dev, 64, 0x37 - rt2x00dev->lna_gain);

	rt2800_bbp_write(rt2x00dev, 79, (rf->channel <= 14) ? 0x1C : 0x18);
	rt2800_bbp_write(rt2x00dev, 80, (rf->channel <= 14) ? 0x0E : 0x08);
	rt2800_bbp_write(rt2x00dev, 81, (rf->channel <= 14) ? 0x3A : 0x38);
	rt2800_bbp_write(rt2x00dev, 82, (rf->channel <= 14) ? 0x62 : 0x92);

	/* GLRT band configuration */
	rt2800_bbp_write(rt2x00dev, 195, 128);
	rt2800_bbp_write(rt2x00dev, 196, (rf->channel <= 14) ? 0xE0 : 0xF0);
	rt2800_bbp_write(rt2x00dev, 195, 129);
	rt2800_bbp_write(rt2x00dev, 196, (rf->channel <= 14) ? 0x1F : 0x1E);
	rt2800_bbp_write(rt2x00dev, 195, 130);
	rt2800_bbp_write(rt2x00dev, 196, (rf->channel <= 14) ? 0x38 : 0x28);
	rt2800_bbp_write(rt2x00dev, 195, 131);
	rt2800_bbp_write(rt2x00dev, 196, (rf->channel <= 14) ? 0x32 : 0x20);
	rt2800_bbp_write(rt2x00dev, 195, 133);
	rt2800_bbp_write(rt2x00dev, 196, (rf->channel <= 14) ? 0x28 : 0x7F);
	rt2800_bbp_write(rt2x00dev, 195, 124);
	rt2800_bbp_write(rt2x00dev, 196, (rf->channel <= 14) ? 0x19 : 0x7F);
}

static void rt2800_config_channel_rf7620(struct rt2x00_dev *rt2x00dev,
					 struct ieee80211_conf *conf,
					 struct rf_channel *rf,
					 struct channel_info *info)
{
	struct rt2800_drv_data *drv_data = rt2x00dev->drv_data;
	uint8_t rx_agc_fc, tx_agc_fc;
	uint8_t rfcsr;

	/* Frequeny plan setting */
	/* Rdiv setting (set 0x03 if Xtal==20)
	 * R13[1:0]
	 */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 13);
	rt2x00_set_field8(&rfcsr, RFCSR13_RDIV_MT7620,
			  rt2800_clk_is_20mhz(rt2x00dev) ? 3 : 0);
	rt2800_rfcsr_write(rt2x00dev, 13, rfcsr);

	/* N setting
	 * R20[7:0] in rf->rf1
	 * R21[0] always 0
	 */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 20);
	rfcsr = (rf->rf1 & 0x00ff);
	rt2800_rfcsr_write(rt2x00dev, 20, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 21);
	rt2x00_set_field8(&rfcsr, RFCSR21_BIT1, 0);
	rt2800_rfcsr_write(rt2x00dev, 21, rfcsr);

	/* K setting (always 0)
	 * R16[3:0] (RF PLL freq selection)
	 */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 16);
	rt2x00_set_field8(&rfcsr, RFCSR16_RF_PLL_FREQ_SEL_MT7620, 0);
	rt2800_rfcsr_write(rt2x00dev, 16, rfcsr);

	/* D setting (always 0)
	 * R22[2:0] (D=15, R22[2:0]=<111>)
	 */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 22);
	rt2x00_set_field8(&rfcsr, RFCSR22_FREQPLAN_D_MT7620, 0);
	rt2800_rfcsr_write(rt2x00dev, 22, rfcsr);

	/* Ksd setting
	 * Ksd: R17<7:0> in rf->rf2
	 *      R18<7:0> in rf->rf3
	 *      R19<1:0> in rf->rf4
	 */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 17);
	rfcsr = rf->rf2;
	rt2800_rfcsr_write(rt2x00dev, 17, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 18);
	rfcsr = rf->rf3;
	rt2800_rfcsr_write(rt2x00dev, 18, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 19);
	rt2x00_set_field8(&rfcsr, RFCSR19_K, rf->rf4);
	rt2800_rfcsr_write(rt2x00dev, 19, rfcsr);

	/* Default: XO=20MHz , SDM mode */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 16);
	rt2x00_set_field8(&rfcsr, RFCSR16_SDM_MODE_MT7620, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 16, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 21);
	rt2x00_set_field8(&rfcsr, RFCSR21_BIT8, 1);
	rt2800_rfcsr_write(rt2x00dev, 21, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_TX2_EN_MT7620,
			  rt2x00dev->default_ant.tx_chain_num != 1);
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 2);
	rt2x00_set_field8(&rfcsr, RFCSR2_TX2_EN_MT7620,
			  rt2x00dev->default_ant.tx_chain_num != 1);
	rt2x00_set_field8(&rfcsr, RFCSR2_RX2_EN_MT7620,
			  rt2x00dev->default_ant.rx_chain_num != 1);
	rt2800_rfcsr_write(rt2x00dev, 2, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 42);
	rt2x00_set_field8(&rfcsr, RFCSR42_TX2_EN_MT7620,
			  rt2x00dev->default_ant.tx_chain_num != 1);
	rt2800_rfcsr_write(rt2x00dev, 42, rfcsr);

	/* RF for DC Cal BW */
	if (conf_is_ht40(conf)) {
		rt2800_rfcsr_write_dccal(rt2x00dev, 6, 0x10);
		rt2800_rfcsr_write_dccal(rt2x00dev, 7, 0x10);
		rt2800_rfcsr_write_dccal(rt2x00dev, 8, 0x04);
		rt2800_rfcsr_write_dccal(rt2x00dev, 58, 0x10);
		rt2800_rfcsr_write_dccal(rt2x00dev, 59, 0x10);
	} else {
		rt2800_rfcsr_write_dccal(rt2x00dev, 6, 0x20);
		rt2800_rfcsr_write_dccal(rt2x00dev, 7, 0x20);
		rt2800_rfcsr_write_dccal(rt2x00dev, 8, 0x00);
		rt2800_rfcsr_write_dccal(rt2x00dev, 58, 0x20);
		rt2800_rfcsr_write_dccal(rt2x00dev, 59, 0x20);
	}

	if (conf_is_ht40(conf)) {
		rt2800_rfcsr_write_dccal(rt2x00dev, 58, 0x08);
		rt2800_rfcsr_write_dccal(rt2x00dev, 59, 0x08);
	} else {
		rt2800_rfcsr_write_dccal(rt2x00dev, 58, 0x28);
		rt2800_rfcsr_write_dccal(rt2x00dev, 59, 0x28);
	}

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 28);
	rt2x00_set_field8(&rfcsr, RFCSR28_CH11_HT40,
			  conf_is_ht40(conf) && (rf->channel == 11));
	rt2800_rfcsr_write(rt2x00dev, 28, rfcsr);

	if (!test_bit(DEVICE_STATE_SCANNING, &rt2x00dev->flags)) {
		if (conf_is_ht40(conf)) {
			rx_agc_fc = drv_data->rx_calibration_bw40;
			tx_agc_fc = drv_data->tx_calibration_bw40;
		} else {
			rx_agc_fc = drv_data->rx_calibration_bw20;
			tx_agc_fc = drv_data->tx_calibration_bw20;
		}
		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 5, 6);
		rfcsr &= (~0x3F);
		rfcsr |= rx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 6, rfcsr);
		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 5, 7);
		rfcsr &= (~0x3F);
		rfcsr |= rx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 7, rfcsr);
		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 7, 6);
		rfcsr &= (~0x3F);
		rfcsr |= rx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 7, 6, rfcsr);
		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 7, 7);
		rfcsr &= (~0x3F);
		rfcsr |= rx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 7, 7, rfcsr);

		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 5, 58);
		rfcsr &= (~0x3F);
		rfcsr |= tx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 58, rfcsr);
		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 5, 59);
		rfcsr &= (~0x3F);
		rfcsr |= tx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 59, rfcsr);
		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 7, 58);
		rfcsr &= (~0x3F);
		rfcsr |= tx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 7, 58, rfcsr);
		rfcsr = rt2800_rfcsr_read_bank(rt2x00dev, 7, 59);
		rfcsr &= (~0x3F);
		rfcsr |= tx_agc_fc;
		rt2800_rfcsr_write_bank(rt2x00dev, 7, 59, rfcsr);
	}
}

#if 0
static void rt2800_config_alc(struct rt2x00_dev *rt2x00dev,
			      struct ieee80211_channel *chan,
			      int power_level) {
	uint16_t eeprom, target_power, max_power;
	uint32_t mac_sys_ctrl, mac_status;
	uint32_t reg;
	uint8_t bbp;
	int i;

	/* hardware unit is 0.5dBm, limited to 23.5dBm */
	power_level *= 2;
	if (power_level > 0x2f)
		power_level = 0x2f;

	max_power = chan->max_power * 2;
	if (max_power > 0x2f)
		max_power = 0x2f;

	reg = rt2800_register_read(rt2x00dev, TX_ALC_CFG_0);
	rt2x00_set_field32(&reg, TX_ALC_CFG_0_CH_INIT_0, power_level);
	rt2x00_set_field32(&reg, TX_ALC_CFG_0_CH_INIT_1, power_level);
	rt2x00_set_field32(&reg, TX_ALC_CFG_0_LIMIT_0, max_power);
	rt2x00_set_field32(&reg, TX_ALC_CFG_0_LIMIT_1, max_power);

	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_INTERNAL_TX_ALC)) {
		/* init base power by eeprom target power */
		target_power = rt2800_eeprom_read(rt2x00dev,
						  EEPROM_TXPOWER_INIT);
		rt2x00_set_field32(&reg, TX_ALC_CFG_0_CH_INIT_0, target_power);
		rt2x00_set_field32(&reg, TX_ALC_CFG_0_CH_INIT_1, target_power);
	}
	rt2800_register_write(rt2x00dev, TX_ALC_CFG_0, reg);

	reg = rt2800_register_read(rt2x00dev, TX_ALC_CFG_1);
	rt2x00_set_field32(&reg, TX_ALC_CFG_1_TX_TEMP_COMP, 0);
	rt2800_register_write(rt2x00dev, TX_ALC_CFG_1, reg);

	/* Save MAC SYS CTRL registers */
	mac_sys_ctrl = rt2800_register_read(rt2x00dev, MAC_SYS_CTRL);
	/* Disable Tx/Rx */
	rt2800_register_write(rt2x00dev, MAC_SYS_CTRL, 0);
	/* Check MAC Tx/Rx idle */
	for (i = 0; i < 10000; i++) {
		mac_status = rt2800_register_read(rt2x00dev, MAC_STATUS_CFG);
		if (mac_status & 0x3)
			usleep_range(50, 200);
		else
			break;
	}

	if (i == 10000)
		rt2x00_warn(rt2x00dev, "Wait MAC Status to MAX !!!\n");

	if (chan->center_freq > 2457) {
		bbp = rt2800_bbp_read(rt2x00dev, 30);
		bbp = 0x40;
		rt2800_bbp_write(rt2x00dev, 30, bbp);
		rt2800_rfcsr_write(rt2x00dev, 39, 0);
		if (rt2x00_has_cap_external_lna_bg(rt2x00dev))
			rt2800_rfcsr_write(rt2x00dev, 42, 0xfb);
		else
			rt2800_rfcsr_write(rt2x00dev, 42, 0x7b);
	} else {
		bbp = rt2800_bbp_read(rt2x00dev, 30);
		bbp = 0x1f;
		rt2800_bbp_write(rt2x00dev, 30, bbp);
		rt2800_rfcsr_write(rt2x00dev, 39, 0x80);
		if (rt2x00_has_cap_external_lna_bg(rt2x00dev))
			rt2800_rfcsr_write(rt2x00dev, 42, 0xdb);
		else
			rt2800_rfcsr_write(rt2x00dev, 42, 0x5b);
	}
	rt2800_register_write(rt2x00dev, MAC_SYS_CTRL, mac_sys_ctrl);

	rt2800_vco_calibration(rt2x00dev);
}
#endif

/*
 * Link tuning
 */
void rt2800_link_stats(struct rt2x00_dev *rt2x00dev, struct link_qual *qual)
{
	uint32_t reg;

	/*
	 * Update FCS error count from register.
	 */
	reg = rt2800_register_read(rt2x00dev, RX_STA_CNT0);
	qual->rx_failed = rt2x00_get_field32(reg, RX_STA_CNT0_CRC_ERR);
}
EXPORT_SYMBOL_GPL(rt2800_link_stats);

static uint8_t rt2800_get_default_vgc(struct rt2x00_dev *rt2x00dev)
{
	uint8_t vgc;

	if (rt2x00dev->curr_band == NL80211_BAND_2GHZ) {
		if (rt2x00_rt(rt2x00dev, RT3070) ||
		    rt2x00_rt(rt2x00dev, RT3071) ||
		    rt2x00_rt(rt2x00dev, RT3090) ||
		    rt2x00_rt(rt2x00dev, RT3290) ||
		    rt2x00_rt(rt2x00dev, RT3390) ||
		    rt2x00_rt(rt2x00dev, RT3572) ||
		    rt2x00_rt(rt2x00dev, RT3593) ||
		    rt2x00_rt(rt2x00dev, RT5390) ||
		    rt2x00_rt(rt2x00dev, RT5392) ||
		    rt2x00_rt(rt2x00dev, RT5592) ||
		    rt2x00_rt(rt2x00dev, RT6352))
			vgc = 0x1c + (2 * rt2x00dev->lna_gain);
		else
			vgc = 0x2e + rt2x00dev->lna_gain;
	} else { /* 5GHZ band */
		if (rt2x00_rt(rt2x00dev, RT3593) ||
		    rt2x00_rt(rt2x00dev, RT3883))
			vgc = 0x20 + (rt2x00dev->lna_gain * 5) / 3;
		else if (rt2x00_rt(rt2x00dev, RT5592))
			vgc = 0x24 + (2 * rt2x00dev->lna_gain);
		else {
			if (!test_bit(CONFIG_CHANNEL_HT40, &rt2x00dev->flags))
				vgc = 0x32 + (rt2x00dev->lna_gain * 5) / 3;
			else
				vgc = 0x3a + (rt2x00dev->lna_gain * 5) / 3;
		}
	}

	return vgc;
}

static inline void rt2800_set_vgc(struct rt2x00_dev *rt2x00dev,
				  struct link_qual *qual, uint8_t vgc_level)
{
	if (qual->vgc_level != vgc_level) {
		if (rt2x00_rt(rt2x00dev, RT3572) ||
		    rt2x00_rt(rt2x00dev, RT3593) ||
		    rt2x00_rt(rt2x00dev, RT3883)) {
			rt2800_bbp_write_with_rx_chain(rt2x00dev, 66,
						       vgc_level);
		} else if (rt2x00_rt(rt2x00dev, RT5592)) {
			rt2800_bbp_write(rt2x00dev, 83, qual->rssi > -65 ? 0x4a : 0x7a);
			rt2800_bbp_write_with_rx_chain(rt2x00dev, 66, vgc_level);
		} else {
			rt2800_bbp_write(rt2x00dev, 66, vgc_level);
		}

		qual->vgc_level = vgc_level;
		qual->vgc_level_reg = vgc_level;
	}
}

void rt2800_reset_tuner(struct rt2x00_dev *rt2x00dev, struct link_qual *qual)
{
	rt2800_set_vgc(rt2x00dev, qual, rt2800_get_default_vgc(rt2x00dev));
}
EXPORT_SYMBOL_GPL(rt2800_reset_tuner);

void rt2800_link_tuner(struct rt2x00_dev *rt2x00dev, struct link_qual *qual,
		       const uint32_t count)
{
	uint8_t vgc;

	if (rt2x00_rt_rev(rt2x00dev, RT2860, REV_RT2860C))
		return;

	/* When RSSI is better than a certain threshold, increase VGC
	 * with a chip specific value in order to improve the balance
	 * between sensibility and noise isolation.
	 */

	vgc = rt2800_get_default_vgc(rt2x00dev);

	switch (rt2x00dev->chip.rt) {
	case RT3572:
	case RT3593:
		if (qual->rssi > -65) {
			if (rt2x00dev->curr_band == NL80211_BAND_2GHZ)
				vgc += 0x20;
			else
				vgc += 0x10;
		}
		break;

	case RT3883:
		if (qual->rssi > -65)
			vgc += 0x10;
		break;

	case RT5592:
		if (qual->rssi > -65)
			vgc += 0x20;
		break;

	default:
		if (qual->rssi > -80)
			vgc += 0x10;
		break;
	}

	rt2800_set_vgc(rt2x00dev, qual, vgc);
}
EXPORT_SYMBOL_GPL(rt2800_link_tuner);

/*
 * Initialization functions.
 */
int rt2800_init_registers(struct rt2x00_dev *rt2x00dev) {
    struct rt2800_drv_data *drv_data = (struct rt2800_drv_data *) rt2x00dev->drv_data;
    uint32_t reg;
    uint16_t eeprom;
    unsigned int i;
    int ret;

    rt2800_disable_wpdma(rt2x00dev);

    ret = rt2800_drv_init_registers(rt2x00dev);
    if (ret)
        return ret;

    rt2800_register_write(rt2x00dev, LEGACY_BASIC_RATE, 0x0000013f);
    rt2800_register_write(rt2x00dev, HT_BASIC_RATE, 0x00008003);

    rt2800_register_write(rt2x00dev, MAC_SYS_CTRL, 0x00000000);

    reg = rt2800_register_read(rt2x00dev, BCN_TIME_CFG);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_BEACON_INTERVAL, 1600);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TSF_TICKING, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TSF_SYNC, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TBTT_ENABLE, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_BEACON_GEN, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TX_TIME_COMPENSATE, 0);
    rt2800_register_write(rt2x00dev, BCN_TIME_CFG, reg);

    /* rt2800_config_filter(rt2x00dev, FIF_ALLMULTI); */
    /* All flags except FIF_FCSFAIL and FIF_PLCPFAIL */
    rt2800_config_filter(rt2x00dev,
            FIF_ALLMULTI | FIF_FCSFAIL | FIF_CONTROL | FIF_PSPOLL | FIF_OTHER_BSS);

    reg = rt2800_register_read(rt2x00dev, BKOFF_SLOT_CFG);
    rt2x00_set_field32(&reg, BKOFF_SLOT_CFG_SLOT_TIME, 9);
    rt2x00_set_field32(&reg, BKOFF_SLOT_CFG_CC_DELAY_TIME, 2);
    rt2800_register_write(rt2x00dev, BKOFF_SLOT_CFG, reg);

    if (rt2x00_rt(rt2x00dev, RT3290)) {
        reg = rt2800_register_read(rt2x00dev, WLAN_FUN_CTRL);
        if (rt2x00_get_field32(reg, WLAN_EN) == 1) {
            rt2x00_set_field32(&reg, PCIE_APP0_CLK_REQ, 1);
            rt2800_register_write(rt2x00dev, WLAN_FUN_CTRL, reg);
        }

        reg = rt2800_register_read(rt2x00dev, CMB_CTRL);
        if (!(rt2x00_get_field32(reg, LDO0_EN) == 1)) {
            rt2x00_set_field32(&reg, LDO0_EN, 1);
            rt2x00_set_field32(&reg, LDO_BGSEL, 3);
            rt2800_register_write(rt2x00dev, CMB_CTRL, reg);
        }

        reg = rt2800_register_read(rt2x00dev, OSC_CTRL);
        rt2x00_set_field32(&reg, OSC_ROSC_EN, 1);
        rt2x00_set_field32(&reg, OSC_CAL_REQ, 1);
        rt2x00_set_field32(&reg, OSC_REF_CYCLE, 0x27);
        rt2800_register_write(rt2x00dev, OSC_CTRL, reg);

        reg = rt2800_register_read(rt2x00dev, COEX_CFG0);
        rt2x00_set_field32(&reg, COEX_CFG_ANT, 0x5e);
        rt2800_register_write(rt2x00dev, COEX_CFG0, reg);

        reg = rt2800_register_read(rt2x00dev, COEX_CFG2);
        rt2x00_set_field32(&reg, BT_COEX_CFG1, 0x00);
        rt2x00_set_field32(&reg, BT_COEX_CFG0, 0x17);
        rt2x00_set_field32(&reg, WL_COEX_CFG1, 0x93);
        rt2x00_set_field32(&reg, WL_COEX_CFG0, 0x7f);
        rt2800_register_write(rt2x00dev, COEX_CFG2, reg);

        reg = rt2800_register_read(rt2x00dev, PLL_CTRL);
        rt2x00_set_field32(&reg, PLL_CONTROL, 1);
        rt2800_register_write(rt2x00dev, PLL_CTRL, reg);
    }

    if (rt2x00_rt(rt2x00dev, RT3071) ||
            rt2x00_rt(rt2x00dev, RT3090) ||
            rt2x00_rt(rt2x00dev, RT3290) ||
            rt2x00_rt(rt2x00dev, RT3390)) {

        if (rt2x00_rt(rt2x00dev, RT3290))
            rt2800_register_write(rt2x00dev, TX_SW_CFG0,
                    0x00000404);
        else
            rt2800_register_write(rt2x00dev, TX_SW_CFG0,
                    0x00000400);

        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00000000);
        if (rt2x00_rt_rev_lt(rt2x00dev, RT3071, REV_RT3071E) ||
                rt2x00_rt_rev_lt(rt2x00dev, RT3090, REV_RT3090E) ||
                rt2x00_rt_rev_lt(rt2x00dev, RT3390, REV_RT3390E)) {
            eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);
            if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_DAC_TEST))
                rt2800_register_write(rt2x00dev, TX_SW_CFG2,
                        0x0000002c);
            else
                rt2800_register_write(rt2x00dev, TX_SW_CFG2,
                        0x0000000f);
        } else {
            rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00000000);
        }
    } else if (rt2x00_rt(rt2x00dev, RT3070)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000400);

        if (rt2x00_rt_rev_lt(rt2x00dev, RT3070, REV_RT3070F)) {
            rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00000000);
            rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x0000002c);
        } else {
            rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00080606);
            rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00000000);
        }
    } else if (rt2800_is_305x_soc(rt2x00dev)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000400);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00000000);
        rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00000030);
    } else if (rt2x00_rt(rt2x00dev, RT3352)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000402);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00080606);
        rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00000000);
    } else if (rt2x00_rt(rt2x00dev, RT3572)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000400);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00080606);
    } else if (rt2x00_rt(rt2x00dev, RT3593)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000402);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00000000);
        if (rt2x00_rt_rev_lt(rt2x00dev, RT3593, REV_RT3593E)) {
            eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);
            if (rt2x00_get_field16(eeprom,
                        EEPROM_NIC_CONF1_DAC_TEST))
                rt2800_register_write(rt2x00dev, TX_SW_CFG2,
                        0x0000001f);
            else
                rt2800_register_write(rt2x00dev, TX_SW_CFG2,
                        0x0000000f);
        } else {
            rt2800_register_write(rt2x00dev, TX_SW_CFG2,
                    0x00000000);
        }
    } else if (rt2x00_rt(rt2x00dev, RT3883)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000402);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00000000);
        rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00040000);
        rt2800_register_write(rt2x00dev, TX_TXBF_CFG_0, 0x8000fc21);
        rt2800_register_write(rt2x00dev, TX_TXBF_CFG_3, 0x00009c40);
    } else if (rt2x00_rt(rt2x00dev, RT5390) ||
            rt2x00_rt(rt2x00dev, RT5392) ||
            rt2x00_rt(rt2x00dev, RT6352)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000404);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00080606);
        rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00000000);
    } else if (rt2x00_rt(rt2x00dev, RT5592)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000404);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00000000);
        rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00000000);
    } else if (rt2x00_rt(rt2x00dev, RT5350)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000404);
    } else if (rt2x00_rt(rt2x00dev, RT6352)) {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000401);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x000C0000);
        rt2800_register_write(rt2x00dev, TX_SW_CFG2, 0x00000000);
        rt2800_register_write(rt2x00dev, MIMO_PS_CFG, 0x00000002);
        rt2800_register_write(rt2x00dev, TX_PIN_CFG, 0x00150F0F);
        rt2800_register_write(rt2x00dev, TX_ALC_VGA3, 0x00000000);
        rt2800_register_write(rt2x00dev, TX0_BB_GAIN_ATTEN, 0x0);
        rt2800_register_write(rt2x00dev, TX1_BB_GAIN_ATTEN, 0x0);
        rt2800_register_write(rt2x00dev, TX0_RF_GAIN_ATTEN, 0x6C6C666C);
        rt2800_register_write(rt2x00dev, TX1_RF_GAIN_ATTEN, 0x6C6C666C);
        rt2800_register_write(rt2x00dev, TX0_RF_GAIN_CORRECT,
                0x3630363A);
        rt2800_register_write(rt2x00dev, TX1_RF_GAIN_CORRECT,
                0x3630363A);
        reg = rt2800_register_read(rt2x00dev, TX_ALC_CFG_1);
        rt2x00_set_field32(&reg, TX_ALC_CFG_1_ROS_BUSY_EN, 0);
        rt2800_register_write(rt2x00dev, TX_ALC_CFG_1, reg);
    } else {
        rt2800_register_write(rt2x00dev, TX_SW_CFG0, 0x00000000);
        rt2800_register_write(rt2x00dev, TX_SW_CFG1, 0x00080606);
    }

    reg = rt2800_register_read(rt2x00dev, TX_LINK_CFG);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_MFB_LIFETIME, 32);
    rt2x00_set_field32(&reg, TX_LINK_CFG_MFB_ENABLE, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_UMFS_ENABLE, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_TX_MRQ_EN, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_TX_RDG_EN, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_TX_CF_ACK_EN, 1);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_MFB, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_MFS, 0);
    rt2800_register_write(rt2x00dev, TX_LINK_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, TX_TIMEOUT_CFG);
    rt2x00_set_field32(&reg, TX_TIMEOUT_CFG_MPDU_LIFETIME, 9);
    rt2x00_set_field32(&reg, TX_TIMEOUT_CFG_RX_ACK_TIMEOUT, 32);
    rt2x00_set_field32(&reg, TX_TIMEOUT_CFG_TX_OP_TIMEOUT, 10);
    rt2800_register_write(rt2x00dev, TX_TIMEOUT_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, MAX_LEN_CFG);
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MAX_MPDU, AGGREGATION_SIZE);
    if (rt2x00_is_usb(rt2x00dev)) {
        drv_data->max_psdu = 3;
    } else if (rt2x00_rt_rev_gte(rt2x00dev, RT2872, REV_RT2872E) ||
            rt2x00_rt(rt2x00dev, RT2883) ||
            rt2x00_rt_rev_lt(rt2x00dev, RT3070, REV_RT3070E)) {
        drv_data->max_psdu = 2;
    } else {
        drv_data->max_psdu = 1;
    }
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MAX_PSDU, drv_data->max_psdu);
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MIN_PSDU, 10);
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MIN_MPDU, 10);
    rt2800_register_write(rt2x00dev, MAX_LEN_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, LED_CFG);
    rt2x00_set_field32(&reg, LED_CFG_ON_PERIOD, 70);
    rt2x00_set_field32(&reg, LED_CFG_OFF_PERIOD, 30);
    rt2x00_set_field32(&reg, LED_CFG_SLOW_BLINK_PERIOD, 3);
    rt2x00_set_field32(&reg, LED_CFG_R_LED_MODE, 3);
    rt2x00_set_field32(&reg, LED_CFG_G_LED_MODE, 3);
    rt2x00_set_field32(&reg, LED_CFG_Y_LED_MODE, 3);
    rt2x00_set_field32(&reg, LED_CFG_LED_POLAR, 1);
    rt2800_register_write(rt2x00dev, LED_CFG, reg);

    rt2800_register_write(rt2x00dev, PBF_MAX_PCNT, 0x1f3fbf9f);

    reg = rt2800_register_read(rt2x00dev, TX_RTY_CFG);
    rt2x00_set_field32(&reg, TX_RTY_CFG_SHORT_RTY_LIMIT, 2);
    rt2x00_set_field32(&reg, TX_RTY_CFG_LONG_RTY_LIMIT, 2);
    rt2x00_set_field32(&reg, TX_RTY_CFG_LONG_RTY_THRE, 2000);
    rt2x00_set_field32(&reg, TX_RTY_CFG_NON_AGG_RTY_MODE, 0);
    rt2x00_set_field32(&reg, TX_RTY_CFG_AGG_RTY_MODE, 0);
    rt2x00_set_field32(&reg, TX_RTY_CFG_TX_AUTO_FB_ENABLE, 1);
    rt2800_register_write(rt2x00dev, TX_RTY_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, AUTO_RSP_CFG);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_AUTORESPONDER, 1);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_BAC_ACK_POLICY, 1);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_CTS_40_MMODE, 1);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_CTS_40_MREF, 0);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_AR_PREAMBLE, 0);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_DUAL_CTS_EN, 0);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_ACK_CTS_PSM_BIT, 0);
    rt2800_register_write(rt2x00dev, AUTO_RSP_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, CCK_PROT_CFG);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_PROTECT_RATE, 3);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_PROTECT_CTRL, 0);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_CCK, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_RTS_TH_EN, 1);
    rt2800_register_write(rt2x00dev, CCK_PROT_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, OFDM_PROT_CFG);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_RATE, 3);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_CTRL, 0);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_CCK, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_RTS_TH_EN, 1);
    rt2800_register_write(rt2x00dev, OFDM_PROT_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, MM20_PROT_CFG);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_RATE, 0x4004);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt2x00dev, MM20_PROT_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, MM40_PROT_CFG);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_RATE, 0x4084);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_MM40, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_GF40, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt2x00dev, MM40_PROT_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, GF20_PROT_CFG);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_RATE, 0x4004);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt2x00dev, GF20_PROT_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, GF40_PROT_CFG);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_RATE, 0x4084);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_MM40, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_GF40, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt2x00dev, GF40_PROT_CFG, reg);

    if (rt2x00_is_usb(rt2x00dev)) {
        rt2800_register_write(rt2x00dev, PBF_CFG, 0xf40006);

        reg = rt2800_register_read(rt2x00dev, WPDMA_GLO_CFG);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_TX_DMA, 0);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_DMA_BUSY, 0);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_RX_DMA, 0);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_RX_DMA_BUSY, 0);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_WP_DMA_BURST_SIZE, 3);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_WRITEBACK_DONE, 0);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_BIG_ENDIAN, 0);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_RX_HDR_SCATTER, 0);
        rt2x00_set_field32(&reg, WPDMA_GLO_CFG_HDR_SEG_LEN, 0);
        rt2800_register_write(rt2x00dev, WPDMA_GLO_CFG, reg);
    }

    /*
     * The legacy driver also sets TXOP_CTRL_CFG_RESERVED_TRUN_EN to 1
     * although it is reserved.
     */
    reg = rt2800_register_read(rt2x00dev, TXOP_CTRL_CFG);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_TIMEOUT_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_AC_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_TXRATEGRP_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_USER_MODE_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_MIMO_PS_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_RESERVED_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_LSIG_TXOP_EN, 0);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_EXT_CCA_EN, 0);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_EXT_CCA_DLY, 88);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_EXT_CWMIN, 0);
    rt2800_register_write(rt2x00dev, TXOP_CTRL_CFG, reg);

    reg = rt2x00_rt(rt2x00dev, RT5592) ? 0x00000082 : 0x00000002;
    rt2800_register_write(rt2x00dev, TXOP_HLDR_ET, reg);

    if (rt2x00_rt(rt2x00dev, RT3883)) {
        rt2800_register_write(rt2x00dev, TX_FBK_CFG_3S_0, 0x12111008);
        rt2800_register_write(rt2x00dev, TX_FBK_CFG_3S_1, 0x16151413);
    }

    reg = rt2800_register_read(rt2x00dev, TX_RTS_CFG);
    rt2x00_set_field32(&reg, TX_RTS_CFG_AUTO_RTS_RETRY_LIMIT, 7);
    rt2x00_set_field32(&reg, TX_RTS_CFG_RTS_THRES,
            IEEE80211_MAX_RTS_THRESHOLD);
    rt2x00_set_field32(&reg, TX_RTS_CFG_RTS_FBK_EN, 1);
    rt2800_register_write(rt2x00dev, TX_RTS_CFG, reg);

    rt2800_register_write(rt2x00dev, EXP_ACK_TIME, 0x002400ca);

    /*
     * Usually the CCK SIFS time should be set to 10 and the OFDM SIFS
     * time should be set to 16. However, the original Ralink driver uses
     * 16 for both and indeed using a value of 10 for CCK SIFS results in
     * connection problems with 11g + CTS protection. Hence, use the same
     * defaults as the Ralink driver: 16 for both, CCK and OFDM SIFS.
     */
    reg = rt2800_register_read(rt2x00dev, XIFS_TIME_CFG);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_CCKM_SIFS_TIME, 16);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_OFDM_SIFS_TIME, 16);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_OFDM_XIFS_TIME, 4);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_EIFS, 314);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_BB_RXEND_ENABLE, 1);
    rt2800_register_write(rt2x00dev, XIFS_TIME_CFG, reg);

    rt2800_register_write(rt2x00dev, PWR_PIN_CFG, 0x00000003);

    /*
     * ASIC will keep garbage value after boot, clear encryption keys.
     */
    for (i = 0; i < 4; i++)
        rt2800_register_write(rt2x00dev,
                SHARED_KEY_MODE_ENTRY(i), 0);

    for (i = 0; i < 256; i++) {
        rt2800_config_wcid(rt2x00dev, NULL, i);
        rt2800_delete_wcid_attr(rt2x00dev, i);
        rt2800_register_write(rt2x00dev, MAC_IVEIV_ENTRY(i), 0);
    }

    /*
     * Clear all beacons
     */
    for (i = 0; i < 8; i++)
        rt2800_clear_beacon_register(rt2x00dev, i);

    if (rt2x00_is_usb(rt2x00dev)) {
        reg = rt2800_register_read(rt2x00dev, US_CYC_CNT);
        rt2x00_set_field32(&reg, US_CYC_CNT_CLOCK_CYCLE, 30);
        rt2800_register_write(rt2x00dev, US_CYC_CNT, reg);
    } else if (rt2x00_is_pcie(rt2x00dev)) {
        reg = rt2800_register_read(rt2x00dev, US_CYC_CNT);
        rt2x00_set_field32(&reg, US_CYC_CNT_CLOCK_CYCLE, 125);
        rt2800_register_write(rt2x00dev, US_CYC_CNT, reg);
    }

    reg = rt2800_register_read(rt2x00dev, HT_FBK_CFG0);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS0FBK, 0);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS1FBK, 0);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS2FBK, 1);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS3FBK, 2);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS4FBK, 3);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS5FBK, 4);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS6FBK, 5);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS7FBK, 6);
    rt2800_register_write(rt2x00dev, HT_FBK_CFG0, reg);

    reg = rt2800_register_read(rt2x00dev, HT_FBK_CFG1);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS8FBK, 8);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS9FBK, 8);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS10FBK, 9);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS11FBK, 10);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS12FBK, 11);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS13FBK, 12);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS14FBK, 13);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS15FBK, 14);
    rt2800_register_write(rt2x00dev, HT_FBK_CFG1, reg);

    reg = rt2800_register_read(rt2x00dev, LG_FBK_CFG0);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS0FBK, 8);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS1FBK, 8);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS2FBK, 9);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS3FBK, 10);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS4FBK, 11);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS5FBK, 12);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS6FBK, 13);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS7FBK, 14);
    rt2800_register_write(rt2x00dev, LG_FBK_CFG0, reg);

    reg = rt2800_register_read(rt2x00dev, LG_FBK_CFG1);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS0FBK, 0);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS1FBK, 0);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS2FBK, 1);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS3FBK, 2);
    rt2800_register_write(rt2x00dev, LG_FBK_CFG1, reg);

    /*
     * Do not force the BA window size, we use the TXWI to set it
     */
    reg = rt2800_register_read(rt2x00dev, AMPDU_BA_WINSIZE);
    rt2x00_set_field32(&reg, AMPDU_BA_WINSIZE_FORCE_WINSIZE_ENABLE, 0);
    rt2x00_set_field32(&reg, AMPDU_BA_WINSIZE_FORCE_WINSIZE, 0);
    rt2800_register_write(rt2x00dev, AMPDU_BA_WINSIZE, reg);

    /*
     * We must clear the error counters.
     * These registers are cleared on read,
     * so we may pass a useless variable to store the value.
     */
    reg = rt2800_register_read(rt2x00dev, RX_STA_CNT0);
    reg = rt2800_register_read(rt2x00dev, RX_STA_CNT1);
    reg = rt2800_register_read(rt2x00dev, RX_STA_CNT2);
    reg = rt2800_register_read(rt2x00dev, TX_STA_CNT0);
    reg = rt2800_register_read(rt2x00dev, TX_STA_CNT1);
    reg = rt2800_register_read(rt2x00dev, TX_STA_CNT2);

    /*
     * Setup leadtime for pre tbtt interrupt to 6ms
     */
    reg = rt2800_register_read(rt2x00dev, INT_TIMER_CFG);
    rt2x00_set_field32(&reg, INT_TIMER_CFG_PRE_TBTT_TIMER, 6 << 4);
    rt2800_register_write(rt2x00dev, INT_TIMER_CFG, reg);

    /*
     * Set up channel statistics timer
     */
    reg = rt2800_register_read(rt2x00dev, CH_TIME_CFG);
    rt2x00_set_field32(&reg, CH_TIME_CFG_EIFS_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_NAV_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_RX_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_TX_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_TMR_EN, 1);
    rt2800_register_write(rt2x00dev, CH_TIME_CFG, reg);

    return 0;
}

static int rt2800_wait_bbp_rf_ready(struct rt2x00_dev *rt2x00dev)
{
    unsigned int i;
    uint32_t reg;

    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        reg = rt2800_register_read(rt2x00dev, MAC_STATUS_CFG);
        if (!rt2x00_get_field32(reg, MAC_STATUS_CFG_BBP_RF_BUSY))
            return 0;

        usleep(REGISTER_BUSY_DELAY);
    }

    rt2x00_err(rt2x00dev, "BBP/RF register access failed, aborting\n");
    return -EACCES;
}

static int rt2800_wait_bbp_ready(struct rt2x00_dev *rt2x00dev)
{
    unsigned int i;
    uint8_t value;

    /*
     * BBP was enabled after firmware was loaded,
     * but we need to reactivate it now.
     */
    rt2800_register_write(rt2x00dev, H2M_BBP_AGENT, 0);
    rt2800_register_write(rt2x00dev, H2M_MAILBOX_CSR, 0);
    usleep(1);

    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        value = rt2800_bbp_read(rt2x00dev, 0);
        if ((value != 0xff) && (value != 0x00))
            return 0;
        usleep(REGISTER_BUSY_DELAY);
    }

    rt2x00_err(rt2x00dev, "BBP register access failed, aborting\n");
    return -EACCES;
}

int rt2800_rfkill_poll(struct rt2x00_dev *rt2x00dev) {
    uint32_t reg;

    if (rt2x00_rt(rt2x00dev, RT3290)) {
        reg = rt2800_register_read(rt2x00dev, WLAN_FUN_CTRL);
        return rt2x00_get_field32(reg, WLAN_GPIO_IN_BIT0);
    } else {
        reg = rt2800_register_read(rt2x00dev, GPIO_CTRL);
        return rt2x00_get_field32(reg, GPIO_CTRL_VAL2);
    }
}

static void rt2800_bbp4_mac_if_ctrl(struct rt2x00_dev *rt2x00dev)
{
	uint8_t value;

	value = rt2800_bbp_read(rt2x00dev, 4);
	rt2x00_set_field8(&value, BBP4_MAC_IF_CTRL, 1);
	rt2800_bbp_write(rt2x00dev, 4, value);
}

static void rt2800_init_freq_calibration(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 142, 1);
	rt2800_bbp_write(rt2x00dev, 143, 57);
}

static void rt2800_init_bbp_5592_glrt(struct rt2x00_dev *rt2x00dev)
{
	static const uint8_t glrt_table[] = {
		0xE0, 0x1F, 0X38, 0x32, 0x08, 0x28, 0x19, 0x0A, 0xFF, 0x00, /* 128 ~ 137 */
		0x16, 0x10, 0x10, 0x0B, 0x36, 0x2C, 0x26, 0x24, 0x42, 0x36, /* 138 ~ 147 */
		0x30, 0x2D, 0x4C, 0x46, 0x3D, 0x40, 0x3E, 0x42, 0x3D, 0x40, /* 148 ~ 157 */
		0X3C, 0x34, 0x2C, 0x2F, 0x3C, 0x35, 0x2E, 0x2A, 0x49, 0x41, /* 158 ~ 167 */
		0x36, 0x31, 0x30, 0x30, 0x0E, 0x0D, 0x28, 0x21, 0x1C, 0x16, /* 168 ~ 177 */
		0x50, 0x4A, 0x43, 0x40, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, /* 178 ~ 187 */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 188 ~ 197 */
		0x00, 0x00, 0x7D, 0x14, 0x32, 0x2C, 0x36, 0x4C, 0x43, 0x2C, /* 198 ~ 207 */
		0x2E, 0x36, 0x30, 0x6E,					    /* 208 ~ 211 */
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(glrt_table); i++) {
		rt2800_bbp_write(rt2x00dev, 195, 128 + i);
		rt2800_bbp_write(rt2x00dev, 196, glrt_table[i]);
	}
};

static void rt2800_init_bbp_early(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 65, 0x2C);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);
	rt2800_bbp_write(rt2x00dev, 68, 0x0B);
	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 70, 0x0a);
	rt2800_bbp_write(rt2x00dev, 73, 0x10);
	rt2800_bbp_write(rt2x00dev, 81, 0x37);
	rt2800_bbp_write(rt2x00dev, 82, 0x62);
	rt2800_bbp_write(rt2x00dev, 83, 0x6A);
	rt2800_bbp_write(rt2x00dev, 84, 0x99);
	rt2800_bbp_write(rt2x00dev, 86, 0x00);
	rt2800_bbp_write(rt2x00dev, 91, 0x04);
	rt2800_bbp_write(rt2x00dev, 92, 0x00);
	rt2800_bbp_write(rt2x00dev, 103, 0x00);
	rt2800_bbp_write(rt2x00dev, 105, 0x05);
	rt2800_bbp_write(rt2x00dev, 106, 0x35);
}

static void rt2800_disable_unused_dac_adc(struct rt2x00_dev *rt2x00dev)
{
	uint16_t eeprom;
	uint8_t value;

	value = rt2800_bbp_read(rt2x00dev, 138);
	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF0);
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_TXPATH) == 1)
		value |= 0x20;
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_RXPATH) == 1)
		value &= ~0x02;
	rt2800_bbp_write(rt2x00dev, 138, value);
}

static void rt2800_init_bbp_305x_soc(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 31, 0x08);

	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 73, 0x10);

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 78, 0x0e);
	rt2800_bbp_write(rt2x00dev, 80, 0x08);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	rt2800_bbp_write(rt2x00dev, 83, 0x6a);

	rt2800_bbp_write(rt2x00dev, 84, 0x99);

	rt2800_bbp_write(rt2x00dev, 86, 0x00);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x00);

	rt2800_bbp_write(rt2x00dev, 103, 0xc0);

	rt2800_bbp_write(rt2x00dev, 105, 0x01);

	rt2800_bbp_write(rt2x00dev, 106, 0x35);
}

static void rt2800_init_bbp_28xx(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	if (rt2x00_rt_rev(rt2x00dev, RT2860, REV_RT2860C)) {
		rt2800_bbp_write(rt2x00dev, 69, 0x16);
		rt2800_bbp_write(rt2x00dev, 73, 0x12);
	} else {
		rt2800_bbp_write(rt2x00dev, 69, 0x12);
		rt2800_bbp_write(rt2x00dev, 73, 0x10);
	}

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 81, 0x37);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	rt2800_bbp_write(rt2x00dev, 83, 0x6a);

	if (rt2x00_rt_rev(rt2x00dev, RT2860, REV_RT2860D))
		rt2800_bbp_write(rt2x00dev, 84, 0x19);
	else
		rt2800_bbp_write(rt2x00dev, 84, 0x99);

	rt2800_bbp_write(rt2x00dev, 86, 0x00);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x00);

	rt2800_bbp_write(rt2x00dev, 103, 0x00);

	rt2800_bbp_write(rt2x00dev, 105, 0x05);

	rt2800_bbp_write(rt2x00dev, 106, 0x35);
}

static void rt2800_init_bbp_30xx(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 73, 0x10);

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 79, 0x13);
	rt2800_bbp_write(rt2x00dev, 80, 0x05);
	rt2800_bbp_write(rt2x00dev, 81, 0x33);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	rt2800_bbp_write(rt2x00dev, 83, 0x6a);

	rt2800_bbp_write(rt2x00dev, 84, 0x99);

	rt2800_bbp_write(rt2x00dev, 86, 0x00);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x00);

	if (rt2x00_rt_rev_gte(rt2x00dev, RT3070, REV_RT3070F) ||
	    rt2x00_rt_rev_gte(rt2x00dev, RT3071, REV_RT3071E) ||
	    rt2x00_rt_rev_gte(rt2x00dev, RT3090, REV_RT3090E))
		rt2800_bbp_write(rt2x00dev, 103, 0xc0);
	else
		rt2800_bbp_write(rt2x00dev, 103, 0x00);

	rt2800_bbp_write(rt2x00dev, 105, 0x05);

	rt2800_bbp_write(rt2x00dev, 106, 0x35);

	if (rt2x00_rt(rt2x00dev, RT3071) ||
	    rt2x00_rt(rt2x00dev, RT3090))
		rt2800_disable_unused_dac_adc(rt2x00dev);
}

static void rt2800_init_bbp_3290(struct rt2x00_dev *rt2x00dev)
{
	uint8_t value;

	rt2800_bbp4_mac_if_ctrl(rt2x00dev);

	rt2800_bbp_write(rt2x00dev, 31, 0x08);

	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	rt2800_bbp_write(rt2x00dev, 68, 0x0b);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 73, 0x13);
	rt2800_bbp_write(rt2x00dev, 75, 0x46);
	rt2800_bbp_write(rt2x00dev, 76, 0x28);

	rt2800_bbp_write(rt2x00dev, 77, 0x58);

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 74, 0x0b);
	rt2800_bbp_write(rt2x00dev, 79, 0x18);
	rt2800_bbp_write(rt2x00dev, 80, 0x09);
	rt2800_bbp_write(rt2x00dev, 81, 0x33);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	rt2800_bbp_write(rt2x00dev, 83, 0x7a);

	rt2800_bbp_write(rt2x00dev, 84, 0x9a);

	rt2800_bbp_write(rt2x00dev, 86, 0x38);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x02);

	rt2800_bbp_write(rt2x00dev, 103, 0xc0);

	rt2800_bbp_write(rt2x00dev, 104, 0x92);

	rt2800_bbp_write(rt2x00dev, 105, 0x1c);

	rt2800_bbp_write(rt2x00dev, 106, 0x03);

	rt2800_bbp_write(rt2x00dev, 128, 0x12);

	rt2800_bbp_write(rt2x00dev, 67, 0x24);
	rt2800_bbp_write(rt2x00dev, 143, 0x04);
	rt2800_bbp_write(rt2x00dev, 142, 0x99);
	rt2800_bbp_write(rt2x00dev, 150, 0x30);
	rt2800_bbp_write(rt2x00dev, 151, 0x2e);
	rt2800_bbp_write(rt2x00dev, 152, 0x20);
	rt2800_bbp_write(rt2x00dev, 153, 0x34);
	rt2800_bbp_write(rt2x00dev, 154, 0x40);
	rt2800_bbp_write(rt2x00dev, 155, 0x3b);
	rt2800_bbp_write(rt2x00dev, 253, 0x04);

	value = rt2800_bbp_read(rt2x00dev, 47);
	rt2x00_set_field8(&value, BBP47_TSSI_ADC6, 1);
	rt2800_bbp_write(rt2x00dev, 47, value);

	/* Use 5-bit ADC for Acquisition and 8-bit ADC for data */
	value = rt2800_bbp_read(rt2x00dev, 3);
	rt2x00_set_field8(&value, BBP3_ADC_MODE_SWITCH, 1);
	rt2x00_set_field8(&value, BBP3_ADC_INIT_MODE, 1);
	rt2800_bbp_write(rt2x00dev, 3, value);
}

static void rt2800_init_bbp_3352(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 3, 0x00);
	rt2800_bbp_write(rt2x00dev, 4, 0x50);

	rt2800_bbp_write(rt2x00dev, 31, 0x08);

	rt2800_bbp_write(rt2x00dev, 47, 0x48);

	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	rt2800_bbp_write(rt2x00dev, 68, 0x0b);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 73, 0x13);
	rt2800_bbp_write(rt2x00dev, 75, 0x46);
	rt2800_bbp_write(rt2x00dev, 76, 0x28);

	rt2800_bbp_write(rt2x00dev, 77, 0x59);

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 78, 0x0e);
	rt2800_bbp_write(rt2x00dev, 80, 0x08);
	rt2800_bbp_write(rt2x00dev, 81, 0x37);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	if (rt2x00_rt(rt2x00dev, RT5350)) {
		rt2800_bbp_write(rt2x00dev, 83, 0x7a);
		rt2800_bbp_write(rt2x00dev, 84, 0x9a);
	} else {
		rt2800_bbp_write(rt2x00dev, 83, 0x6a);
		rt2800_bbp_write(rt2x00dev, 84, 0x99);
	}

	rt2800_bbp_write(rt2x00dev, 86, 0x38);

	rt2800_bbp_write(rt2x00dev, 88, 0x90);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x02);

	rt2800_bbp_write(rt2x00dev, 103, 0xc0);

	rt2800_bbp_write(rt2x00dev, 104, 0x92);

	if (rt2x00_rt(rt2x00dev, RT5350)) {
		rt2800_bbp_write(rt2x00dev, 105, 0x3c);
		rt2800_bbp_write(rt2x00dev, 106, 0x03);
	} else {
		rt2800_bbp_write(rt2x00dev, 105, 0x34);
		rt2800_bbp_write(rt2x00dev, 106, 0x05);
	}

	rt2800_bbp_write(rt2x00dev, 120, 0x50);

	rt2800_bbp_write(rt2x00dev, 137, 0x0f);

	rt2800_bbp_write(rt2x00dev, 163, 0xbd);
	/* Set ITxBF timeout to 0x9c40=1000msec */
	rt2800_bbp_write(rt2x00dev, 179, 0x02);
	rt2800_bbp_write(rt2x00dev, 180, 0x00);
	rt2800_bbp_write(rt2x00dev, 182, 0x40);
	rt2800_bbp_write(rt2x00dev, 180, 0x01);
	rt2800_bbp_write(rt2x00dev, 182, 0x9c);
	rt2800_bbp_write(rt2x00dev, 179, 0x00);
	/* Reprogram the inband interface to put right values in RXWI */
	rt2800_bbp_write(rt2x00dev, 142, 0x04);
	rt2800_bbp_write(rt2x00dev, 143, 0x3b);
	rt2800_bbp_write(rt2x00dev, 142, 0x06);
	rt2800_bbp_write(rt2x00dev, 143, 0xa0);
	rt2800_bbp_write(rt2x00dev, 142, 0x07);
	rt2800_bbp_write(rt2x00dev, 143, 0xa1);
	rt2800_bbp_write(rt2x00dev, 142, 0x08);
	rt2800_bbp_write(rt2x00dev, 143, 0xa2);

	rt2800_bbp_write(rt2x00dev, 148, 0xc8);

	if (rt2x00_rt(rt2x00dev, RT5350)) {
		/* Antenna Software OFDM */
		rt2800_bbp_write(rt2x00dev, 150, 0x40);
		/* Antenna Software CCK */
		rt2800_bbp_write(rt2x00dev, 151, 0x30);
		rt2800_bbp_write(rt2x00dev, 152, 0xa3);
		/* Clear previously selected antenna */
		rt2800_bbp_write(rt2x00dev, 154, 0);
	}
}

static void rt2800_init_bbp_3390(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 73, 0x10);

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 79, 0x13);
	rt2800_bbp_write(rt2x00dev, 80, 0x05);
	rt2800_bbp_write(rt2x00dev, 81, 0x33);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	rt2800_bbp_write(rt2x00dev, 83, 0x6a);

	rt2800_bbp_write(rt2x00dev, 84, 0x99);

	rt2800_bbp_write(rt2x00dev, 86, 0x00);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x00);

	if (rt2x00_rt_rev_gte(rt2x00dev, RT3390, REV_RT3390E))
		rt2800_bbp_write(rt2x00dev, 103, 0xc0);
	else
		rt2800_bbp_write(rt2x00dev, 103, 0x00);

	rt2800_bbp_write(rt2x00dev, 105, 0x05);

	rt2800_bbp_write(rt2x00dev, 106, 0x35);

	rt2800_disable_unused_dac_adc(rt2x00dev);
}

static void rt2800_init_bbp_3572(struct rt2x00_dev *rt2x00dev)
{
	rt2800_bbp_write(rt2x00dev, 31, 0x08);

	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 73, 0x10);

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 79, 0x13);
	rt2800_bbp_write(rt2x00dev, 80, 0x05);
	rt2800_bbp_write(rt2x00dev, 81, 0x33);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	rt2800_bbp_write(rt2x00dev, 83, 0x6a);

	rt2800_bbp_write(rt2x00dev, 84, 0x99);

	rt2800_bbp_write(rt2x00dev, 86, 0x00);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x00);

	rt2800_bbp_write(rt2x00dev, 103, 0xc0);

	rt2800_bbp_write(rt2x00dev, 105, 0x05);

	rt2800_bbp_write(rt2x00dev, 106, 0x35);

	rt2800_disable_unused_dac_adc(rt2x00dev);
}

static void rt2800_init_bbp_3593(struct rt2x00_dev *rt2x00dev)
{
	rt2800_init_bbp_early(rt2x00dev);

	rt2800_bbp_write(rt2x00dev, 79, 0x13);
	rt2800_bbp_write(rt2x00dev, 80, 0x05);
	rt2800_bbp_write(rt2x00dev, 81, 0x33);
	rt2800_bbp_write(rt2x00dev, 137, 0x0f);

	rt2800_bbp_write(rt2x00dev, 84, 0x19);

	/* Enable DC filter */
	if (rt2x00_rt_rev_gte(rt2x00dev, RT3593, REV_RT3593E))
		rt2800_bbp_write(rt2x00dev, 103, 0xc0);
}

static void rt2800_init_bbp_3883(struct rt2x00_dev *rt2x00dev)
{
	rt2800_init_bbp_early(rt2x00dev);

	rt2800_bbp_write(rt2x00dev, 4, 0x50);
	rt2800_bbp_write(rt2x00dev, 47, 0x48);

	rt2800_bbp_write(rt2x00dev, 86, 0x46);
	rt2800_bbp_write(rt2x00dev, 88, 0x90);

	rt2800_bbp_write(rt2x00dev, 92, 0x02);

	rt2800_bbp_write(rt2x00dev, 103, 0xc0);
	rt2800_bbp_write(rt2x00dev, 104, 0x92);
	rt2800_bbp_write(rt2x00dev, 105, 0x34);
	rt2800_bbp_write(rt2x00dev, 106, 0x12);
	rt2800_bbp_write(rt2x00dev, 120, 0x50);
	rt2800_bbp_write(rt2x00dev, 137, 0x0f);
	rt2800_bbp_write(rt2x00dev, 163, 0x9d);

	/* Set ITxBF timeout to 0x9C40=1000msec */
	rt2800_bbp_write(rt2x00dev, 179, 0x02);
	rt2800_bbp_write(rt2x00dev, 180, 0x00);
	rt2800_bbp_write(rt2x00dev, 182, 0x40);
	rt2800_bbp_write(rt2x00dev, 180, 0x01);
	rt2800_bbp_write(rt2x00dev, 182, 0x9c);

	rt2800_bbp_write(rt2x00dev, 179, 0x00);

	/* Reprogram the inband interface to put right values in RXWI */
	rt2800_bbp_write(rt2x00dev, 142, 0x04);
	rt2800_bbp_write(rt2x00dev, 143, 0x3b);
	rt2800_bbp_write(rt2x00dev, 142, 0x06);
	rt2800_bbp_write(rt2x00dev, 143, 0xa0);
	rt2800_bbp_write(rt2x00dev, 142, 0x07);
	rt2800_bbp_write(rt2x00dev, 143, 0xa1);
	rt2800_bbp_write(rt2x00dev, 142, 0x08);
	rt2800_bbp_write(rt2x00dev, 143, 0xa2);
	rt2800_bbp_write(rt2x00dev, 148, 0xc8);
}

static void rt2800_init_bbp_53xx(struct rt2x00_dev *rt2x00dev)
{
	int ant, div_mode;
	uint16_t eeprom;
	uint8_t value;

	rt2800_bbp4_mac_if_ctrl(rt2x00dev);

	rt2800_bbp_write(rt2x00dev, 31, 0x08);

	rt2800_bbp_write(rt2x00dev, 65, 0x2c);
	rt2800_bbp_write(rt2x00dev, 66, 0x38);

	rt2800_bbp_write(rt2x00dev, 68, 0x0b);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);
	rt2800_bbp_write(rt2x00dev, 73, 0x13);
	rt2800_bbp_write(rt2x00dev, 75, 0x46);
	rt2800_bbp_write(rt2x00dev, 76, 0x28);

	rt2800_bbp_write(rt2x00dev, 77, 0x59);

	rt2800_bbp_write(rt2x00dev, 70, 0x0a);

	rt2800_bbp_write(rt2x00dev, 79, 0x13);
	rt2800_bbp_write(rt2x00dev, 80, 0x05);
	rt2800_bbp_write(rt2x00dev, 81, 0x33);

	rt2800_bbp_write(rt2x00dev, 82, 0x62);

	rt2800_bbp_write(rt2x00dev, 83, 0x7a);

	rt2800_bbp_write(rt2x00dev, 84, 0x9a);

	rt2800_bbp_write(rt2x00dev, 86, 0x38);

	if (rt2x00_rt(rt2x00dev, RT5392))
		rt2800_bbp_write(rt2x00dev, 88, 0x90);

	rt2800_bbp_write(rt2x00dev, 91, 0x04);

	rt2800_bbp_write(rt2x00dev, 92, 0x02);

	if (rt2x00_rt(rt2x00dev, RT5392)) {
		rt2800_bbp_write(rt2x00dev, 95, 0x9a);
		rt2800_bbp_write(rt2x00dev, 98, 0x12);
	}

	rt2800_bbp_write(rt2x00dev, 103, 0xc0);

	rt2800_bbp_write(rt2x00dev, 104, 0x92);

	rt2800_bbp_write(rt2x00dev, 105, 0x3c);

	if (rt2x00_rt(rt2x00dev, RT5390))
		rt2800_bbp_write(rt2x00dev, 106, 0x03);
	else if (rt2x00_rt(rt2x00dev, RT5392))
		rt2800_bbp_write(rt2x00dev, 106, 0x12);
	else
		WARN_ON(1);

	rt2800_bbp_write(rt2x00dev, 128, 0x12);

	if (rt2x00_rt(rt2x00dev, RT5392)) {
		rt2800_bbp_write(rt2x00dev, 134, 0xd0);
		rt2800_bbp_write(rt2x00dev, 135, 0xf6);
	}

	rt2800_disable_unused_dac_adc(rt2x00dev);

	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);
	div_mode = rt2x00_get_field16(eeprom,
				      EEPROM_NIC_CONF1_ANT_DIVERSITY);
	ant = (div_mode == 3) ? 1 : 0;

	/* check if this is a Bluetooth combo card */
	if (rt2x00_has_cap_bt_coexist(rt2x00dev)) {
		uint32_t reg;

		reg = rt2800_register_read(rt2x00dev, GPIO_CTRL);
		rt2x00_set_field32(&reg, GPIO_CTRL_DIR3, 0);
		rt2x00_set_field32(&reg, GPIO_CTRL_DIR6, 0);
		rt2x00_set_field32(&reg, GPIO_CTRL_VAL3, 0);
		rt2x00_set_field32(&reg, GPIO_CTRL_VAL6, 0);
		if (ant == 0)
			rt2x00_set_field32(&reg, GPIO_CTRL_VAL3, 1);
		else if (ant == 1)
			rt2x00_set_field32(&reg, GPIO_CTRL_VAL6, 1);
		rt2800_register_write(rt2x00dev, GPIO_CTRL, reg);
	}

	/* These chips have hardware RX antenna diversity */
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390R) ||
	    rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5370G)) {
		rt2800_bbp_write(rt2x00dev, 150, 0); /* Disable Antenna Software OFDM */
		rt2800_bbp_write(rt2x00dev, 151, 0); /* Disable Antenna Software CCK */
		rt2800_bbp_write(rt2x00dev, 154, 0); /* Clear previously selected antenna */
	}

	value = rt2800_bbp_read(rt2x00dev, 152);
	if (ant == 0)
		rt2x00_set_field8(&value, BBP152_RX_DEFAULT_ANT, 1);
	else
		rt2x00_set_field8(&value, BBP152_RX_DEFAULT_ANT, 0);
	rt2800_bbp_write(rt2x00dev, 152, value);

	rt2800_init_freq_calibration(rt2x00dev);
}

static void rt2800_init_bbp_5592(struct rt2x00_dev *rt2x00dev)
{
	int ant, div_mode;
	uint16_t eeprom;
	uint8_t value;

	rt2800_init_bbp_early(rt2x00dev);

	value = rt2800_bbp_read(rt2x00dev, 105);
	rt2x00_set_field8(&value, BBP105_MLD,
			  rt2x00dev->default_ant.rx_chain_num == 2);
	rt2800_bbp_write(rt2x00dev, 105, value);

	rt2800_bbp4_mac_if_ctrl(rt2x00dev);

	rt2800_bbp_write(rt2x00dev, 20, 0x06);
	rt2800_bbp_write(rt2x00dev, 31, 0x08);
	rt2800_bbp_write(rt2x00dev, 65, 0x2C);
	rt2800_bbp_write(rt2x00dev, 68, 0xDD);
	rt2800_bbp_write(rt2x00dev, 69, 0x1A);
	rt2800_bbp_write(rt2x00dev, 70, 0x05);
	rt2800_bbp_write(rt2x00dev, 73, 0x13);
	rt2800_bbp_write(rt2x00dev, 74, 0x0F);
	rt2800_bbp_write(rt2x00dev, 75, 0x4F);
	rt2800_bbp_write(rt2x00dev, 76, 0x28);
	rt2800_bbp_write(rt2x00dev, 77, 0x59);
	rt2800_bbp_write(rt2x00dev, 84, 0x9A);
	rt2800_bbp_write(rt2x00dev, 86, 0x38);
	rt2800_bbp_write(rt2x00dev, 88, 0x90);
	rt2800_bbp_write(rt2x00dev, 91, 0x04);
	rt2800_bbp_write(rt2x00dev, 92, 0x02);
	rt2800_bbp_write(rt2x00dev, 95, 0x9a);
	rt2800_bbp_write(rt2x00dev, 98, 0x12);
	rt2800_bbp_write(rt2x00dev, 103, 0xC0);
	rt2800_bbp_write(rt2x00dev, 104, 0x92);
	/* FIXME BBP105 owerwrite */
	rt2800_bbp_write(rt2x00dev, 105, 0x3C);
	rt2800_bbp_write(rt2x00dev, 106, 0x35);
	rt2800_bbp_write(rt2x00dev, 128, 0x12);
	rt2800_bbp_write(rt2x00dev, 134, 0xD0);
	rt2800_bbp_write(rt2x00dev, 135, 0xF6);
	rt2800_bbp_write(rt2x00dev, 137, 0x0F);

	/* Initialize GLRT (Generalized Likehood Radio Test) */
	rt2800_init_bbp_5592_glrt(rt2x00dev);

	rt2800_bbp4_mac_if_ctrl(rt2x00dev);

	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);
	div_mode = rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_ANT_DIVERSITY);
	ant = (div_mode == 3) ? 1 : 0;
	value = rt2800_bbp_read(rt2x00dev, 152);
	if (ant == 0) {
		/* Main antenna */
		rt2x00_set_field8(&value, BBP152_RX_DEFAULT_ANT, 1);
	} else {
		/* Auxiliary antenna */
		rt2x00_set_field8(&value, BBP152_RX_DEFAULT_ANT, 0);
	}
	rt2800_bbp_write(rt2x00dev, 152, value);

	if (rt2x00_rt_rev_gte(rt2x00dev, RT5592, REV_RT5592C)) {
		value = rt2800_bbp_read(rt2x00dev, 254);
		rt2x00_set_field8(&value, BBP254_BIT7, 1);
		rt2800_bbp_write(rt2x00dev, 254, value);
	}

	rt2800_init_freq_calibration(rt2x00dev);

	rt2800_bbp_write(rt2x00dev, 84, 0x19);
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5592, REV_RT5592C))
		rt2800_bbp_write(rt2x00dev, 103, 0xc0);
}

static void rt2800_bbp_glrt_write(struct rt2x00_dev *rt2x00dev,
				  const uint8_t reg, const uint8_t value)
{
	rt2800_bbp_write(rt2x00dev, 195, reg);
	rt2800_bbp_write(rt2x00dev, 196, value);
}

static void rt2800_bbp_dcoc_write(struct rt2x00_dev *rt2x00dev,
				  const uint8_t reg, const uint8_t value)
{
	rt2800_bbp_write(rt2x00dev, 158, reg);
	rt2800_bbp_write(rt2x00dev, 159, value);
}

static uint8_t rt2800_bbp_dcoc_read(struct rt2x00_dev *rt2x00dev, const uint8_t reg)
{
	rt2800_bbp_write(rt2x00dev, 158, reg);
	return rt2800_bbp_read(rt2x00dev, 159);
}

static void rt2800_init_bbp_6352(struct rt2x00_dev *rt2x00dev)
{
	uint8_t bbp;

	/* Apply Maximum Likelihood Detection (MLD) for 2 stream case */
	bbp = rt2800_bbp_read(rt2x00dev, 105);
	rt2x00_set_field8(&bbp, BBP105_MLD,
			  rt2x00dev->default_ant.rx_chain_num == 2);
	rt2800_bbp_write(rt2x00dev, 105, bbp);

	/* Avoid data loss and CRC errors */
	rt2800_bbp4_mac_if_ctrl(rt2x00dev);

	/* Fix I/Q swap issue */
	bbp = rt2800_bbp_read(rt2x00dev, 1);
	bbp |= 0x04;
	rt2800_bbp_write(rt2x00dev, 1, bbp);

	/* BBP for G band */
	rt2800_bbp_write(rt2x00dev, 3, 0x08);
	rt2800_bbp_write(rt2x00dev, 4, 0x00); /* rt2800_bbp4_mac_if_ctrl? */
	rt2800_bbp_write(rt2x00dev, 6, 0x08);
	rt2800_bbp_write(rt2x00dev, 14, 0x09);
	rt2800_bbp_write(rt2x00dev, 15, 0xFF);
	rt2800_bbp_write(rt2x00dev, 16, 0x01);
	rt2800_bbp_write(rt2x00dev, 20, 0x06);
	rt2800_bbp_write(rt2x00dev, 21, 0x00);
	rt2800_bbp_write(rt2x00dev, 22, 0x00);
	rt2800_bbp_write(rt2x00dev, 27, 0x00);
	rt2800_bbp_write(rt2x00dev, 28, 0x00);
	rt2800_bbp_write(rt2x00dev, 30, 0x00);
	rt2800_bbp_write(rt2x00dev, 31, 0x48);
	rt2800_bbp_write(rt2x00dev, 47, 0x40);
	rt2800_bbp_write(rt2x00dev, 62, 0x00);
	rt2800_bbp_write(rt2x00dev, 63, 0x00);
	rt2800_bbp_write(rt2x00dev, 64, 0x00);
	rt2800_bbp_write(rt2x00dev, 65, 0x2C);
	rt2800_bbp_write(rt2x00dev, 66, 0x1C);
	rt2800_bbp_write(rt2x00dev, 67, 0x20);
	rt2800_bbp_write(rt2x00dev, 68, 0xDD);
	rt2800_bbp_write(rt2x00dev, 69, 0x10);
	rt2800_bbp_write(rt2x00dev, 70, 0x05);
	rt2800_bbp_write(rt2x00dev, 73, 0x18);
	rt2800_bbp_write(rt2x00dev, 74, 0x0F);
	rt2800_bbp_write(rt2x00dev, 75, 0x60);
	rt2800_bbp_write(rt2x00dev, 76, 0x44);
	rt2800_bbp_write(rt2x00dev, 77, 0x59);
	rt2800_bbp_write(rt2x00dev, 78, 0x1E);
	rt2800_bbp_write(rt2x00dev, 79, 0x1C);
	rt2800_bbp_write(rt2x00dev, 80, 0x0C);
	rt2800_bbp_write(rt2x00dev, 81, 0x3A);
	rt2800_bbp_write(rt2x00dev, 82, 0xB6);
	rt2800_bbp_write(rt2x00dev, 83, 0x9A);
	rt2800_bbp_write(rt2x00dev, 84, 0x9A);
	rt2800_bbp_write(rt2x00dev, 86, 0x38);
	rt2800_bbp_write(rt2x00dev, 88, 0x90);
	rt2800_bbp_write(rt2x00dev, 91, 0x04);
	rt2800_bbp_write(rt2x00dev, 92, 0x02);
	rt2800_bbp_write(rt2x00dev, 95, 0x9A);
	rt2800_bbp_write(rt2x00dev, 96, 0x00);
	rt2800_bbp_write(rt2x00dev, 103, 0xC0);
	rt2800_bbp_write(rt2x00dev, 104, 0x92);
	/* FIXME BBP105 owerwrite */
	rt2800_bbp_write(rt2x00dev, 105, 0x3C);
	rt2800_bbp_write(rt2x00dev, 106, 0x12);
	rt2800_bbp_write(rt2x00dev, 109, 0x00);
	rt2800_bbp_write(rt2x00dev, 134, 0x10);
	rt2800_bbp_write(rt2x00dev, 135, 0xA6);
	rt2800_bbp_write(rt2x00dev, 137, 0x04);
	rt2800_bbp_write(rt2x00dev, 142, 0x30);
	rt2800_bbp_write(rt2x00dev, 143, 0xF7);
	rt2800_bbp_write(rt2x00dev, 160, 0xEC);
	rt2800_bbp_write(rt2x00dev, 161, 0xC4);
	rt2800_bbp_write(rt2x00dev, 162, 0x77);
	rt2800_bbp_write(rt2x00dev, 163, 0xF9);
	rt2800_bbp_write(rt2x00dev, 164, 0x00);
	rt2800_bbp_write(rt2x00dev, 165, 0x00);
	rt2800_bbp_write(rt2x00dev, 186, 0x00);
	rt2800_bbp_write(rt2x00dev, 187, 0x00);
	rt2800_bbp_write(rt2x00dev, 188, 0x00);
	rt2800_bbp_write(rt2x00dev, 186, 0x00);
	rt2800_bbp_write(rt2x00dev, 187, 0x01);
	rt2800_bbp_write(rt2x00dev, 188, 0x00);
	rt2800_bbp_write(rt2x00dev, 189, 0x00);

	rt2800_bbp_write(rt2x00dev, 91, 0x06);
	rt2800_bbp_write(rt2x00dev, 92, 0x04);
	rt2800_bbp_write(rt2x00dev, 93, 0x54);
	rt2800_bbp_write(rt2x00dev, 99, 0x50);
	rt2800_bbp_write(rt2x00dev, 148, 0x84);
	rt2800_bbp_write(rt2x00dev, 167, 0x80);
	rt2800_bbp_write(rt2x00dev, 178, 0xFF);
	rt2800_bbp_write(rt2x00dev, 106, 0x13);

	/* BBP for G band GLRT function (BBP_128 ~ BBP_221) */
	rt2800_bbp_glrt_write(rt2x00dev, 0, 0x00);
	rt2800_bbp_glrt_write(rt2x00dev, 1, 0x14);
	rt2800_bbp_glrt_write(rt2x00dev, 2, 0x20);
	rt2800_bbp_glrt_write(rt2x00dev, 3, 0x0A);
	rt2800_bbp_glrt_write(rt2x00dev, 10, 0x16);
	rt2800_bbp_glrt_write(rt2x00dev, 11, 0x06);
	rt2800_bbp_glrt_write(rt2x00dev, 12, 0x02);
	rt2800_bbp_glrt_write(rt2x00dev, 13, 0x07);
	rt2800_bbp_glrt_write(rt2x00dev, 14, 0x05);
	rt2800_bbp_glrt_write(rt2x00dev, 15, 0x09);
	rt2800_bbp_glrt_write(rt2x00dev, 16, 0x20);
	rt2800_bbp_glrt_write(rt2x00dev, 17, 0x08);
	rt2800_bbp_glrt_write(rt2x00dev, 18, 0x4A);
	rt2800_bbp_glrt_write(rt2x00dev, 19, 0x00);
	rt2800_bbp_glrt_write(rt2x00dev, 20, 0x00);
	rt2800_bbp_glrt_write(rt2x00dev, 128, 0xE0);
	rt2800_bbp_glrt_write(rt2x00dev, 129, 0x1F);
	rt2800_bbp_glrt_write(rt2x00dev, 130, 0x4F);
	rt2800_bbp_glrt_write(rt2x00dev, 131, 0x32);
	rt2800_bbp_glrt_write(rt2x00dev, 132, 0x08);
	rt2800_bbp_glrt_write(rt2x00dev, 133, 0x28);
	rt2800_bbp_glrt_write(rt2x00dev, 134, 0x19);
	rt2800_bbp_glrt_write(rt2x00dev, 135, 0x0A);
	rt2800_bbp_glrt_write(rt2x00dev, 138, 0x16);
	rt2800_bbp_glrt_write(rt2x00dev, 139, 0x10);
	rt2800_bbp_glrt_write(rt2x00dev, 140, 0x10);
	rt2800_bbp_glrt_write(rt2x00dev, 141, 0x1A);
	rt2800_bbp_glrt_write(rt2x00dev, 142, 0x36);
	rt2800_bbp_glrt_write(rt2x00dev, 143, 0x2C);
	rt2800_bbp_glrt_write(rt2x00dev, 144, 0x26);
	rt2800_bbp_glrt_write(rt2x00dev, 145, 0x24);
	rt2800_bbp_glrt_write(rt2x00dev, 146, 0x42);
	rt2800_bbp_glrt_write(rt2x00dev, 147, 0x40);
	rt2800_bbp_glrt_write(rt2x00dev, 148, 0x30);
	rt2800_bbp_glrt_write(rt2x00dev, 149, 0x29);
	rt2800_bbp_glrt_write(rt2x00dev, 150, 0x4C);
	rt2800_bbp_glrt_write(rt2x00dev, 151, 0x46);
	rt2800_bbp_glrt_write(rt2x00dev, 152, 0x3D);
	rt2800_bbp_glrt_write(rt2x00dev, 153, 0x40);
	rt2800_bbp_glrt_write(rt2x00dev, 154, 0x3E);
	rt2800_bbp_glrt_write(rt2x00dev, 155, 0x38);
	rt2800_bbp_glrt_write(rt2x00dev, 156, 0x3D);
	rt2800_bbp_glrt_write(rt2x00dev, 157, 0x2F);
	rt2800_bbp_glrt_write(rt2x00dev, 158, 0x3C);
	rt2800_bbp_glrt_write(rt2x00dev, 159, 0x34);
	rt2800_bbp_glrt_write(rt2x00dev, 160, 0x2C);
	rt2800_bbp_glrt_write(rt2x00dev, 161, 0x2F);
	rt2800_bbp_glrt_write(rt2x00dev, 162, 0x3C);
	rt2800_bbp_glrt_write(rt2x00dev, 163, 0x35);
	rt2800_bbp_glrt_write(rt2x00dev, 164, 0x2E);
	rt2800_bbp_glrt_write(rt2x00dev, 165, 0x2F);
	rt2800_bbp_glrt_write(rt2x00dev, 166, 0x49);
	rt2800_bbp_glrt_write(rt2x00dev, 167, 0x41);
	rt2800_bbp_glrt_write(rt2x00dev, 168, 0x36);
	rt2800_bbp_glrt_write(rt2x00dev, 169, 0x39);
	rt2800_bbp_glrt_write(rt2x00dev, 170, 0x30);
	rt2800_bbp_glrt_write(rt2x00dev, 171, 0x30);
	rt2800_bbp_glrt_write(rt2x00dev, 172, 0x0E);
	rt2800_bbp_glrt_write(rt2x00dev, 173, 0x0D);
	rt2800_bbp_glrt_write(rt2x00dev, 174, 0x28);
	rt2800_bbp_glrt_write(rt2x00dev, 175, 0x21);
	rt2800_bbp_glrt_write(rt2x00dev, 176, 0x1C);
	rt2800_bbp_glrt_write(rt2x00dev, 177, 0x16);
	rt2800_bbp_glrt_write(rt2x00dev, 178, 0x50);
	rt2800_bbp_glrt_write(rt2x00dev, 179, 0x4A);
	rt2800_bbp_glrt_write(rt2x00dev, 180, 0x43);
	rt2800_bbp_glrt_write(rt2x00dev, 181, 0x50);
	rt2800_bbp_glrt_write(rt2x00dev, 182, 0x10);
	rt2800_bbp_glrt_write(rt2x00dev, 183, 0x10);
	rt2800_bbp_glrt_write(rt2x00dev, 184, 0x10);
	rt2800_bbp_glrt_write(rt2x00dev, 185, 0x10);
	rt2800_bbp_glrt_write(rt2x00dev, 200, 0x7D);
	rt2800_bbp_glrt_write(rt2x00dev, 201, 0x14);
	rt2800_bbp_glrt_write(rt2x00dev, 202, 0x32);
	rt2800_bbp_glrt_write(rt2x00dev, 203, 0x2C);
	rt2800_bbp_glrt_write(rt2x00dev, 204, 0x36);
	rt2800_bbp_glrt_write(rt2x00dev, 205, 0x4C);
	rt2800_bbp_glrt_write(rt2x00dev, 206, 0x43);
	rt2800_bbp_glrt_write(rt2x00dev, 207, 0x2C);
	rt2800_bbp_glrt_write(rt2x00dev, 208, 0x2E);
	rt2800_bbp_glrt_write(rt2x00dev, 209, 0x36);
	rt2800_bbp_glrt_write(rt2x00dev, 210, 0x30);
	rt2800_bbp_glrt_write(rt2x00dev, 211, 0x6E);

	/* BBP for G band DCOC function */
	rt2800_bbp_dcoc_write(rt2x00dev, 140, 0x0C);
	rt2800_bbp_dcoc_write(rt2x00dev, 141, 0x00);
	rt2800_bbp_dcoc_write(rt2x00dev, 142, 0x10);
	rt2800_bbp_dcoc_write(rt2x00dev, 143, 0x10);
	rt2800_bbp_dcoc_write(rt2x00dev, 144, 0x10);
	rt2800_bbp_dcoc_write(rt2x00dev, 145, 0x10);
	rt2800_bbp_dcoc_write(rt2x00dev, 146, 0x08);
	rt2800_bbp_dcoc_write(rt2x00dev, 147, 0x40);
	rt2800_bbp_dcoc_write(rt2x00dev, 148, 0x04);
	rt2800_bbp_dcoc_write(rt2x00dev, 149, 0x04);
	rt2800_bbp_dcoc_write(rt2x00dev, 150, 0x08);
	rt2800_bbp_dcoc_write(rt2x00dev, 151, 0x08);
	rt2800_bbp_dcoc_write(rt2x00dev, 152, 0x03);
	rt2800_bbp_dcoc_write(rt2x00dev, 153, 0x03);
	rt2800_bbp_dcoc_write(rt2x00dev, 154, 0x03);
	rt2800_bbp_dcoc_write(rt2x00dev, 155, 0x02);
	rt2800_bbp_dcoc_write(rt2x00dev, 156, 0x40);
	rt2800_bbp_dcoc_write(rt2x00dev, 157, 0x40);
	rt2800_bbp_dcoc_write(rt2x00dev, 158, 0x64);
	rt2800_bbp_dcoc_write(rt2x00dev, 159, 0x64);

	rt2800_bbp4_mac_if_ctrl(rt2x00dev);
}

static void rt2800_init_bbp(struct rt2x00_dev *rt2x00dev)
{
	unsigned int i;
	uint16_t eeprom;
	uint8_t reg_id;
	uint8_t value;

	if (rt2800_is_305x_soc(rt2x00dev))
		rt2800_init_bbp_305x_soc(rt2x00dev);

	switch (rt2x00dev->chip.rt) {
	case RT2860:
	case RT2872:
	case RT2883:
		rt2800_init_bbp_28xx(rt2x00dev);
		break;
	case RT3070:
	case RT3071:
	case RT3090:
		rt2800_init_bbp_30xx(rt2x00dev);
		break;
	case RT3290:
		rt2800_init_bbp_3290(rt2x00dev);
		break;
	case RT3352:
	case RT5350:
		rt2800_init_bbp_3352(rt2x00dev);
		break;
	case RT3390:
		rt2800_init_bbp_3390(rt2x00dev);
		break;
	case RT3572:
		rt2800_init_bbp_3572(rt2x00dev);
		break;
	case RT3593:
		rt2800_init_bbp_3593(rt2x00dev);
		return;
	case RT3883:
		rt2800_init_bbp_3883(rt2x00dev);
		return;
	case RT5390:
	case RT5392:
		rt2800_init_bbp_53xx(rt2x00dev);
		break;
	case RT5592:
		rt2800_init_bbp_5592(rt2x00dev);
		return;
	case RT6352:
		rt2800_init_bbp_6352(rt2x00dev);
		break;
	}

	for (i = 0; i < EEPROM_BBP_SIZE; i++) {
		eeprom = rt2800_eeprom_read_from_array(rt2x00dev,
						       EEPROM_BBP_START, i);

		if (eeprom != 0xffff && eeprom != 0x0000) {
			reg_id = rt2x00_get_field16(eeprom, EEPROM_BBP_REG_ID);
			value = rt2x00_get_field16(eeprom, EEPROM_BBP_VALUE);
			rt2800_bbp_write(rt2x00dev, reg_id, value);
		}
	}
}

static void rt2800_led_open_drain_enable(struct rt2x00_dev *rt2x00dev)
{
	uint32_t reg;

	reg = rt2800_register_read(rt2x00dev, OPT_14_CSR);
	rt2x00_set_field32(&reg, OPT_14_CSR_BIT0, 1);
	rt2800_register_write(rt2x00dev, OPT_14_CSR, reg);
}

static uint8_t rt2800_init_rx_filter(struct rt2x00_dev *rt2x00dev, bool bw40,
				uint8_t filter_target)
{
	unsigned int i;
	uint8_t bbp;
	uint8_t rfcsr;
	uint8_t passband;
	uint8_t stopband;
	uint8_t overtuned = 0;
	uint8_t rfcsr24 = (bw40) ? 0x27 : 0x07;

	rt2800_rfcsr_write(rt2x00dev, 24, rfcsr24);

	bbp = rt2800_bbp_read(rt2x00dev, 4);
	rt2x00_set_field8(&bbp, BBP4_BANDWIDTH, 2 * bw40);
	rt2800_bbp_write(rt2x00dev, 4, bbp);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 31);
	rt2x00_set_field8(&rfcsr, RFCSR31_RX_H20M, bw40);
	rt2800_rfcsr_write(rt2x00dev, 31, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 22);
	rt2x00_set_field8(&rfcsr, RFCSR22_BASEBAND_LOOPBACK, 1);
	rt2800_rfcsr_write(rt2x00dev, 22, rfcsr);

	/*
	 * Set power & frequency of passband test tone
	 */
	rt2800_bbp_write(rt2x00dev, 24, 0);

	for (i = 0; i < 100; i++) {
		rt2800_bbp_write(rt2x00dev, 25, 0x90);
		msleep(1);

		passband = rt2800_bbp_read(rt2x00dev, 55);
		if (passband)
			break;
	}

	/*
	 * Set power & frequency of stopband test tone
	 */
	rt2800_bbp_write(rt2x00dev, 24, 0x06);

	for (i = 0; i < 100; i++) {
		rt2800_bbp_write(rt2x00dev, 25, 0x90);
		msleep(1);

		stopband = rt2800_bbp_read(rt2x00dev, 55);

		if ((passband - stopband) <= filter_target) {
			rfcsr24++;
			overtuned += ((passband - stopband) == filter_target);
		} else
			break;

		rt2800_rfcsr_write(rt2x00dev, 24, rfcsr24);
	}

	rfcsr24 -= !!overtuned;

	rt2800_rfcsr_write(rt2x00dev, 24, rfcsr24);
	return rfcsr24;
}

static void rt2800_rf_init_calibration(struct rt2x00_dev *rt2x00dev,
				       const unsigned int rf_reg)
{
	uint8_t rfcsr;

	rfcsr = rt2800_rfcsr_read(rt2x00dev, rf_reg);
	rt2x00_set_field8(&rfcsr, FIELD8(0x80), 1);
	rt2800_rfcsr_write(rt2x00dev, rf_reg, rfcsr);
	msleep(1);
	rt2x00_set_field8(&rfcsr, FIELD8(0x80), 0);
	rt2800_rfcsr_write(rt2x00dev, rf_reg, rfcsr);
}

static void rt2800_rx_filter_calibration(struct rt2x00_dev *rt2x00dev)
{
	struct rt2800_drv_data *drv_data = rt2x00dev->drv_data;
	uint8_t filter_tgt_bw20;
	uint8_t filter_tgt_bw40;
	uint8_t rfcsr, bbp;

	/*
	 * TODO: sync filter_tgt values with vendor driver
	 */
	if (rt2x00_rt(rt2x00dev, RT3070)) {
		filter_tgt_bw20 = 0x16;
		filter_tgt_bw40 = 0x19;
	} else {
		filter_tgt_bw20 = 0x13;
		filter_tgt_bw40 = 0x15;
	}

	drv_data->calibration_bw20 =
		rt2800_init_rx_filter(rt2x00dev, false, filter_tgt_bw20);
	drv_data->calibration_bw40 =
		rt2800_init_rx_filter(rt2x00dev, true, filter_tgt_bw40);

	/*
	 * Save BBP 25 & 26 values for later use in channel switching (for 3052)
	 */
	drv_data->bbp25 = rt2800_bbp_read(rt2x00dev, 25);
	drv_data->bbp26 = rt2800_bbp_read(rt2x00dev, 26);

	/*
	 * Set back to initial state
	 */
	rt2800_bbp_write(rt2x00dev, 24, 0);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 22);
	rt2x00_set_field8(&rfcsr, RFCSR22_BASEBAND_LOOPBACK, 0);
	rt2800_rfcsr_write(rt2x00dev, 22, rfcsr);

	/*
	 * Set BBP back to BW20
	 */
	bbp = rt2800_bbp_read(rt2x00dev, 4);
	rt2x00_set_field8(&bbp, BBP4_BANDWIDTH, 0);
	rt2800_bbp_write(rt2x00dev, 4, bbp);
}

static void rt2800_normal_mode_setup_3xxx(struct rt2x00_dev *rt2x00dev)
{
	struct rt2800_drv_data *drv_data = rt2x00dev->drv_data;
	uint8_t min_gain, rfcsr, bbp;
	uint16_t eeprom;

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 17);

	rt2x00_set_field8(&rfcsr, RFCSR17_TX_LO1_EN, 0);
	if (rt2x00_rt(rt2x00dev, RT3070) ||
	    rt2x00_rt_rev_lt(rt2x00dev, RT3071, REV_RT3071E) ||
	    rt2x00_rt_rev_lt(rt2x00dev, RT3090, REV_RT3090E) ||
	    rt2x00_rt_rev_lt(rt2x00dev, RT3390, REV_RT3390E)) {
		if (!rt2x00_has_cap_external_lna_bg(rt2x00dev))
			rt2x00_set_field8(&rfcsr, RFCSR17_R, 1);
	}

	min_gain = rt2x00_rt(rt2x00dev, RT3070) ? 1 : 2;
	if (drv_data->txmixer_gain_24g >= min_gain) {
		rt2x00_set_field8(&rfcsr, RFCSR17_TXMIXER_GAIN,
				  drv_data->txmixer_gain_24g);
	}

	rt2800_rfcsr_write(rt2x00dev, 17, rfcsr);

	if (rt2x00_rt(rt2x00dev, RT3090)) {
		/*  Turn off unused DAC1 and ADC1 to reduce power consumption */
		bbp = rt2800_bbp_read(rt2x00dev, 138);
		eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF0);
		if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_RXPATH) == 1)
			rt2x00_set_field8(&bbp, BBP138_RX_ADC1, 0);
		if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_TXPATH) == 1)
			rt2x00_set_field8(&bbp, BBP138_TX_DAC1, 1);
		rt2800_bbp_write(rt2x00dev, 138, bbp);
	}

	if (rt2x00_rt(rt2x00dev, RT3070)) {
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 27);
		if (rt2x00_rt_rev_lt(rt2x00dev, RT3070, REV_RT3070F))
			rt2x00_set_field8(&rfcsr, RFCSR27_R1, 3);
		else
			rt2x00_set_field8(&rfcsr, RFCSR27_R1, 0);
		rt2x00_set_field8(&rfcsr, RFCSR27_R2, 0);
		rt2x00_set_field8(&rfcsr, RFCSR27_R3, 0);
		rt2x00_set_field8(&rfcsr, RFCSR27_R4, 0);
		rt2800_rfcsr_write(rt2x00dev, 27, rfcsr);
	} else if (rt2x00_rt(rt2x00dev, RT3071) ||
		   rt2x00_rt(rt2x00dev, RT3090) ||
		   rt2x00_rt(rt2x00dev, RT3390)) {
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
		rt2x00_set_field8(&rfcsr, RFCSR1_RF_BLOCK_EN, 1);
		rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 0);
		rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 0);
		rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, 1);
		rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, 1);
		rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

		rfcsr = rt2800_rfcsr_read(rt2x00dev, 15);
		rt2x00_set_field8(&rfcsr, RFCSR15_TX_LO2_EN, 0);
		rt2800_rfcsr_write(rt2x00dev, 15, rfcsr);

		rfcsr = rt2800_rfcsr_read(rt2x00dev, 20);
		rt2x00_set_field8(&rfcsr, RFCSR20_RX_LO1_EN, 0);
		rt2800_rfcsr_write(rt2x00dev, 20, rfcsr);

		rfcsr = rt2800_rfcsr_read(rt2x00dev, 21);
		rt2x00_set_field8(&rfcsr, RFCSR21_RX_LO2_EN, 0);
		rt2800_rfcsr_write(rt2x00dev, 21, rfcsr);
	}
}

static void rt2800_normal_mode_setup_3593(struct rt2x00_dev *rt2x00dev)
{
	struct rt2800_drv_data *drv_data = rt2x00dev->drv_data;
	uint8_t rfcsr;
	uint8_t tx_gain;

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 50);
	rt2x00_set_field8(&rfcsr, RFCSR50_TX_LO2_EN, 0);
	rt2800_rfcsr_write(rt2x00dev, 50, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 51);
	tx_gain = rt2x00_get_field8(drv_data->txmixer_gain_24g,
				    RFCSR17_TXMIXER_GAIN);
	rt2x00_set_field8(&rfcsr, RFCSR51_BITS24, tx_gain);
	rt2800_rfcsr_write(rt2x00dev, 51, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 38);
	rt2x00_set_field8(&rfcsr, RFCSR38_RX_LO1_EN, 0);
	rt2800_rfcsr_write(rt2x00dev, 38, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 39);
	rt2x00_set_field8(&rfcsr, RFCSR39_RX_LO2_EN, 0);
	rt2800_rfcsr_write(rt2x00dev, 39, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RF_BLOCK_EN, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_PLL_PD, 1);
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 30);
	rt2x00_set_field8(&rfcsr, RFCSR30_RX_VCM, 2);
	rt2800_rfcsr_write(rt2x00dev, 30, rfcsr);

	/* TODO: enable stream mode */
}

static void rt2800_normal_mode_setup_5xxx(struct rt2x00_dev *rt2x00dev)
{
	uint8_t reg;
	uint16_t eeprom;

	/*  Turn off unused DAC1 and ADC1 to reduce power consumption */
	reg = rt2800_bbp_read(rt2x00dev, 138);
	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF0);
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_RXPATH) == 1)
		rt2x00_set_field8(&reg, BBP138_RX_ADC1, 0);
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_TXPATH) == 1)
		rt2x00_set_field8(&reg, BBP138_TX_DAC1, 1);
	rt2800_bbp_write(rt2x00dev, 138, reg);

	reg = rt2800_rfcsr_read(rt2x00dev, 38);
	rt2x00_set_field8(&reg, RFCSR38_RX_LO1_EN, 0);
	rt2800_rfcsr_write(rt2x00dev, 38, reg);

	reg = rt2800_rfcsr_read(rt2x00dev, 39);
	rt2x00_set_field8(&reg, RFCSR39_RX_LO2_EN, 0);
	rt2800_rfcsr_write(rt2x00dev, 39, reg);

	rt2800_bbp4_mac_if_ctrl(rt2x00dev);

	reg = rt2800_rfcsr_read(rt2x00dev, 30);
	rt2x00_set_field8(&reg, RFCSR30_RX_VCM, 2);
	rt2800_rfcsr_write(rt2x00dev, 30, reg);
}

static void rt2800_init_rfcsr_305x_soc(struct rt2x00_dev *rt2x00dev)
{
	rt2800_rf_init_calibration(rt2x00dev, 30);

	rt2800_rfcsr_write(rt2x00dev, 0, 0x50);
	rt2800_rfcsr_write(rt2x00dev, 1, 0x01);
	rt2800_rfcsr_write(rt2x00dev, 2, 0xf7);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x75);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x50);
	rt2800_rfcsr_write(rt2x00dev, 8, 0x39);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x0f);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x60);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x21);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x75);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x75);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x90);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x58);
	rt2800_rfcsr_write(rt2x00dev, 16, 0xb3);
	rt2800_rfcsr_write(rt2x00dev, 17, 0x92);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x2c);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 20, 0xba);
	rt2800_rfcsr_write(rt2x00dev, 21, 0xdb);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x31);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x01);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x25);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x23);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x13);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x83);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x00);
}

static void rt2800_init_rfcsr_30xx(struct rt2x00_dev *rt2x00dev)
{
	uint8_t rfcsr;
	uint16_t eeprom;
	uint32_t reg;

	/* XXX vendor driver do this only for 3070 */
	rt2800_rf_init_calibration(rt2x00dev, 30);

	rt2800_rfcsr_write(rt2x00dev, 4, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x60);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x0f);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x41);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x21);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x7b);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x90);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x58);
	rt2800_rfcsr_write(rt2x00dev, 16, 0xb3);
	rt2800_rfcsr_write(rt2x00dev, 17, 0x92);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x2c);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 20, 0xba);
	rt2800_rfcsr_write(rt2x00dev, 21, 0xdb);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x16);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x1f);

	if (rt2x00_rt_rev_lt(rt2x00dev, RT3070, REV_RT3070F)) {
		reg = rt2800_register_read(rt2x00dev, LDO_CFG0);
		rt2x00_set_field32(&reg, LDO_CFG0_BGSEL, 1);
		rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 3);
		rt2800_register_write(rt2x00dev, LDO_CFG0, reg);
	} else if (rt2x00_rt(rt2x00dev, RT3071) ||
		   rt2x00_rt(rt2x00dev, RT3090)) {
		rt2800_rfcsr_write(rt2x00dev, 31, 0x14);

		rfcsr = rt2800_rfcsr_read(rt2x00dev, 6);
		rt2x00_set_field8(&rfcsr, RFCSR6_R2, 1);
		rt2800_rfcsr_write(rt2x00dev, 6, rfcsr);

		reg = rt2800_register_read(rt2x00dev, LDO_CFG0);
		rt2x00_set_field32(&reg, LDO_CFG0_BGSEL, 1);
		if (rt2x00_rt_rev_lt(rt2x00dev, RT3071, REV_RT3071E) ||
		    rt2x00_rt_rev_lt(rt2x00dev, RT3090, REV_RT3090E)) {
			eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);
			if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_DAC_TEST))
				rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 3);
			else
				rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 0);
		}
		rt2800_register_write(rt2x00dev, LDO_CFG0, reg);

		reg = rt2800_register_read(rt2x00dev, GPIO_SWITCH);
		rt2x00_set_field32(&reg, GPIO_SWITCH_5, 0);
		rt2800_register_write(rt2x00dev, GPIO_SWITCH, reg);
	}

	rt2800_rx_filter_calibration(rt2x00dev);

	if (rt2x00_rt_rev_lt(rt2x00dev, RT3070, REV_RT3070F) ||
	    rt2x00_rt_rev_lt(rt2x00dev, RT3071, REV_RT3071E) ||
	    rt2x00_rt_rev_lt(rt2x00dev, RT3090, REV_RT3090E))
		rt2800_rfcsr_write(rt2x00dev, 27, 0x03);

	rt2800_led_open_drain_enable(rt2x00dev);
	rt2800_normal_mode_setup_3xxx(rt2x00dev);
}

static void rt2800_init_rfcsr_3290(struct rt2x00_dev *rt2x00dev)
{
	uint8_t rfcsr;

	rt2800_rf_init_calibration(rt2x00dev, 2);

	rt2800_rfcsr_write(rt2x00dev, 1, 0x0f);
	rt2800_rfcsr_write(rt2x00dev, 2, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 6, 0xa0);
	rt2800_rfcsr_write(rt2x00dev, 8, 0xf3);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x53);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x4a);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x46);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x9f);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x83);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x82);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x09);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 33, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x05);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 36, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x85);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x1b);
	rt2800_rfcsr_write(rt2x00dev, 40, 0x0b);
	rt2800_rfcsr_write(rt2x00dev, 41, 0xbb);
	rt2800_rfcsr_write(rt2x00dev, 42, 0xd5);
	rt2800_rfcsr_write(rt2x00dev, 43, 0x7b);
	rt2800_rfcsr_write(rt2x00dev, 44, 0x0e);
	rt2800_rfcsr_write(rt2x00dev, 45, 0xa2);
	rt2800_rfcsr_write(rt2x00dev, 46, 0x73);
	rt2800_rfcsr_write(rt2x00dev, 47, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 48, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 49, 0x98);
	rt2800_rfcsr_write(rt2x00dev, 52, 0x38);
	rt2800_rfcsr_write(rt2x00dev, 53, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 54, 0x78);
	rt2800_rfcsr_write(rt2x00dev, 55, 0x43);
	rt2800_rfcsr_write(rt2x00dev, 56, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 57, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 58, 0x7f);
	rt2800_rfcsr_write(rt2x00dev, 59, 0x09);
	rt2800_rfcsr_write(rt2x00dev, 60, 0x45);
	rt2800_rfcsr_write(rt2x00dev, 61, 0xc1);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 29);
	rt2x00_set_field8(&rfcsr, RFCSR29_RSSI_GAIN, 3);
	rt2800_rfcsr_write(rt2x00dev, 29, rfcsr);

	rt2800_led_open_drain_enable(rt2x00dev);
	rt2800_normal_mode_setup_3xxx(rt2x00dev);
}

static void rt2800_init_rfcsr_3352(struct rt2x00_dev *rt2x00dev)
{
	int tx0_ext_pa = test_bit(CAPABILITY_EXTERNAL_PA_TX0,
				  &rt2x00dev->cap_flags);
	int tx1_ext_pa = test_bit(CAPABILITY_EXTERNAL_PA_TX1,
				  &rt2x00dev->cap_flags);
	uint8_t rfcsr;

	rt2800_rf_init_calibration(rt2x00dev, 30);

	rt2800_rfcsr_write(rt2x00dev, 0, 0xf0);
	rt2800_rfcsr_write(rt2x00dev, 1, 0x23);
	rt2800_rfcsr_write(rt2x00dev, 2, 0x50);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x18);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x33);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 8, 0xf1);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 10, 0xd2);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x42);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x1c);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x5a);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x01);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x45);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 20, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 21, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 33, 0x00);
	rfcsr = 0x01;
	if (tx0_ext_pa)
		rt2x00_set_field8(&rfcsr, RFCSR34_TX0_EXT_PA, 1);
	if (tx1_ext_pa)
		rt2x00_set_field8(&rfcsr, RFCSR34_TX1_EXT_PA, 1);
	rt2800_rfcsr_write(rt2x00dev, 34, rfcsr);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 36, 0xbd);
	rt2800_rfcsr_write(rt2x00dev, 37, 0x3c);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x5f);
	rt2800_rfcsr_write(rt2x00dev, 39, 0xc5);
	rt2800_rfcsr_write(rt2x00dev, 40, 0x33);
	rfcsr = 0x52;
	if (!tx0_ext_pa) {
		rt2x00_set_field8(&rfcsr, RFCSR41_BIT1, 1);
		rt2x00_set_field8(&rfcsr, RFCSR41_BIT4, 1);
	}
	rt2800_rfcsr_write(rt2x00dev, 41, rfcsr);
	rfcsr = 0x52;
	if (!tx1_ext_pa) {
		rt2x00_set_field8(&rfcsr, RFCSR42_BIT1, 1);
		rt2x00_set_field8(&rfcsr, RFCSR42_BIT4, 1);
	}
	rt2800_rfcsr_write(rt2x00dev, 42, rfcsr);
	rt2800_rfcsr_write(rt2x00dev, 43, 0xdb);
	rt2800_rfcsr_write(rt2x00dev, 44, 0xdb);
	rt2800_rfcsr_write(rt2x00dev, 45, 0xdb);
	rt2800_rfcsr_write(rt2x00dev, 46, 0xdd);
	rt2800_rfcsr_write(rt2x00dev, 47, 0x0d);
	rt2800_rfcsr_write(rt2x00dev, 48, 0x14);
	rt2800_rfcsr_write(rt2x00dev, 49, 0x00);
	rfcsr = 0x2d;
	if (tx0_ext_pa)
		rt2x00_set_field8(&rfcsr, RFCSR50_TX0_EXT_PA, 1);
	if (tx1_ext_pa)
		rt2x00_set_field8(&rfcsr, RFCSR50_TX1_EXT_PA, 1);
	rt2800_rfcsr_write(rt2x00dev, 50, rfcsr);
	rt2800_rfcsr_write(rt2x00dev, 51, (tx0_ext_pa ? 0x52 : 0x7f));
	rt2800_rfcsr_write(rt2x00dev, 52, (tx0_ext_pa ? 0xc0 : 0x00));
	rt2800_rfcsr_write(rt2x00dev, 53, (tx0_ext_pa ? 0xd2 : 0x52));
	rt2800_rfcsr_write(rt2x00dev, 54, (tx0_ext_pa ? 0xc0 : 0x1b));
	rt2800_rfcsr_write(rt2x00dev, 55, (tx1_ext_pa ? 0x52 : 0x7f));
	rt2800_rfcsr_write(rt2x00dev, 56, (tx1_ext_pa ? 0xc0 : 0x00));
	rt2800_rfcsr_write(rt2x00dev, 57, (tx0_ext_pa ? 0x49 : 0x52));
	rt2800_rfcsr_write(rt2x00dev, 58, (tx1_ext_pa ? 0xc0 : 0x1b));
	rt2800_rfcsr_write(rt2x00dev, 59, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 60, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 61, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 62, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 63, 0x00);

	rt2800_rx_filter_calibration(rt2x00dev);
	rt2800_led_open_drain_enable(rt2x00dev);
	rt2800_normal_mode_setup_3xxx(rt2x00dev);
}

static void rt2800_init_rfcsr_3390(struct rt2x00_dev *rt2x00dev)
{
	uint32_t reg;

	rt2800_rf_init_calibration(rt2x00dev, 30);

	rt2800_rfcsr_write(rt2x00dev, 0, 0xa0);
	rt2800_rfcsr_write(rt2x00dev, 1, 0xe1);
	rt2800_rfcsr_write(rt2x00dev, 2, 0xf1);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x62);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x8b);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x42);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x34);
	rt2800_rfcsr_write(rt2x00dev, 8, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 9, 0xc0);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x61);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x21);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x3b);
	rt2800_rfcsr_write(rt2x00dev, 13, 0xe0);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x90);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x53);
	rt2800_rfcsr_write(rt2x00dev, 16, 0xe0);
	rt2800_rfcsr_write(rt2x00dev, 17, 0x94);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x5c);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x4a);
	rt2800_rfcsr_write(rt2x00dev, 20, 0xb2);
	rt2800_rfcsr_write(rt2x00dev, 21, 0xf6);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x14);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x3d);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x85);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x41);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x8f);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x0f);

	reg = rt2800_register_read(rt2x00dev, GPIO_SWITCH);
	rt2x00_set_field32(&reg, GPIO_SWITCH_5, 0);
	rt2800_register_write(rt2x00dev, GPIO_SWITCH, reg);

	rt2800_rx_filter_calibration(rt2x00dev);

	if (rt2x00_rt_rev_lt(rt2x00dev, RT3390, REV_RT3390E))
		rt2800_rfcsr_write(rt2x00dev, 27, 0x03);

	rt2800_led_open_drain_enable(rt2x00dev);
	rt2800_normal_mode_setup_3xxx(rt2x00dev);
}

static void rt2800_init_rfcsr_3572(struct rt2x00_dev *rt2x00dev)
{
	uint8_t rfcsr;
	uint32_t reg;

	rt2800_rf_init_calibration(rt2x00dev, 30);

	rt2800_rfcsr_write(rt2x00dev, 0, 0x70);
	rt2800_rfcsr_write(rt2x00dev, 1, 0x81);
	rt2800_rfcsr_write(rt2x00dev, 2, 0xf1);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x4c);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x05);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x4a);
	rt2800_rfcsr_write(rt2x00dev, 7, 0xd8);
	rt2800_rfcsr_write(rt2x00dev, 9, 0xc3);
	rt2800_rfcsr_write(rt2x00dev, 10, 0xf1);
	rt2800_rfcsr_write(rt2x00dev, 11, 0xb9);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x70);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x65);
	rt2800_rfcsr_write(rt2x00dev, 14, 0xa0);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x53);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x4c);
	rt2800_rfcsr_write(rt2x00dev, 17, 0x23);
	rt2800_rfcsr_write(rt2x00dev, 18, 0xac);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x93);
	rt2800_rfcsr_write(rt2x00dev, 20, 0xb3);
	rt2800_rfcsr_write(rt2x00dev, 21, 0xd0);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x3c);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x16);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x15);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x85);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x9b);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x09);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x10);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 6);
	rt2x00_set_field8(&rfcsr, RFCSR6_R2, 1);
	rt2800_rfcsr_write(rt2x00dev, 6, rfcsr);

	reg = rt2800_register_read(rt2x00dev, LDO_CFG0);
	rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 3);
	rt2x00_set_field32(&reg, LDO_CFG0_BGSEL, 1);
	rt2800_register_write(rt2x00dev, LDO_CFG0, reg);
	msleep(1);
	reg = rt2800_register_read(rt2x00dev, LDO_CFG0);
	rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 0);
	rt2x00_set_field32(&reg, LDO_CFG0_BGSEL, 1);
	rt2800_register_write(rt2x00dev, LDO_CFG0, reg);

	rt2800_rx_filter_calibration(rt2x00dev);
	rt2800_led_open_drain_enable(rt2x00dev);
	rt2800_normal_mode_setup_3xxx(rt2x00dev);
}

static void rt3593_post_bbp_init(struct rt2x00_dev *rt2x00dev)
{
	uint8_t bbp;
	bool txbf_enabled = false; /* FIXME */

	bbp = rt2800_bbp_read(rt2x00dev, 105);
	if (rt2x00dev->default_ant.rx_chain_num == 1)
		rt2x00_set_field8(&bbp, BBP105_MLD, 0);
	else
		rt2x00_set_field8(&bbp, BBP105_MLD, 1);
	rt2800_bbp_write(rt2x00dev, 105, bbp);

	rt2800_bbp4_mac_if_ctrl(rt2x00dev);

	rt2800_bbp_write(rt2x00dev, 92, 0x02);
	rt2800_bbp_write(rt2x00dev, 82, 0x82);
	rt2800_bbp_write(rt2x00dev, 106, 0x05);
	rt2800_bbp_write(rt2x00dev, 104, 0x92);
	rt2800_bbp_write(rt2x00dev, 88, 0x90);
	rt2800_bbp_write(rt2x00dev, 148, 0xc8);
	rt2800_bbp_write(rt2x00dev, 47, 0x48);
	rt2800_bbp_write(rt2x00dev, 120, 0x50);

	if (txbf_enabled)
		rt2800_bbp_write(rt2x00dev, 163, 0xbd);
	else
		rt2800_bbp_write(rt2x00dev, 163, 0x9d);

	/* SNR mapping */
	rt2800_bbp_write(rt2x00dev, 142, 6);
	rt2800_bbp_write(rt2x00dev, 143, 160);
	rt2800_bbp_write(rt2x00dev, 142, 7);
	rt2800_bbp_write(rt2x00dev, 143, 161);
	rt2800_bbp_write(rt2x00dev, 142, 8);
	rt2800_bbp_write(rt2x00dev, 143, 162);

	/* ADC/DAC control */
	rt2800_bbp_write(rt2x00dev, 31, 0x08);

	/* RX AGC energy lower bound in log2 */
	rt2800_bbp_write(rt2x00dev, 68, 0x0b);

	/* FIXME: BBP 105 owerwrite? */
	rt2800_bbp_write(rt2x00dev, 105, 0x04);

}

static void rt2800_init_rfcsr_3593(struct rt2x00_dev *rt2x00dev)
{
	struct rt2800_drv_data *drv_data = rt2x00dev->drv_data;
	uint32_t reg;
	uint8_t rfcsr;

	/* Disable GPIO #4 and #7 function for LAN PE control */
	reg = rt2800_register_read(rt2x00dev, GPIO_SWITCH);
	rt2x00_set_field32(&reg, GPIO_SWITCH_4, 0);
	rt2x00_set_field32(&reg, GPIO_SWITCH_7, 0);
	rt2800_register_write(rt2x00dev, GPIO_SWITCH, reg);

	/* Initialize default register values */
	rt2800_rfcsr_write(rt2x00dev, 1, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 8, 0xf1);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 10, 0xd3);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x4e);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x78);
	rt2800_rfcsr_write(rt2x00dev, 33, 0x3b);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x3c);
	rt2800_rfcsr_write(rt2x00dev, 35, 0xe0);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x86);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x23);
	rt2800_rfcsr_write(rt2x00dev, 44, 0xd3);
	rt2800_rfcsr_write(rt2x00dev, 45, 0xbb);
	rt2800_rfcsr_write(rt2x00dev, 46, 0x60);
	rt2800_rfcsr_write(rt2x00dev, 49, 0x8e);
	rt2800_rfcsr_write(rt2x00dev, 50, 0x86);
	rt2800_rfcsr_write(rt2x00dev, 51, 0x75);
	rt2800_rfcsr_write(rt2x00dev, 52, 0x45);
	rt2800_rfcsr_write(rt2x00dev, 53, 0x18);
	rt2800_rfcsr_write(rt2x00dev, 54, 0x18);
	rt2800_rfcsr_write(rt2x00dev, 55, 0x18);
	rt2800_rfcsr_write(rt2x00dev, 56, 0xdb);
	rt2800_rfcsr_write(rt2x00dev, 57, 0x6e);

	/* Initiate calibration */
	/* TODO: use rt2800_rf_init_calibration ? */
	rfcsr = rt2800_rfcsr_read(rt2x00dev, 2);
	rt2x00_set_field8(&rfcsr, RFCSR2_RESCAL_EN, 1);
	rt2800_rfcsr_write(rt2x00dev, 2, rfcsr);

	rt2800_freq_cal_mode1(rt2x00dev);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 18);
	rt2x00_set_field8(&rfcsr, RFCSR18_XO_TUNE_BYPASS, 1);
	rt2800_rfcsr_write(rt2x00dev, 18, rfcsr);

	reg = rt2800_register_read(rt2x00dev, LDO_CFG0);
	rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 3);
	rt2x00_set_field32(&reg, LDO_CFG0_BGSEL, 1);
	rt2800_register_write(rt2x00dev, LDO_CFG0, reg);
	usleep_range(1000, 1500);
	reg = rt2800_register_read(rt2x00dev, LDO_CFG0);
	rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 0);
	rt2800_register_write(rt2x00dev, LDO_CFG0, reg);

	/* Set initial values for RX filter calibration */
	drv_data->calibration_bw20 = 0x1f;
	drv_data->calibration_bw40 = 0x2f;

	/* Save BBP 25 & 26 values for later use in channel switching */
	drv_data->bbp25 = rt2800_bbp_read(rt2x00dev, 25);
	drv_data->bbp26 = rt2800_bbp_read(rt2x00dev, 26);

	rt2800_led_open_drain_enable(rt2x00dev);
	rt2800_normal_mode_setup_3593(rt2x00dev);

	rt3593_post_bbp_init(rt2x00dev);

	/* TODO: enable stream mode support */
}

static void rt2800_init_rfcsr_5350(struct rt2x00_dev *rt2x00dev)
{
	rt2800_rfcsr_write(rt2x00dev, 0, 0xf0);
	rt2800_rfcsr_write(rt2x00dev, 1, 0x23);
	rt2800_rfcsr_write(rt2x00dev, 2, 0x50);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x49);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 6, 0xe0);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 8, 0xf1);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x53);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x4a);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x46);
	if (rt2800_clk_is_20mhz(rt2x00dev))
		rt2800_rfcsr_write(rt2x00dev, 13, 0x1f);
	else
		rt2800_rfcsr_write(rt2x00dev, 13, 0x9f);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 16, 0xc0);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 20, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 21, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 29, 0xd0);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 33, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x07);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 36, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 37, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x85);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x1b);
	rt2800_rfcsr_write(rt2x00dev, 40, 0x0b);
	rt2800_rfcsr_write(rt2x00dev, 41, 0xbb);
	rt2800_rfcsr_write(rt2x00dev, 42, 0xd5);
	rt2800_rfcsr_write(rt2x00dev, 43, 0x9b);
	rt2800_rfcsr_write(rt2x00dev, 44, 0x0c);
	rt2800_rfcsr_write(rt2x00dev, 45, 0xa6);
	rt2800_rfcsr_write(rt2x00dev, 46, 0x73);
	rt2800_rfcsr_write(rt2x00dev, 47, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 48, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 49, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 50, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 51, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 52, 0x38);
	rt2800_rfcsr_write(rt2x00dev, 53, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 54, 0x38);
	rt2800_rfcsr_write(rt2x00dev, 55, 0x43);
	rt2800_rfcsr_write(rt2x00dev, 56, 0x82);
	rt2800_rfcsr_write(rt2x00dev, 57, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 58, 0x39);
	rt2800_rfcsr_write(rt2x00dev, 59, 0x0b);
	rt2800_rfcsr_write(rt2x00dev, 60, 0x45);
	rt2800_rfcsr_write(rt2x00dev, 61, 0xd1);
	rt2800_rfcsr_write(rt2x00dev, 62, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 63, 0x00);
}

static void rt2800_init_rfcsr_3883(struct rt2x00_dev *rt2x00dev)
{
	uint8_t rfcsr;

	/* TODO: get the actual ECO value from the SoC */
	const unsigned int eco = 5;

	rt2800_rf_init_calibration(rt2x00dev, 2);

	rt2800_rfcsr_write(rt2x00dev, 0, 0xe0);
	rt2800_rfcsr_write(rt2x00dev, 1, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 2, 0x50);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 8, 0x5b);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 10, 0xd3);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x48);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x1a);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x00);

	/* RFCSR 17 will be initialized later based on the
	 * frequency offset stored in the EEPROM
	 */

	rt2800_rfcsr_write(rt2x00dev, 18, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 20, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 21, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 23, 0xc0);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 33, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 36, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 37, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x86);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x23);
	rt2800_rfcsr_write(rt2x00dev, 40, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 41, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 42, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 43, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 44, 0x93);
	rt2800_rfcsr_write(rt2x00dev, 45, 0xbb);
	rt2800_rfcsr_write(rt2x00dev, 46, 0x60);
	rt2800_rfcsr_write(rt2x00dev, 47, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 48, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 49, 0x8e);
	rt2800_rfcsr_write(rt2x00dev, 50, 0x86);
	rt2800_rfcsr_write(rt2x00dev, 51, 0x51);
	rt2800_rfcsr_write(rt2x00dev, 52, 0x05);
	rt2800_rfcsr_write(rt2x00dev, 53, 0x76);
	rt2800_rfcsr_write(rt2x00dev, 54, 0x76);
	rt2800_rfcsr_write(rt2x00dev, 55, 0x76);
	rt2800_rfcsr_write(rt2x00dev, 56, 0xdb);
	rt2800_rfcsr_write(rt2x00dev, 57, 0x3e);
	rt2800_rfcsr_write(rt2x00dev, 58, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 59, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 60, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 61, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 62, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 63, 0x00);

	/* TODO: rx filter calibration? */

	rt2800_bbp_write(rt2x00dev, 137, 0x0f);

	rt2800_bbp_write(rt2x00dev, 163, 0x9d);

	rt2800_bbp_write(rt2x00dev, 105, 0x05);

	rt2800_bbp_write(rt2x00dev, 179, 0x02);
	rt2800_bbp_write(rt2x00dev, 180, 0x00);
	rt2800_bbp_write(rt2x00dev, 182, 0x40);
	rt2800_bbp_write(rt2x00dev, 180, 0x01);
	rt2800_bbp_write(rt2x00dev, 182, 0x9c);

	rt2800_bbp_write(rt2x00dev, 179, 0x00);

	rt2800_bbp_write(rt2x00dev, 142, 0x04);
	rt2800_bbp_write(rt2x00dev, 143, 0x3b);
	rt2800_bbp_write(rt2x00dev, 142, 0x06);
	rt2800_bbp_write(rt2x00dev, 143, 0xa0);
	rt2800_bbp_write(rt2x00dev, 142, 0x07);
	rt2800_bbp_write(rt2x00dev, 143, 0xa1);
	rt2800_bbp_write(rt2x00dev, 142, 0x08);
	rt2800_bbp_write(rt2x00dev, 143, 0xa2);
	rt2800_bbp_write(rt2x00dev, 148, 0xc8);

	if (eco == 5) {
		rt2800_rfcsr_write(rt2x00dev, 32, 0xd8);
		rt2800_rfcsr_write(rt2x00dev, 33, 0x32);
	}

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 2);
	rt2x00_set_field8(&rfcsr, RFCSR2_RESCAL_BP, 0);
	rt2x00_set_field8(&rfcsr, RFCSR2_RESCAL_EN, 1);
	rt2800_rfcsr_write(rt2x00dev, 2, rfcsr);
	msleep(1);
	rt2x00_set_field8(&rfcsr, RFCSR2_RESCAL_EN, 0);
	rt2800_rfcsr_write(rt2x00dev, 2, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 1);
	rt2x00_set_field8(&rfcsr, RFCSR1_RF_BLOCK_EN, 1);
	rt2800_rfcsr_write(rt2x00dev, 1, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 6);
	rfcsr |= 0xc0;
	rt2800_rfcsr_write(rt2x00dev, 6, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 22);
	rfcsr |= 0x20;
	rt2800_rfcsr_write(rt2x00dev, 22, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 46);
	rfcsr |= 0x20;
	rt2800_rfcsr_write(rt2x00dev, 46, rfcsr);

	rfcsr = rt2800_rfcsr_read(rt2x00dev, 20);
	rfcsr &= ~0xee;
	rt2800_rfcsr_write(rt2x00dev, 20, rfcsr);
}

static void rt2800_init_rfcsr_5390(struct rt2x00_dev *rt2x00dev)
{
	rt2800_rf_init_calibration(rt2x00dev, 2);

	rt2800_rfcsr_write(rt2x00dev, 1, 0x0f);
	rt2800_rfcsr_write(rt2x00dev, 2, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x88);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x10);
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F))
		rt2800_rfcsr_write(rt2x00dev, 6, 0xe0);
	else
		rt2800_rfcsr_write(rt2x00dev, 6, 0xa0);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x53);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x4a);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x46);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x9f);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x00);

	rt2800_rfcsr_write(rt2x00dev, 20, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 21, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x00);
	if (rt2x00_is_usb(rt2x00dev) &&
	    rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F))
		rt2800_rfcsr_write(rt2x00dev, 25, 0x80);
	else
		rt2800_rfcsr_write(rt2x00dev, 25, 0xc0);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x09);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x10);

	rt2800_rfcsr_write(rt2x00dev, 30, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 33, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x07);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 36, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 37, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x85);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x1b);

	rt2800_rfcsr_write(rt2x00dev, 40, 0x0b);
	rt2800_rfcsr_write(rt2x00dev, 41, 0xbb);
	rt2800_rfcsr_write(rt2x00dev, 42, 0xd2);
	rt2800_rfcsr_write(rt2x00dev, 43, 0x9a);
	rt2800_rfcsr_write(rt2x00dev, 44, 0x0e);
	rt2800_rfcsr_write(rt2x00dev, 45, 0xa2);
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F))
		rt2800_rfcsr_write(rt2x00dev, 46, 0x73);
	else
		rt2800_rfcsr_write(rt2x00dev, 46, 0x7b);
	rt2800_rfcsr_write(rt2x00dev, 47, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 48, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 49, 0x94);

	rt2800_rfcsr_write(rt2x00dev, 52, 0x38);
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F))
		rt2800_rfcsr_write(rt2x00dev, 53, 0x00);
	else
		rt2800_rfcsr_write(rt2x00dev, 53, 0x84);
	rt2800_rfcsr_write(rt2x00dev, 54, 0x78);
	rt2800_rfcsr_write(rt2x00dev, 55, 0x44);
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F))
		rt2800_rfcsr_write(rt2x00dev, 56, 0x42);
	else
		rt2800_rfcsr_write(rt2x00dev, 56, 0x22);
	rt2800_rfcsr_write(rt2x00dev, 57, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 58, 0x7f);
	rt2800_rfcsr_write(rt2x00dev, 59, 0x8f);

	rt2800_rfcsr_write(rt2x00dev, 60, 0x45);
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390F)) {
		if (rt2x00_is_usb(rt2x00dev))
			rt2800_rfcsr_write(rt2x00dev, 61, 0xd1);
		else
			rt2800_rfcsr_write(rt2x00dev, 61, 0xd5);
	} else {
		if (rt2x00_is_usb(rt2x00dev))
			rt2800_rfcsr_write(rt2x00dev, 61, 0xdd);
		else
			rt2800_rfcsr_write(rt2x00dev, 61, 0xb5);
	}
	rt2800_rfcsr_write(rt2x00dev, 62, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 63, 0x00);

	rt2800_normal_mode_setup_5xxx(rt2x00dev);

	rt2800_led_open_drain_enable(rt2x00dev);
}

static void rt2800_init_rfcsr_5392(struct rt2x00_dev *rt2x00dev)
{
	rt2800_rf_init_calibration(rt2x00dev, 2);

	rt2800_rfcsr_write(rt2x00dev, 1, 0x17);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x88);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 6, 0xe0);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x53);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x4a);
	rt2800_rfcsr_write(rt2x00dev, 12, 0x46);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x9f);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x4d);
	rt2800_rfcsr_write(rt2x00dev, 20, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 21, 0x8d);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x0b);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x44);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x82);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x09);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x20);
	rt2800_rfcsr_write(rt2x00dev, 33, 0xC0);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x07);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 36, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 37, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x89);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x1b);
	rt2800_rfcsr_write(rt2x00dev, 40, 0x0f);
	rt2800_rfcsr_write(rt2x00dev, 41, 0xbb);
	rt2800_rfcsr_write(rt2x00dev, 42, 0xd5);
	rt2800_rfcsr_write(rt2x00dev, 43, 0x9b);
	rt2800_rfcsr_write(rt2x00dev, 44, 0x0e);
	rt2800_rfcsr_write(rt2x00dev, 45, 0xa2);
	rt2800_rfcsr_write(rt2x00dev, 46, 0x73);
	rt2800_rfcsr_write(rt2x00dev, 47, 0x0c);
	rt2800_rfcsr_write(rt2x00dev, 48, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 49, 0x94);
	rt2800_rfcsr_write(rt2x00dev, 50, 0x94);
	rt2800_rfcsr_write(rt2x00dev, 51, 0x3a);
	rt2800_rfcsr_write(rt2x00dev, 52, 0x48);
	rt2800_rfcsr_write(rt2x00dev, 53, 0x44);
	rt2800_rfcsr_write(rt2x00dev, 54, 0x38);
	rt2800_rfcsr_write(rt2x00dev, 55, 0x43);
	rt2800_rfcsr_write(rt2x00dev, 56, 0xa1);
	rt2800_rfcsr_write(rt2x00dev, 57, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 58, 0x39);
	rt2800_rfcsr_write(rt2x00dev, 59, 0x07);
	rt2800_rfcsr_write(rt2x00dev, 60, 0x45);
	rt2800_rfcsr_write(rt2x00dev, 61, 0x91);
	rt2800_rfcsr_write(rt2x00dev, 62, 0x39);
	rt2800_rfcsr_write(rt2x00dev, 63, 0x07);

	rt2800_normal_mode_setup_5xxx(rt2x00dev);

	rt2800_led_open_drain_enable(rt2x00dev);
}

static void rt2800_init_rfcsr_5592(struct rt2x00_dev *rt2x00dev)
{
	rt2800_rf_init_calibration(rt2x00dev, 30);

	rt2800_rfcsr_write(rt2x00dev, 1, 0x3F);
	rt2800_rfcsr_write(rt2x00dev, 3, 0x08);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 6, 0xE4);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x4D);
	rt2800_rfcsr_write(rt2x00dev, 20, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 21, 0x8D);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x82);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x10);
	rt2800_rfcsr_write(rt2x00dev, 33, 0xC0);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x07);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 47, 0x0C);
	rt2800_rfcsr_write(rt2x00dev, 53, 0x22);
	rt2800_rfcsr_write(rt2x00dev, 63, 0x07);

	rt2800_rfcsr_write(rt2x00dev, 2, 0x80);
	msleep(1);

	rt2800_freq_cal_mode1(rt2x00dev);

	/* Enable DC filter */
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5592, REV_RT5592C))
		rt2800_bbp_write(rt2x00dev, 103, 0xc0);

	rt2800_normal_mode_setup_5xxx(rt2x00dev);

	if (rt2x00_rt_rev_lt(rt2x00dev, RT5592, REV_RT5592C))
		rt2800_rfcsr_write(rt2x00dev, 27, 0x03);

	rt2800_led_open_drain_enable(rt2x00dev);
}

static void rt2800_bbp_core_soft_reset(struct rt2x00_dev *rt2x00dev,
				       bool set_bw, bool is_ht40)
{
	uint8_t bbp_val;

	bbp_val = rt2800_bbp_read(rt2x00dev, 21);
	bbp_val |= 0x1;
	rt2800_bbp_write(rt2x00dev, 21, bbp_val);
	usleep_range(100, 200);

	if (set_bw) {
		bbp_val = rt2800_bbp_read(rt2x00dev, 4);
		rt2x00_set_field8(&bbp_val, BBP4_BANDWIDTH, 2 * is_ht40);
		rt2800_bbp_write(rt2x00dev, 4, bbp_val);
		usleep_range(100, 200);
	}

	bbp_val = rt2800_bbp_read(rt2x00dev, 21);
	bbp_val &= (~0x1);
	rt2800_bbp_write(rt2x00dev, 21, bbp_val);
	usleep_range(100, 200);
}

static int rt2800_rf_lp_config(struct rt2x00_dev *rt2x00dev, bool btxcal)
{
	uint8_t rf_val;

	if (btxcal)
		rt2800_register_write(rt2x00dev, RF_CONTROL0, 0x04);
	else
		rt2800_register_write(rt2x00dev, RF_CONTROL0, 0x02);

	rt2800_register_write(rt2x00dev, RF_BYPASS0, 0x06);

	rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 17);
	rf_val |= 0x80;
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 17, rf_val);

	if (btxcal) {
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 18, 0xC1);
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 19, 0x20);
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 20, 0x02);
		rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 3);
		rf_val &= (~0x3F);
		rf_val |= 0x3F;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 3, rf_val);
		rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 4);
		rf_val &= (~0x3F);
		rf_val |= 0x3F;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 4, rf_val);
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 5, 0x31);
	} else {
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 18, 0xF1);
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 19, 0x18);
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 20, 0x02);
		rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 3);
		rf_val &= (~0x3F);
		rf_val |= 0x34;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 3, rf_val);
		rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 4);
		rf_val &= (~0x3F);
		rf_val |= 0x34;
		rt2800_rfcsr_write_bank(rt2x00dev, 5, 4, rf_val);
	}

	return 0;
}

static char rt2800_lp_tx_filter_bw_cal(struct rt2x00_dev *rt2x00dev)
{
	unsigned int cnt;
	uint8_t bbp_val;
	char cal_val;

	rt2800_bbp_dcoc_write(rt2x00dev, 0, 0x82);

	cnt = 0;
	do {
		usleep_range(500, 2000);
		bbp_val = rt2800_bbp_read(rt2x00dev, 159);
		if (bbp_val == 0x02 || cnt == 20)
			break;

		cnt++;
	} while (cnt < 20);

	bbp_val = rt2800_bbp_dcoc_read(rt2x00dev, 0x39);
	cal_val = bbp_val & 0x7F;
	if (cal_val >= 0x40)
		cal_val -= 128;

	return cal_val;
}

static void rt2800_bw_filter_calibration(struct rt2x00_dev *rt2x00dev,
					 bool btxcal)
{
	struct rt2800_drv_data *drv_data = rt2x00dev->drv_data;
	uint8_t tx_agc_fc = 0, rx_agc_fc = 0, cmm_agc_fc;
	uint8_t filter_target;
	uint8_t tx_filter_target_20m = 0x09, tx_filter_target_40m = 0x02;
	uint8_t rx_filter_target_20m = 0x27, rx_filter_target_40m = 0x31;
	int loop = 0, is_ht40, cnt;
	uint8_t bbp_val, rf_val;
	char cal_r32_init, cal_r32_val, cal_diff;
	uint8_t saverfb5r00, saverfb5r01, saverfb5r03, saverfb5r04, saverfb5r05;
	uint8_t saverfb5r06, saverfb5r07;
	uint8_t saverfb5r08, saverfb5r17, saverfb5r18, saverfb5r19, saverfb5r20;
	uint8_t saverfb5r37, saverfb5r38, saverfb5r39, saverfb5r40, saverfb5r41;
	uint8_t saverfb5r42, saverfb5r43, saverfb5r44, saverfb5r45, saverfb5r46;
	uint8_t saverfb5r58, saverfb5r59;
	uint8_t savebbp159r0, savebbp159r2, savebbpr23;
	uint32_t MAC_RF_CONTROL0, MAC_RF_BYPASS0;

	/* Save MAC registers */
	MAC_RF_CONTROL0 = rt2800_register_read(rt2x00dev, RF_CONTROL0);
	MAC_RF_BYPASS0 = rt2800_register_read(rt2x00dev, RF_BYPASS0);

	/* save BBP registers */
	savebbpr23 = rt2800_bbp_read(rt2x00dev, 23);

	savebbp159r0 = rt2800_bbp_dcoc_read(rt2x00dev, 0);
	savebbp159r2 = rt2800_bbp_dcoc_read(rt2x00dev, 2);

	/* Save RF registers */
	saverfb5r00 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 0);
	saverfb5r01 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 1);
	saverfb5r03 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 3);
	saverfb5r04 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 4);
	saverfb5r05 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 5);
	saverfb5r06 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 6);
	saverfb5r07 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 7);
	saverfb5r08 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 8);
	saverfb5r17 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 17);
	saverfb5r18 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 18);
	saverfb5r19 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 19);
	saverfb5r20 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 20);

	saverfb5r37 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 37);
	saverfb5r38 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 38);
	saverfb5r39 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 39);
	saverfb5r40 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 40);
	saverfb5r41 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 41);
	saverfb5r42 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 42);
	saverfb5r43 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 43);
	saverfb5r44 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 44);
	saverfb5r45 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 45);
	saverfb5r46 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 46);

	saverfb5r58 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 58);
	saverfb5r59 = rt2800_rfcsr_read_bank(rt2x00dev, 5, 59);

	rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 0);
	rf_val |= 0x3;
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 0, rf_val);

	rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 1);
	rf_val |= 0x1;
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 1, rf_val);

	cnt = 0;
	do {
		usleep_range(500, 2000);
		rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 1);
		if (((rf_val & 0x1) == 0x00) || (cnt == 40))
			break;
		cnt++;
	} while (cnt < 40);

	rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 0);
	rf_val &= (~0x3);
	rf_val |= 0x1;
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 0, rf_val);

	/* I-3 */
	bbp_val = rt2800_bbp_read(rt2x00dev, 23);
	bbp_val &= (~0x1F);
	bbp_val |= 0x10;
	rt2800_bbp_write(rt2x00dev, 23, bbp_val);

	do {
		/* I-4,5,6,7,8,9 */
		if (loop == 0) {
			is_ht40 = false;

			if (btxcal)
				filter_target = tx_filter_target_20m;
			else
				filter_target = rx_filter_target_20m;
		} else {
			is_ht40 = true;

			if (btxcal)
				filter_target = tx_filter_target_40m;
			else
				filter_target = rx_filter_target_40m;
		}

		rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 8);
		rf_val &= (~0x04);
		if (loop == 1)
			rf_val |= 0x4;

		rt2800_rfcsr_write_bank(rt2x00dev, 5, 8, rf_val);

		rt2800_bbp_core_soft_reset(rt2x00dev, true, is_ht40);

		rt2800_rf_lp_config(rt2x00dev, btxcal);
		if (btxcal) {
			tx_agc_fc = 0;
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 58);
			rf_val &= (~0x7F);
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 58, rf_val);
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 59);
			rf_val &= (~0x7F);
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 59, rf_val);
		} else {
			rx_agc_fc = 0;
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 6);
			rf_val &= (~0x7F);
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 6, rf_val);
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 7);
			rf_val &= (~0x7F);
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 7, rf_val);
		}

		usleep_range(1000, 2000);

		bbp_val = rt2800_bbp_dcoc_read(rt2x00dev, 2);
		bbp_val &= (~0x6);
		rt2800_bbp_dcoc_write(rt2x00dev, 2, bbp_val);

		rt2800_bbp_core_soft_reset(rt2x00dev, false, is_ht40);

		cal_r32_init = rt2800_lp_tx_filter_bw_cal(rt2x00dev);

		bbp_val = rt2800_bbp_dcoc_read(rt2x00dev, 2);
		bbp_val |= 0x6;
		rt2800_bbp_dcoc_write(rt2x00dev, 2, bbp_val);
do_cal:
		if (btxcal) {
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 58);
			rf_val &= (~0x7F);
			rf_val |= tx_agc_fc;
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 58, rf_val);
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 59);
			rf_val &= (~0x7F);
			rf_val |= tx_agc_fc;
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 59, rf_val);
		} else {
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 6);
			rf_val &= (~0x7F);
			rf_val |= rx_agc_fc;
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 6, rf_val);
			rf_val = rt2800_rfcsr_read_bank(rt2x00dev, 5, 7);
			rf_val &= (~0x7F);
			rf_val |= rx_agc_fc;
			rt2800_rfcsr_write_bank(rt2x00dev, 5, 7, rf_val);
		}

		usleep_range(500, 1000);

		rt2800_bbp_core_soft_reset(rt2x00dev, false, is_ht40);

		cal_r32_val = rt2800_lp_tx_filter_bw_cal(rt2x00dev);

		cal_diff = cal_r32_init - cal_r32_val;

		if (btxcal)
			cmm_agc_fc = tx_agc_fc;
		else
			cmm_agc_fc = rx_agc_fc;

		if (((cal_diff > filter_target) && (cmm_agc_fc == 0)) ||
		    ((cal_diff < filter_target) && (cmm_agc_fc == 0x3f))) {
			if (btxcal)
				tx_agc_fc = 0;
			else
				rx_agc_fc = 0;
		} else if ((cal_diff <= filter_target) && (cmm_agc_fc < 0x3f)) {
			if (btxcal)
				tx_agc_fc++;
			else
				rx_agc_fc++;
			goto do_cal;
		}

		if (btxcal) {
			if (loop == 0)
				drv_data->tx_calibration_bw20 = tx_agc_fc;
			else
				drv_data->tx_calibration_bw40 = tx_agc_fc;
		} else {
			if (loop == 0)
				drv_data->rx_calibration_bw20 = rx_agc_fc;
			else
				drv_data->rx_calibration_bw40 = rx_agc_fc;
		}

		loop++;
	} while (loop <= 1);

	rt2800_rfcsr_write_bank(rt2x00dev, 5, 0, saverfb5r00);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 1, saverfb5r01);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 3, saverfb5r03);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 4, saverfb5r04);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 5, saverfb5r05);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 6, saverfb5r06);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 7, saverfb5r07);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 8, saverfb5r08);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 17, saverfb5r17);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 18, saverfb5r18);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 19, saverfb5r19);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 20, saverfb5r20);

	rt2800_rfcsr_write_bank(rt2x00dev, 5, 37, saverfb5r37);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 38, saverfb5r38);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 39, saverfb5r39);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 40, saverfb5r40);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 41, saverfb5r41);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 42, saverfb5r42);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 43, saverfb5r43);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 44, saverfb5r44);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 45, saverfb5r45);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 46, saverfb5r46);

	rt2800_rfcsr_write_bank(rt2x00dev, 5, 58, saverfb5r58);
	rt2800_rfcsr_write_bank(rt2x00dev, 5, 59, saverfb5r59);

	rt2800_bbp_write(rt2x00dev, 23, savebbpr23);

	rt2800_bbp_dcoc_write(rt2x00dev, 0, savebbp159r0);
	rt2800_bbp_dcoc_write(rt2x00dev, 2, savebbp159r2);

	bbp_val = rt2800_bbp_read(rt2x00dev, 4);
	rt2x00_set_field8(&bbp_val, BBP4_BANDWIDTH,
			  2 * test_bit(CONFIG_CHANNEL_HT40, &rt2x00dev->flags));
	rt2800_bbp_write(rt2x00dev, 4, bbp_val);

	rt2800_register_write(rt2x00dev, RF_CONTROL0, MAC_RF_CONTROL0);
	rt2800_register_write(rt2x00dev, RF_BYPASS0, MAC_RF_BYPASS0);
}

static void rt2800_init_rfcsr_6352(struct rt2x00_dev *rt2x00dev)
{
	/* Initialize RF central register to default value */
	rt2800_rfcsr_write(rt2x00dev, 0, 0x02);
	rt2800_rfcsr_write(rt2x00dev, 1, 0x03);
	rt2800_rfcsr_write(rt2x00dev, 2, 0x33);
	rt2800_rfcsr_write(rt2x00dev, 3, 0xFF);
	rt2800_rfcsr_write(rt2x00dev, 4, 0x0C);
	rt2800_rfcsr_write(rt2x00dev, 5, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 6, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 7, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 8, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 9, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 10, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 11, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 12, rt2x00dev->freq_offset);
	rt2800_rfcsr_write(rt2x00dev, 13, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x40);
	rt2800_rfcsr_write(rt2x00dev, 15, 0x22);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x4C);
	rt2800_rfcsr_write(rt2x00dev, 17, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 20, 0xA0);
	rt2800_rfcsr_write(rt2x00dev, 21, 0x12);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x07);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x13);
	rt2800_rfcsr_write(rt2x00dev, 24, 0xFE);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x24);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x7A);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 29, 0x05);
	rt2800_rfcsr_write(rt2x00dev, 30, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 31, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 32, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 33, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 34, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 35, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 36, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 37, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 38, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 40, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 41, 0xD0);
	rt2800_rfcsr_write(rt2x00dev, 42, 0x5B);
	rt2800_rfcsr_write(rt2x00dev, 43, 0x00);

	rt2800_rfcsr_write(rt2x00dev, 11, 0x21);
	if (rt2800_clk_is_20mhz(rt2x00dev))
		rt2800_rfcsr_write(rt2x00dev, 13, 0x03);
	else
		rt2800_rfcsr_write(rt2x00dev, 13, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 14, 0x7C);
	rt2800_rfcsr_write(rt2x00dev, 16, 0x80);
	rt2800_rfcsr_write(rt2x00dev, 17, 0x99);
	rt2800_rfcsr_write(rt2x00dev, 18, 0x99);
	rt2800_rfcsr_write(rt2x00dev, 19, 0x09);
	rt2800_rfcsr_write(rt2x00dev, 20, 0x50);
	rt2800_rfcsr_write(rt2x00dev, 21, 0xB0);
	rt2800_rfcsr_write(rt2x00dev, 22, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 23, 0x06);
	rt2800_rfcsr_write(rt2x00dev, 24, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 25, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 26, 0x5D);
	rt2800_rfcsr_write(rt2x00dev, 27, 0x00);
	rt2800_rfcsr_write(rt2x00dev, 28, 0x61);
	rt2800_rfcsr_write(rt2x00dev, 29, 0xB5);
	rt2800_rfcsr_write(rt2x00dev, 43, 0x02);

	rt2800_rfcsr_write(rt2x00dev, 28, 0x62);
	rt2800_rfcsr_write(rt2x00dev, 29, 0xAD);
	rt2800_rfcsr_write(rt2x00dev, 39, 0x80);

	/* Initialize RF channel register to default value */
	rt2800_rfcsr_write_chanreg(rt2x00dev, 0, 0x03);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 1, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 2, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 3, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 4, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 5, 0x08);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 6, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 7, 0x51);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 8, 0x53);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 9, 0x16);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 10, 0x61);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 11, 0x53);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 12, 0x22);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 13, 0x3D);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 14, 0x06);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 15, 0x13);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 16, 0x22);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 17, 0x27);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 18, 0x02);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 19, 0xA7);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 20, 0x01);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 21, 0x52);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 22, 0x80);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 23, 0xB3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 24, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 25, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 26, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 27, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 28, 0x5C);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 29, 0x6B);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 30, 0x6B);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 31, 0x31);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 32, 0x5D);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 33, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 34, 0xE6);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 35, 0x55);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 36, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 37, 0xBB);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 38, 0xB3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 39, 0xB3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 40, 0x03);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 41, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 42, 0x00);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 43, 0xB3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 44, 0xD3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 45, 0xD5);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 46, 0x07);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 47, 0x68);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 48, 0xEF);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 49, 0x1C);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 54, 0x07);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 55, 0xA8);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 56, 0x85);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 57, 0x10);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 58, 0x07);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 59, 0x6A);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 60, 0x85);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 61, 0x10);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 62, 0x1C);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 63, 0x00);

	rt2800_rfcsr_write_bank(rt2x00dev, 6, 45, 0xC5);

	rt2800_rfcsr_write_chanreg(rt2x00dev, 9, 0x47);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 10, 0x71);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 11, 0x33);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 14, 0x0E);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 17, 0x23);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 19, 0xA4);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 20, 0x02);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 21, 0x12);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 28, 0x1C);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 29, 0xEB);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 32, 0x7D);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 34, 0xD6);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 36, 0x08);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 38, 0xB4);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 43, 0xD3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 44, 0xB3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 45, 0xD5);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 46, 0x27);
	rt2800_rfcsr_write_bank(rt2x00dev, 4, 47, 0x67);
	rt2800_rfcsr_write_bank(rt2x00dev, 6, 47, 0x69);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 48, 0xFF);
	rt2800_rfcsr_write_bank(rt2x00dev, 4, 54, 0x27);
	rt2800_rfcsr_write_bank(rt2x00dev, 6, 54, 0x20);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 55, 0x66);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 56, 0xFF);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 57, 0x1C);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 58, 0x20);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 59, 0x6B);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 60, 0xF7);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 61, 0x09);

	rt2800_rfcsr_write_chanreg(rt2x00dev, 10, 0x51);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 14, 0x06);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 19, 0xA7);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 28, 0x2C);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 55, 0x64);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 8, 0x51);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 9, 0x36);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 11, 0x53);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 14, 0x16);

	rt2800_rfcsr_write_chanreg(rt2x00dev, 47, 0x6C);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 48, 0xFC);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 49, 0x1F);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 54, 0x27);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 55, 0x66);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 59, 0x6B);

	/* Initialize RF channel register for DRQFN */
	rt2800_rfcsr_write_chanreg(rt2x00dev, 43, 0xD3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 44, 0xE3);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 45, 0xE5);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 47, 0x28);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 55, 0x68);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 56, 0xF7);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 58, 0x02);
	rt2800_rfcsr_write_chanreg(rt2x00dev, 60, 0xC7);

	/* Initialize RF DC calibration register to default value */
	rt2800_rfcsr_write_dccal(rt2x00dev, 0, 0x47);
	rt2800_rfcsr_write_dccal(rt2x00dev, 1, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 2, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 3, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 4, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 5, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 6, 0x10);
	rt2800_rfcsr_write_dccal(rt2x00dev, 7, 0x10);
	rt2800_rfcsr_write_dccal(rt2x00dev, 8, 0x04);
	rt2800_rfcsr_write_dccal(rt2x00dev, 9, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 10, 0x07);
	rt2800_rfcsr_write_dccal(rt2x00dev, 11, 0x01);
	rt2800_rfcsr_write_dccal(rt2x00dev, 12, 0x07);
	rt2800_rfcsr_write_dccal(rt2x00dev, 13, 0x07);
	rt2800_rfcsr_write_dccal(rt2x00dev, 14, 0x07);
	rt2800_rfcsr_write_dccal(rt2x00dev, 15, 0x20);
	rt2800_rfcsr_write_dccal(rt2x00dev, 16, 0x22);
	rt2800_rfcsr_write_dccal(rt2x00dev, 17, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 18, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 19, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 20, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 21, 0xF1);
	rt2800_rfcsr_write_dccal(rt2x00dev, 22, 0x11);
	rt2800_rfcsr_write_dccal(rt2x00dev, 23, 0x02);
	rt2800_rfcsr_write_dccal(rt2x00dev, 24, 0x41);
	rt2800_rfcsr_write_dccal(rt2x00dev, 25, 0x20);
	rt2800_rfcsr_write_dccal(rt2x00dev, 26, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 27, 0xD7);
	rt2800_rfcsr_write_dccal(rt2x00dev, 28, 0xA2);
	rt2800_rfcsr_write_dccal(rt2x00dev, 29, 0x20);
	rt2800_rfcsr_write_dccal(rt2x00dev, 30, 0x49);
	rt2800_rfcsr_write_dccal(rt2x00dev, 31, 0x20);
	rt2800_rfcsr_write_dccal(rt2x00dev, 32, 0x04);
	rt2800_rfcsr_write_dccal(rt2x00dev, 33, 0xF1);
	rt2800_rfcsr_write_dccal(rt2x00dev, 34, 0xA1);
	rt2800_rfcsr_write_dccal(rt2x00dev, 35, 0x01);
	rt2800_rfcsr_write_dccal(rt2x00dev, 41, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 42, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 43, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 44, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 45, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 46, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 47, 0x3E);
	rt2800_rfcsr_write_dccal(rt2x00dev, 48, 0x3D);
	rt2800_rfcsr_write_dccal(rt2x00dev, 49, 0x3E);
	rt2800_rfcsr_write_dccal(rt2x00dev, 50, 0x3D);
	rt2800_rfcsr_write_dccal(rt2x00dev, 51, 0x3E);
	rt2800_rfcsr_write_dccal(rt2x00dev, 52, 0x3D);
	rt2800_rfcsr_write_dccal(rt2x00dev, 53, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 54, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 55, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 56, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 57, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 58, 0x10);
	rt2800_rfcsr_write_dccal(rt2x00dev, 59, 0x10);
	rt2800_rfcsr_write_dccal(rt2x00dev, 60, 0x0A);
	rt2800_rfcsr_write_dccal(rt2x00dev, 61, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 62, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 63, 0x00);

	rt2800_rfcsr_write_dccal(rt2x00dev, 3, 0x08);
	rt2800_rfcsr_write_dccal(rt2x00dev, 4, 0x04);
	rt2800_rfcsr_write_dccal(rt2x00dev, 5, 0x20);

	rt2800_rfcsr_write_dccal(rt2x00dev, 5, 0x00);
	rt2800_rfcsr_write_dccal(rt2x00dev, 17, 0x7C);

	rt2800_bw_filter_calibration(rt2x00dev, true);
	rt2800_bw_filter_calibration(rt2x00dev, false);
}

static void rt2800_init_rfcsr(struct rt2x00_dev *rt2x00dev)
{
	if (rt2800_is_305x_soc(rt2x00dev)) {
		rt2800_init_rfcsr_305x_soc(rt2x00dev);
		return;
	}

	switch (rt2x00dev->chip.rt) {
	case RT3070:
	case RT3071:
	case RT3090:
		rt2800_init_rfcsr_30xx(rt2x00dev);
		break;
	case RT3290:
		rt2800_init_rfcsr_3290(rt2x00dev);
		break;
	case RT3352:
		rt2800_init_rfcsr_3352(rt2x00dev);
		break;
	case RT3390:
		rt2800_init_rfcsr_3390(rt2x00dev);
		break;
	case RT3883:
		rt2800_init_rfcsr_3883(rt2x00dev);
		break;
	case RT3572:
		rt2800_init_rfcsr_3572(rt2x00dev);
		break;
	case RT3593:
		rt2800_init_rfcsr_3593(rt2x00dev);
		break;
	case RT5350:
		rt2800_init_rfcsr_5350(rt2x00dev);
		break;
	case RT5390:
		rt2800_init_rfcsr_5390(rt2x00dev);
		break;
	case RT5392:
		rt2800_init_rfcsr_5392(rt2x00dev);
		break;
	case RT5592:
		rt2800_init_rfcsr_5592(rt2x00dev);
		break;
	case RT6352:
		rt2800_init_rfcsr_6352(rt2x00dev);
		break;
	}
}

int rt2800_enable_radio(struct rt2x00_dev *rt2x00dev) {
    uint32_t reg;
    uint16_t word;

    /*
     * Initialize MAC registers.
     */
    if (unlikely(rt2800_wait_wpdma_ready(rt2x00dev) ||
                rt2800_init_registers(rt2x00dev)))
        return -EIO;

    /*
     * Wait BBP/RF to wake up.
     */
    if (unlikely(rt2800_wait_bbp_rf_ready(rt2x00dev)))
        return -EIO;

    /*
     * Send signal during boot time to initialize firmware.
     */
    rt2800_register_write(rt2x00dev, H2M_BBP_AGENT, 0);
    rt2800_register_write(rt2x00dev, H2M_MAILBOX_CSR, 0);
    if (rt2x00_is_usb(rt2x00dev))
        rt2800_register_write(rt2x00dev, H2M_INT_SRC, 0);
    rt2800_mcu_request(rt2x00dev, MCU_BOOT_SIGNAL, 0, 0, 0);
    msleep(1);

    /*
     * Make sure BBP is up and running.
     */
    if (unlikely(rt2800_wait_bbp_ready(rt2x00dev)))
        return -EIO;

    /*
     * Initialize BBP/RF registers.
     */
    rt2800_init_bbp(rt2x00dev);
    rt2800_init_rfcsr(rt2x00dev);

    if (rt2x00_is_usb(rt2x00dev) &&
            (rt2x00_rt(rt2x00dev, RT3070) ||
             rt2x00_rt(rt2x00dev, RT3071) ||
             rt2x00_rt(rt2x00dev, RT3572))) {
        usleep(200);
        rt2800_mcu_request(rt2x00dev, MCU_CURRENT, 0, 0, 0);
        usleep(10);
    }

    /*
     * Enable RX.
     */
    reg = rt2800_register_read(rt2x00dev, MAC_SYS_CTRL);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_TX, 1);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_RX, 0);
    rt2800_register_write(rt2x00dev, MAC_SYS_CTRL, reg);

    usleep(50);

    reg = rt2800_register_read(rt2x00dev, WPDMA_GLO_CFG);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_TX_DMA, 1);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_RX_DMA, 1);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_WRITEBACK_DONE, 1);
    rt2800_register_write(rt2x00dev, WPDMA_GLO_CFG, reg);

    reg = rt2800_register_read(rt2x00dev, MAC_SYS_CTRL);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_TX, 1);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_RX, 1);
    rt2800_register_write(rt2x00dev, MAC_SYS_CTRL, reg);

    /*
     * Initialize LED control
     */
    word = rt2800_eeprom_read(rt2x00dev, EEPROM_LED_AG_CONF);
    rt2800_mcu_request(rt2x00dev, MCU_LED_AG_CONF, 0xff,
            word & 0xff, (word >> 8) & 0xff);

    word = rt2800_eeprom_read(rt2x00dev, EEPROM_LED_ACT_CONF);
    rt2800_mcu_request(rt2x00dev, MCU_LED_ACT_CONF, 0xff,
            word & 0xff, (word >> 8) & 0xff);

    word = rt2800_eeprom_read(rt2x00dev, EEPROM_LED_POLARITY);
    rt2800_mcu_request(rt2x00dev, MCU_LED_LED_POLARITY, 0xff,
            word & 0xff, (word >> 8) & 0xff);

    return 0;
}

void rt2800_disable_radio(struct rt2x00_dev *rt2x00dev) {
    uint32_t reg;

    rt2800_disable_wpdma(rt2x00dev);

    /* Wait for DMA, ignore error */
    rt2800_wait_wpdma_ready(rt2x00dev);

    reg = rt2800_register_read(rt2x00dev, MAC_SYS_CTRL);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_TX, 0);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_RX, 0);
    rt2800_register_write(rt2x00dev, MAC_SYS_CTRL, reg);
}

int rt2800_validate_eeprom(struct rt2x00_dev *rt2x00dev)
{
	uint16_t word;
	uint8_t default_lna_gain;
	int retval;

	/*
	 * Read the EEPROM.
	 */
	retval = rt2800_read_eeprom(rt2x00dev);
	if (retval)
		return retval;

	/*
	 * Start validation of the data that has been read.
	 */
	rt2x00dev->mac = rt2800_eeprom_addr(rt2x00dev, EEPROM_MAC_ADDR_0);

	word = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF0);
	if (word == 0xffff) {
		rt2x00_set_field16(&word, EEPROM_NIC_CONF0_RXPATH, 2);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF0_TXPATH, 1);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF0_RF_TYPE, RF2820);
		rt2800_eeprom_write(rt2x00dev, EEPROM_NIC_CONF0, word);
		rt2x00_eeprom_dbg(rt2x00dev, "Antenna: 0x%04x\n", word);
	} else if (rt2x00_rt(rt2x00dev, RT2860) ||
		   rt2x00_rt(rt2x00dev, RT2872)) {
		/*
		 * There is a max of 2 RX streams for RT28x0 series
		 */
		if (rt2x00_get_field16(word, EEPROM_NIC_CONF0_RXPATH) > 2)
			rt2x00_set_field16(&word, EEPROM_NIC_CONF0_RXPATH, 2);
		rt2800_eeprom_write(rt2x00dev, EEPROM_NIC_CONF0, word);
	}

	word = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);
	if (word == 0xffff) {
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_HW_RADIO, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_EXTERNAL_TX_ALC, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_EXTERNAL_LNA_2G, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_EXTERNAL_LNA_5G, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_CARDBUS_ACCEL, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_BW40M_SB_2G, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_BW40M_SB_5G, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_WPS_PBC, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_BW40M_2G, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_BW40M_5G, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_BROADBAND_EXT_LNA, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_ANT_DIVERSITY, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_INTERNAL_TX_ALC, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_BT_COEXIST, 0);
		rt2x00_set_field16(&word, EEPROM_NIC_CONF1_DAC_TEST, 0);
		rt2800_eeprom_write(rt2x00dev, EEPROM_NIC_CONF1, word);
		rt2x00_eeprom_dbg(rt2x00dev, "NIC: 0x%04x\n", word);
	}

	word = rt2800_eeprom_read(rt2x00dev, EEPROM_FREQ);
	if ((word & 0x00ff) == 0x00ff) {
		rt2x00_set_field16(&word, EEPROM_FREQ_OFFSET, 0);
		rt2800_eeprom_write(rt2x00dev, EEPROM_FREQ, word);
		rt2x00_eeprom_dbg(rt2x00dev, "Freq: 0x%04x\n", word);
	}
	if ((word & 0xff00) == 0xff00) {
		rt2x00_set_field16(&word, EEPROM_FREQ_LED_MODE,
				   LED_MODE_TXRX_ACTIVITY);
		rt2x00_set_field16(&word, EEPROM_FREQ_LED_POLARITY, 0);
		rt2800_eeprom_write(rt2x00dev, EEPROM_FREQ, word);
		rt2800_eeprom_write(rt2x00dev, EEPROM_LED_AG_CONF, 0x5555);
		rt2800_eeprom_write(rt2x00dev, EEPROM_LED_ACT_CONF, 0x2221);
		rt2800_eeprom_write(rt2x00dev, EEPROM_LED_POLARITY, 0xa9f8);
		rt2x00_eeprom_dbg(rt2x00dev, "Led Mode: 0x%04x\n", word);
	}

	/*
	 * During the LNA validation we are going to use
	 * lna0 as correct value. Note that EEPROM_LNA
	 * is never validated.
	 */
	word = rt2800_eeprom_read(rt2x00dev, EEPROM_LNA);
	default_lna_gain = rt2x00_get_field16(word, EEPROM_LNA_A0);

	word = rt2800_eeprom_read(rt2x00dev, EEPROM_RSSI_BG);
	if (abs(rt2x00_get_field16(word, EEPROM_RSSI_BG_OFFSET0)) > 10)
		rt2x00_set_field16(&word, EEPROM_RSSI_BG_OFFSET0, 0);
	if (abs(rt2x00_get_field16(word, EEPROM_RSSI_BG_OFFSET1)) > 10)
		rt2x00_set_field16(&word, EEPROM_RSSI_BG_OFFSET1, 0);
	rt2800_eeprom_write(rt2x00dev, EEPROM_RSSI_BG, word);

	word = rt2800_eeprom_read(rt2x00dev, EEPROM_RSSI_BG2);
	if (abs(rt2x00_get_field16(word, EEPROM_RSSI_BG2_OFFSET2)) > 10)
		rt2x00_set_field16(&word, EEPROM_RSSI_BG2_OFFSET2, 0);
	if (!rt2x00_rt(rt2x00dev, RT3593) &&
	    !rt2x00_rt(rt2x00dev, RT3883)) {
		if (rt2x00_get_field16(word, EEPROM_RSSI_BG2_LNA_A1) == 0x00 ||
		    rt2x00_get_field16(word, EEPROM_RSSI_BG2_LNA_A1) == 0xff)
			rt2x00_set_field16(&word, EEPROM_RSSI_BG2_LNA_A1,
					   default_lna_gain);
	}
	rt2800_eeprom_write(rt2x00dev, EEPROM_RSSI_BG2, word);

	word = rt2800_eeprom_read(rt2x00dev, EEPROM_RSSI_A);
	if (abs(rt2x00_get_field16(word, EEPROM_RSSI_A_OFFSET0)) > 10)
		rt2x00_set_field16(&word, EEPROM_RSSI_A_OFFSET0, 0);
	if (abs(rt2x00_get_field16(word, EEPROM_RSSI_A_OFFSET1)) > 10)
		rt2x00_set_field16(&word, EEPROM_RSSI_A_OFFSET1, 0);
	rt2800_eeprom_write(rt2x00dev, EEPROM_RSSI_A, word);

	word = rt2800_eeprom_read(rt2x00dev, EEPROM_RSSI_A2);
	if (abs(rt2x00_get_field16(word, EEPROM_RSSI_A2_OFFSET2)) > 10)
		rt2x00_set_field16(&word, EEPROM_RSSI_A2_OFFSET2, 0);
	if (!rt2x00_rt(rt2x00dev, RT3593) &&
	    !rt2x00_rt(rt2x00dev, RT3883)) {
		if (rt2x00_get_field16(word, EEPROM_RSSI_A2_LNA_A2) == 0x00 ||
		    rt2x00_get_field16(word, EEPROM_RSSI_A2_LNA_A2) == 0xff)
			rt2x00_set_field16(&word, EEPROM_RSSI_A2_LNA_A2,
					   default_lna_gain);
	}
	rt2800_eeprom_write(rt2x00dev, EEPROM_RSSI_A2, word);

	if (rt2x00_rt(rt2x00dev, RT3593) ||
	    rt2x00_rt(rt2x00dev, RT3883)) {
		word = rt2800_eeprom_read(rt2x00dev, EEPROM_EXT_LNA2);
		if (rt2x00_get_field16(word, EEPROM_EXT_LNA2_A1) == 0x00 ||
		    rt2x00_get_field16(word, EEPROM_EXT_LNA2_A1) == 0xff)
			rt2x00_set_field16(&word, EEPROM_EXT_LNA2_A1,
					   default_lna_gain);
		if (rt2x00_get_field16(word, EEPROM_EXT_LNA2_A2) == 0x00 ||
		    rt2x00_get_field16(word, EEPROM_EXT_LNA2_A2) == 0xff)
			rt2x00_set_field16(&word, EEPROM_EXT_LNA2_A1,
					   default_lna_gain);
		rt2800_eeprom_write(rt2x00dev, EEPROM_EXT_LNA2, word);
	}

	return 0;
}

int rt2800_init_eeprom(struct rt2x00_dev *rt2x00dev)
{
	uint16_t value;
	uint16_t eeprom;
	uint16_t rf;

	/*
	 * Read EEPROM word for configuration.
	 */
	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF0);

	/*
	 * Identify RF chipset by EEPROM value
	 * RT28xx/RT30xx: defined in "EEPROM_NIC_CONF0_RF_TYPE" field
	 * RT53xx: defined in "EEPROM_CHIP_ID" field
	 */
	if (rt2x00_rt(rt2x00dev, RT3290) ||
	    rt2x00_rt(rt2x00dev, RT5390) ||
	    rt2x00_rt(rt2x00dev, RT5392) ||
	    rt2x00_rt(rt2x00dev, RT6352))
		rf = rt2800_eeprom_read(rt2x00dev, EEPROM_CHIP_ID);
	else if (rt2x00_rt(rt2x00dev, RT3352))
		rf = RF3322;
	else if (rt2x00_rt(rt2x00dev, RT3883))
		rf = RF3853;
	else if (rt2x00_rt(rt2x00dev, RT5350))
		rf = RF5350;
	else
		rf = rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_RF_TYPE);

	switch (rf) {
	case RF2820:
	case RF2850:
	case RF2720:
	case RF2750:
	case RF3020:
	case RF2020:
	case RF3021:
	case RF3022:
	case RF3052:
	case RF3053:
	case RF3070:
	case RF3290:
	case RF3320:
	case RF3322:
	case RF3853:
	case RF5350:
	case RF5360:
	case RF5362:
	case RF5370:
	case RF5372:
	case RF5390:
	case RF5392:
	case RF5592:
	case RF7620:
        rt2x00_info(rt2x00dev, "Found RF chipset 0x%04x\n", rf);
		break;
	default:
		rt2x00_err(rt2x00dev, "Invalid RF chipset 0x%04x detected\n",
			   rf);
		return -ENODEV;
	}

	rt2x00_set_rf(rt2x00dev, rf);

	/*
	 * Identify default antenna configuration.
	 */
	rt2x00dev->default_ant.tx_chain_num =
	    rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_TXPATH);
	rt2x00dev->default_ant.rx_chain_num =
	    rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_RXPATH);

	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);

	if (rt2x00_rt(rt2x00dev, RT3070) ||
	    rt2x00_rt(rt2x00dev, RT3090) ||
	    rt2x00_rt(rt2x00dev, RT3352) ||
	    rt2x00_rt(rt2x00dev, RT3390)) {
		value = rt2x00_get_field16(eeprom,
				EEPROM_NIC_CONF1_ANT_DIVERSITY);
		switch (value) {
		case 0:
		case 1:
		case 2:
			rt2x00dev->default_ant.tx = ANTENNA_A;
			rt2x00dev->default_ant.rx = ANTENNA_A;
			break;
		case 3:
			rt2x00dev->default_ant.tx = ANTENNA_A;
			rt2x00dev->default_ant.rx = ANTENNA_B;
			break;
		}
	} else {
		rt2x00dev->default_ant.tx = ANTENNA_A;
		rt2x00dev->default_ant.rx = ANTENNA_A;
	}

	/* These chips have hardware RX antenna diversity */
	if (rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5390R) ||
	    rt2x00_rt_rev_gte(rt2x00dev, RT5390, REV_RT5370G)) {
		rt2x00dev->default_ant.tx = ANTENNA_HW_DIVERSITY; /* Unused */
		rt2x00dev->default_ant.rx = ANTENNA_HW_DIVERSITY; /* Unused */
	}

	/*
	 * Determine external LNA informations.
	 */
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_EXTERNAL_LNA_5G))
		__set_bit(CAPABILITY_EXTERNAL_LNA_A, &rt2x00dev->cap_flags);
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_EXTERNAL_LNA_2G))
		__set_bit(CAPABILITY_EXTERNAL_LNA_BG, &rt2x00dev->cap_flags);

	/*
	 * Detect if this device has an hardware controlled radio.
	 */
	if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_HW_RADIO))
		__set_bit(CAPABILITY_HW_BUTTON, &rt2x00dev->cap_flags);

	/*
	 * Detect if this device has Bluetooth co-existence.
	 */
	if (!rt2x00_rt(rt2x00dev, RT3352) &&
	    rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_BT_COEXIST))
		__set_bit(CAPABILITY_BT_COEXIST, &rt2x00dev->cap_flags);

	/*
	 * Read frequency offset and RF programming sequence.
	 */
	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_FREQ);
	rt2x00dev->freq_offset = rt2x00_get_field16(eeprom, EEPROM_FREQ_OFFSET);

	/*
	 * Store led settings, for correct led behaviour.
	 */
	rt2x00dev->led_mcu_reg = eeprom;

	/*
	 * Check if support EIRP tx power limit feature.
	 */
	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_EIRP_MAX_TX_POWER);

	if (rt2x00_get_field16(eeprom, EEPROM_EIRP_MAX_TX_POWER_2GHZ) <
					EIRP_MAX_TX_POWER_LIMIT)
		__set_bit(CAPABILITY_POWER_LIMIT, &rt2x00dev->cap_flags);

	/*
	 * Detect if device uses internal or external PA
	 */
	eeprom = rt2800_eeprom_read(rt2x00dev, EEPROM_NIC_CONF1);

	if (rt2x00_rt(rt2x00dev, RT3352)) {
		if (rt2x00_get_field16(eeprom,
		    EEPROM_NIC_CONF1_EXTERNAL_TX0_PA_3352))
		    __set_bit(CAPABILITY_EXTERNAL_PA_TX0,
			      &rt2x00dev->cap_flags);
		if (rt2x00_get_field16(eeprom,
		    EEPROM_NIC_CONF1_EXTERNAL_TX1_PA_3352))
		    __set_bit(CAPABILITY_EXTERNAL_PA_TX1,
			      &rt2x00dev->cap_flags);
	}

	return 0;
}

void rt2800_disable_wpdma(struct rt2x00_dev *rt2x00dev) {
    uint32_t reg;

    reg = rt2800_register_read(rt2x00dev, WPDMA_GLO_CFG);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_TX_DMA, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_DMA_BUSY, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_RX_DMA, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_RX_DMA_BUSY, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_WRITEBACK_DONE, 1);
    rt2800_register_write(rt2x00dev, WPDMA_GLO_CFG, reg);
}

static bool rt2800_check_firmware_crc(const uint8_t *data, const size_t len) {
    uint16_t fw_crc;
    uint16_t crc;

    /*
     * The last 2 bytes in the firmware array are the crc checksum itself,
     * this means that we should never pass those 2 bytes to the crc
     * algorithm.
     */
    fw_crc = (data[len - 2] << 8 | data[len - 1]);

    /*
     * Use the crc ccitt algorithm.
     * This will return the same value as the legacy driver which
     * used bit ordering reversion on the both the firmware bytes
     * before input input as well as on the final output.
     * Obviously using crc ccitt directly is much more efficient.
     */
    crc = crc_ccitt(~0, data, len - 2);

    /*
     * There is a small difference between the crc-itu-t + bitrev and
     * the crc-ccitt crc calculation. In the latter method the 2 bytes
     * will be swapped, use swab16 to convert the crc to the correct
     * value.
     */
    crc = swab16(crc);

    return fw_crc == crc;
}

int rt2800_check_firmware(struct rt2x00_dev *rt2x00dev,
        const uint8_t *data, const size_t len) {
    size_t offset = 0;
    size_t fw_len;
    bool multiple;

    /*
     * PCI(e) & SOC devices require firmware with a length
     * of 8kb. USB devices require firmware files with a length
     * of 4kb. Certain USB chipsets however require different firmware,
     * which Ralink only provides attached to the original firmware
     * file. Thus for USB devices, firmware files have a length
     * which is a multiple of 4kb. The firmware for rt3290 chip also
     * have a length which is a multiple of 4kb.
     */
    if (rt2x00_is_usb(rt2x00dev) || rt2x00_rt(rt2x00dev, RT3290))
        fw_len = 4096;
    else
        fw_len = 8192;

    multiple = true;
    /*
     * Validate the firmware length
     */
    if (len != fw_len && (!multiple || (len % fw_len) != 0))
        return FW_BAD_LENGTH;

    /*
     * Check if the chipset requires one of the upper parts
     * of the firmware.
     */
    if (rt2x00_is_usb(rt2x00dev) &&
            !rt2x00_rt(rt2x00dev, RT2860) &&
            !rt2x00_rt(rt2x00dev, RT2872) &&
            !rt2x00_rt(rt2x00dev, RT3070) &&
            ((len / fw_len) == 1))
        return FW_BAD_VERSION;

    /*
     * 8kb firmware files must be checked as if it were
     * 2 separate firmware files.
     */
    while (offset < len) {
        if (!rt2800_check_firmware_crc(data + offset, fw_len))
            return FW_BAD_CRC;

        offset += fw_len;
    }

    return FW_OK;
}

int rt2800_load_firmware(struct rt2x00_dev *rt2x00dev,
        const uint8_t *data, const size_t len) {
    unsigned int i;
    uint32_t reg;
    int retval;

    if (rt2x00_rt(rt2x00dev, RT3290)) {
        retval = rt2800_enable_wlan_rt3290(rt2x00dev);
        if (retval)
            return -EBUSY;
    }

    /*
     * If driver doesn't wake up firmware here,
     * rt2800_load_firmware will hang forever when interface is up again.
     */
    rt2800_register_write(rt2x00dev, AUTOWAKEUP_CFG, 0x00000000);

    /*
     * Wait for stable hardware.
     */
    if (rt2800_wait_csr_ready(rt2x00dev))
        return -EBUSY;

    rt2x00_info(rt2x00dev, "Got CSR ready, hw is stable for FW load\n");

    if (rt2x00_is_pci(rt2x00dev)) {
        if (rt2x00_rt(rt2x00dev, RT3290) ||
                rt2x00_rt(rt2x00dev, RT3572) ||
                rt2x00_rt(rt2x00dev, RT5390) ||
                rt2x00_rt(rt2x00dev, RT5392)) {
            reg = rt2800_register_read(rt2x00dev, AUX_CTRL);
            rt2x00_set_field32(&reg, AUX_CTRL_FORCE_PCIE_CLK, 1);
            rt2x00_set_field32(&reg, AUX_CTRL_WAKE_PCIE_EN, 1);
            rt2800_register_write(rt2x00dev, AUX_CTRL, reg);
        }
        rt2800_register_write(rt2x00dev, PWR_PIN_CFG, 0x00000002);
    }

    rt2800_disable_wpdma(rt2x00dev);

    /*
     * Write firmware to the device.
     */
    rt2800_drv_write_firmware(rt2x00dev, data, len);

    /*
     * Wait for device to stabilize.
     */
    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        reg = rt2800_register_read(rt2x00dev, PBF_SYS_CTRL);
        if (rt2x00_get_field32(reg, PBF_SYS_CTRL_READY))
            break;
        usleep(1);
    }

    if (i == REGISTER_BUSY_COUNT) {
        rt2x00_err(rt2x00dev, "PBF system register not ready\n");
        return -EBUSY;
    }

    /*
     * Disable DMA, will be reenabled later when enabling
     * the radio.
     */
    rt2800_disable_wpdma(rt2x00dev);

    /*
     * Initialize firmware.
     */
    rt2800_register_write(rt2x00dev, H2M_BBP_AGENT, 0);
    rt2800_register_write(rt2x00dev, H2M_MAILBOX_CSR, 0);
    if (rt2x00_is_usb(rt2x00dev)) {
        rt2800_register_write(rt2x00dev, H2M_INT_SRC, 0);
        rt2800_mcu_request(rt2x00dev, MCU_BOOT_SIGNAL, 0, 0, 0);
    }
    usleep(1);

    return 0;
}

void rt2800_config_filter(struct rt2x00_dev *rt2x00dev,
			  const unsigned int filter_flags)
{
	uint32_t reg;

    /* Modified to never drop packets not to me, etc */

	reg = rt2800_register_read(rt2x00dev, RX_FILTER_CFG);
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CRC_ERROR,
			   !(filter_flags & FIF_FCSFAIL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_PHY_ERROR,
			   !(filter_flags & FIF_PLCPFAIL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_NOT_TO_ME, 0);
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_NOT_MY_BSSD, 0);
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_VER_ERROR, 1);
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_MULTICAST,
			   !(filter_flags & FIF_ALLMULTI));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_BROADCAST, 0);
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_DUPLICATE, 0);
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CF_END_ACK,
			   !(filter_flags & FIF_CONTROL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CF_END,
			   !(filter_flags & FIF_CONTROL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_ACK,
			   !(filter_flags & FIF_CONTROL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CTS,
			   !(filter_flags & FIF_CONTROL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_RTS,
			   !(filter_flags & FIF_CONTROL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_PSPOLL,
			   !(filter_flags & FIF_PSPOLL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_BA, 0);
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_BAR,
			   !(filter_flags & FIF_CONTROL));
	rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CNTL,
			   !(filter_flags & FIF_CONTROL));
	rt2800_register_write(rt2x00dev, RX_FILTER_CFG, reg);
}

static void rt2800_config_ht_opmode(struct rt2x00_dev *rt2x00dev,
				    struct rt2x00lib_erp *erp)
{
	bool any_sta_nongf = !!(erp->ht_opmode &
				IEEE80211_HT_OP_MODE_NON_GF_STA_PRSNT);
	uint8_t protection = erp->ht_opmode & IEEE80211_HT_OP_MODE_PROTECTION;
	uint8_t mm20_mode, mm40_mode, gf20_mode, gf40_mode;
	uint16_t mm20_rate, mm40_rate, gf20_rate, gf40_rate;
	uint32_t reg;

	/* default protection rate for HT20: OFDM 24M */
	mm20_rate = gf20_rate = 0x4004;

	/* default protection rate for HT40: duplicate OFDM 24M */
	mm40_rate = gf40_rate = 0x4084;

	switch (protection) {
	case IEEE80211_HT_OP_MODE_PROTECTION_NONE:
		/*
		 * All STAs in this BSS are HT20/40 but there might be
		 * STAs not supporting greenfield mode.
		 * => Disable protection for HT transmissions.
		 */
		mm20_mode = mm40_mode = gf20_mode = gf40_mode = 0;

		break;
	case IEEE80211_HT_OP_MODE_PROTECTION_20MHZ:
		/*
		 * All STAs in this BSS are HT20 or HT20/40 but there
		 * might be STAs not supporting greenfield mode.
		 * => Protect all HT40 transmissions.
		 */
		mm20_mode = gf20_mode = 0;
		mm40_mode = gf40_mode = 1;

		break;
	case IEEE80211_HT_OP_MODE_PROTECTION_NONMEMBER:
		/*
		 * Nonmember protection:
		 * According to 802.11n we _should_ protect all
		 * HT transmissions (but we don't have to).
		 *
		 * But if cts_protection is enabled we _shall_ protect
		 * all HT transmissions using a CCK rate.
		 *
		 * And if any station is non GF we _shall_ protect
		 * GF transmissions.
		 *
		 * We decide to protect everything
		 * -> fall through to mixed mode.
		 */
	case IEEE80211_HT_OP_MODE_PROTECTION_NONHT_MIXED:
		/*
		 * Legacy STAs are present
		 * => Protect all HT transmissions.
		 */
		mm20_mode = mm40_mode = gf20_mode = gf40_mode = 1;

		/*
		 * If erp protection is needed we have to protect HT
		 * transmissions with CCK 11M long preamble.
		 */
		if (erp->cts_protection) {
			/* don't duplicate RTS/CTS in CCK mode */
			mm20_rate = mm40_rate = 0x0003;
			gf20_rate = gf40_rate = 0x0003;
		}
		break;
	}

	/* check for STAs not supporting greenfield mode */
	if (any_sta_nongf)
		gf20_mode = gf40_mode = 1;

	/* Update HT protection config */
	reg = rt2800_register_read(rt2x00dev, MM20_PROT_CFG);
	rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_RATE, mm20_rate);
	rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_CTRL, mm20_mode);
	rt2800_register_write(rt2x00dev, MM20_PROT_CFG, reg);

	reg = rt2800_register_read(rt2x00dev, MM40_PROT_CFG);
	rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_RATE, mm40_rate);
	rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_CTRL, mm40_mode);
	rt2800_register_write(rt2x00dev, MM40_PROT_CFG, reg);

	reg = rt2800_register_read(rt2x00dev, GF20_PROT_CFG);
	rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_RATE, gf20_rate);
	rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_CTRL, gf20_mode);
	rt2800_register_write(rt2x00dev, GF20_PROT_CFG, reg);

	reg = rt2800_register_read(rt2x00dev, GF40_PROT_CFG);
	rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_RATE, gf40_rate);
	rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_CTRL, gf40_mode);
	rt2800_register_write(rt2x00dev, GF40_PROT_CFG, reg);
}

void rt2800_config_intf(struct rt2x00_dev *rt2x00dev, struct rt2x00_intf *intf,
			struct rt2x00intf_conf *conf, const unsigned int flags)
{
	uint32_t reg;
	bool update_bssid = false;

	if (flags & CONFIG_UPDATE_TYPE) {
        rt2x00_info(rt2x00dev, "Updating interface type\n");
		/*
		 * Enable synchronisation.
		 */
		reg = rt2800_register_read(rt2x00dev, BCN_TIME_CFG);
		rt2x00_set_field32(&reg, BCN_TIME_CFG_TSF_SYNC, conf->sync);
		rt2800_register_write(rt2x00dev, BCN_TIME_CFG, reg);

		if (conf->sync == TSF_SYNC_AP_NONE) {
			/*
			 * Tune beacon queue transmit parameters for AP mode
			 */
			reg = rt2800_register_read(rt2x00dev, TBTT_SYNC_CFG);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_CWMIN, 0);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_AIFSN, 1);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_EXP_WIN, 32);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_TBTT_ADJUST, 0);
			rt2800_register_write(rt2x00dev, TBTT_SYNC_CFG, reg);
		} else {
			reg = rt2800_register_read(rt2x00dev, TBTT_SYNC_CFG);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_CWMIN, 4);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_AIFSN, 2);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_EXP_WIN, 32);
			rt2x00_set_field32(&reg, TBTT_SYNC_CFG_TBTT_ADJUST, 16);
			rt2800_register_write(rt2x00dev, TBTT_SYNC_CFG, reg);
		}
	}

	if (flags & CONFIG_UPDATE_MAC) {
		if (flags & CONFIG_UPDATE_TYPE &&
		    conf->sync == TSF_SYNC_AP_NONE) {
			/*
			 * The BSSID register has to be set to our own mac
			 * address in AP mode.
			 */
			memcpy(conf->bssid, conf->mac, sizeof(conf->mac));
			update_bssid = true;
		}

		if (!is_zero_ether_addr((const uint8_t *)conf->mac)) {
			reg = le32_to_cpu(conf->mac[1]);
			rt2x00_set_field32(&reg, MAC_ADDR_DW1_UNICAST_TO_ME_MASK, 0xff);
			conf->mac[1] = cpu_to_le32(reg);
		}

		rt2800_register_multiwrite(rt2x00dev, MAC_ADDR_DW0,
					      conf->mac, sizeof(conf->mac));
	}

	if ((flags & CONFIG_UPDATE_BSSID) || update_bssid) {
		if (!is_zero_ether_addr((const uint8_t *)conf->bssid)) {
			reg = le32_to_cpu(conf->bssid[1]);
			rt2x00_set_field32(&reg, MAC_BSSID_DW1_BSS_ID_MASK, 3);
			rt2x00_set_field32(&reg, MAC_BSSID_DW1_BSS_BCN_NUM, 0);
			conf->bssid[1] = cpu_to_le32(reg);
		}

		rt2800_register_multiwrite(rt2x00dev, MAC_BSSID_DW0,
					      conf->bssid, sizeof(conf->bssid));
	}
}

void rt2800_vco_calibration(struct rt2x00_dev *rt2x00dev)
{
	uint32_t	tx_pin;
	uint8_t	rfcsr;
	unsigned long min_sleep = 0;

	/*
	 * A voltage-controlled oscillator(VCO) is an electronic oscillator
	 * designed to be controlled in oscillation frequency by a voltage
	 * input. Maybe the temperature will affect the frequency of
	 * oscillation to be shifted. The VCO calibration will be called
	 * periodically to adjust the frequency to be precision.
	*/

	tx_pin = rt2800_register_read(rt2x00dev, TX_PIN_CFG);
	tx_pin &= TX_PIN_CFG_PA_PE_DISABLE;
	rt2800_register_write(rt2x00dev, TX_PIN_CFG, tx_pin);

	switch (rt2x00dev->chip.rf) {
	case RF2020:
	case RF3020:
	case RF3021:
	case RF3022:
	case RF3320:
	case RF3052:
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 7);
		rt2x00_set_field8(&rfcsr, RFCSR7_RF_TUNING, 1);
		rt2800_rfcsr_write(rt2x00dev, 7, rfcsr);
		break;
	case RF3053:
	case RF3070:
	case RF3290:
	case RF3853:
	case RF5350:
	case RF5360:
	case RF5362:
	case RF5370:
	case RF5372:
	case RF5390:
	case RF5392:
	case RF5592:
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 3);
		rt2x00_set_field8(&rfcsr, RFCSR3_VCOCAL_EN, 1);
		rt2800_rfcsr_write(rt2x00dev, 3, rfcsr);
		min_sleep = 1000;
		break;
	case RF7620:
		rt2800_rfcsr_write(rt2x00dev, 5, 0x40);
		rt2800_rfcsr_write(rt2x00dev, 4, 0x0C);
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 4);
		rt2x00_set_field8(&rfcsr, RFCSR4_VCOCAL_EN, 1);
		rt2800_rfcsr_write(rt2x00dev, 4, rfcsr);
		min_sleep = 2000;
		break;
	default:
		WARN_ONCE(1, "Not supported RF chipset %x for VCO recalibration",
			  rt2x00dev->chip.rf);
		return;
	}

	if (min_sleep > 0)
		usleep_range(min_sleep, min_sleep * 2);

	tx_pin = rt2800_register_read(rt2x00dev, TX_PIN_CFG);
	if (rt2x00dev->rf_channel <= 14) {
		switch (rt2x00dev->default_ant.tx_chain_num) {
		case 3:
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G2_EN, 1);
			/* fall through */
		case 2:
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G1_EN, 1);
			/* fall through */
		case 1:
		default:
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G0_EN, 1);
			break;
		}
	} else {
		switch (rt2x00dev->default_ant.tx_chain_num) {
		case 3:
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A2_EN, 1);
			/* fall through */
		case 2:
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A1_EN, 1);
			/* fall through */
		case 1:
		default:
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A0_EN, 1);
			break;
		}
	}
	rt2800_register_write(rt2x00dev, TX_PIN_CFG, tx_pin);

	if (rt2x00_rt(rt2x00dev, RT6352)) {
		if (rt2x00dev->default_ant.rx_chain_num == 1) {
			rt2800_bbp_write(rt2x00dev, 91, 0x07);
			rt2800_bbp_write(rt2x00dev, 95, 0x1A);
			rt2800_bbp_write(rt2x00dev, 195, 128);
			rt2800_bbp_write(rt2x00dev, 196, 0xA0);
			rt2800_bbp_write(rt2x00dev, 195, 170);
			rt2800_bbp_write(rt2x00dev, 196, 0x12);
			rt2800_bbp_write(rt2x00dev, 195, 171);
			rt2800_bbp_write(rt2x00dev, 196, 0x10);
		} else {
			rt2800_bbp_write(rt2x00dev, 91, 0x06);
			rt2800_bbp_write(rt2x00dev, 95, 0x9A);
			rt2800_bbp_write(rt2x00dev, 195, 128);
			rt2800_bbp_write(rt2x00dev, 196, 0xE0);
			rt2800_bbp_write(rt2x00dev, 195, 170);
			rt2800_bbp_write(rt2x00dev, 196, 0x30);
			rt2800_bbp_write(rt2x00dev, 195, 171);
			rt2800_bbp_write(rt2x00dev, 196, 0x30);
		}

		if (rt2x00_has_cap_external_lna_bg(rt2x00dev)) {
			rt2800_bbp_write(rt2x00dev, 75, 0x68);
			rt2800_bbp_write(rt2x00dev, 76, 0x4C);
			rt2800_bbp_write(rt2x00dev, 79, 0x1C);
			rt2800_bbp_write(rt2x00dev, 80, 0x0C);
			rt2800_bbp_write(rt2x00dev, 82, 0xB6);
		}

		/* On 11A, We should delay and wait RF/BBP to be stable
		 * and the appropriate time should be 1000 micro seconds
		 * 2005/06/05 - On 11G, we also need this delay time.
		 * Otherwise it's difficult to pass the WHQL.
		 */
		usleep_range(1000, 1500);
	}
}
EXPORT_SYMBOL_GPL(rt2800_vco_calibration);

void rt2800_config_erp(struct rt2x00_dev *rt2x00dev, struct rt2x00lib_erp *erp,
		       uint32_t changed)
{
	uint32_t reg;

	if (changed & BSS_CHANGED_ERP_PREAMBLE) {
		reg = rt2800_register_read(rt2x00dev, AUTO_RSP_CFG);
		rt2x00_set_field32(&reg, AUTO_RSP_CFG_AR_PREAMBLE,
				   !!erp->short_preamble);
		rt2800_register_write(rt2x00dev, AUTO_RSP_CFG, reg);
	}

	if (changed & BSS_CHANGED_ERP_CTS_PROT) {
		reg = rt2800_register_read(rt2x00dev, OFDM_PROT_CFG);
		rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_CTRL,
				   erp->cts_protection ? 2 : 0);
		rt2800_register_write(rt2x00dev, OFDM_PROT_CFG, reg);
	}

	if (changed & BSS_CHANGED_BASIC_RATES) {
		rt2800_register_write(rt2x00dev, LEGACY_BASIC_RATE,
				      0xff0 | erp->basic_rates);
		rt2800_register_write(rt2x00dev, HT_BASIC_RATE, 0x00008003);
	}

	if (changed & BSS_CHANGED_ERP_SLOT) {
		reg = rt2800_register_read(rt2x00dev, BKOFF_SLOT_CFG);
		rt2x00_set_field32(&reg, BKOFF_SLOT_CFG_SLOT_TIME,
				   erp->slot_time);
		rt2800_register_write(rt2x00dev, BKOFF_SLOT_CFG, reg);

		reg = rt2800_register_read(rt2x00dev, XIFS_TIME_CFG);
		rt2x00_set_field32(&reg, XIFS_TIME_CFG_EIFS, erp->eifs);
		rt2800_register_write(rt2x00dev, XIFS_TIME_CFG, reg);
	}

	if (changed & BSS_CHANGED_BEACON_INT) {
		reg = rt2800_register_read(rt2x00dev, BCN_TIME_CFG);
		rt2x00_set_field32(&reg, BCN_TIME_CFG_BEACON_INTERVAL,
				   erp->beacon_int * 16);
		rt2800_register_write(rt2x00dev, BCN_TIME_CFG, reg);
	}

	if (changed & BSS_CHANGED_HT)
		rt2800_config_ht_opmode(rt2x00dev, erp);
}
EXPORT_SYMBOL_GPL(rt2800_config_erp);

static void rt2800_iq_calibrate(struct rt2x00_dev *rt2x00dev, int channel)
{
	uint8_t cal;

	/* TX0 IQ Gain */
	rt2800_bbp_write(rt2x00dev, 158, 0x2c);
	if (channel <= 14)
		cal = rt2x00_eeprom_byte(rt2x00dev, EEPROM_IQ_GAIN_CAL_TX0_2G);
	else if (channel >= 36 && channel <= 64)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_GAIN_CAL_TX0_CH36_TO_CH64_5G);
	else if (channel >= 100 && channel <= 138)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_GAIN_CAL_TX0_CH100_TO_CH138_5G);
	else if (channel >= 140 && channel <= 165)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_GAIN_CAL_TX0_CH140_TO_CH165_5G);
	else
		cal = 0;
	rt2800_bbp_write(rt2x00dev, 159, cal);

	/* TX0 IQ Phase */
	rt2800_bbp_write(rt2x00dev, 158, 0x2d);
	if (channel <= 14)
		cal = rt2x00_eeprom_byte(rt2x00dev, EEPROM_IQ_PHASE_CAL_TX0_2G);
	else if (channel >= 36 && channel <= 64)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_PHASE_CAL_TX0_CH36_TO_CH64_5G);
	else if (channel >= 100 && channel <= 138)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_PHASE_CAL_TX0_CH100_TO_CH138_5G);
	else if (channel >= 140 && channel <= 165)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_PHASE_CAL_TX0_CH140_TO_CH165_5G);
	else
		cal = 0;
	rt2800_bbp_write(rt2x00dev, 159, cal);

	/* TX1 IQ Gain */
	rt2800_bbp_write(rt2x00dev, 158, 0x4a);
	if (channel <= 14)
		cal = rt2x00_eeprom_byte(rt2x00dev, EEPROM_IQ_GAIN_CAL_TX1_2G);
	else if (channel >= 36 && channel <= 64)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_GAIN_CAL_TX1_CH36_TO_CH64_5G);
	else if (channel >= 100 && channel <= 138)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_GAIN_CAL_TX1_CH100_TO_CH138_5G);
	else if (channel >= 140 && channel <= 165)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_GAIN_CAL_TX1_CH140_TO_CH165_5G);
	else
		cal = 0;
	rt2800_bbp_write(rt2x00dev, 159, cal);

	/* TX1 IQ Phase */
	rt2800_bbp_write(rt2x00dev, 158, 0x4b);
	if (channel <= 14)
		cal = rt2x00_eeprom_byte(rt2x00dev, EEPROM_IQ_PHASE_CAL_TX1_2G);
	else if (channel >= 36 && channel <= 64)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_PHASE_CAL_TX1_CH36_TO_CH64_5G);
	else if (channel >= 100 && channel <= 138)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_PHASE_CAL_TX1_CH100_TO_CH138_5G);
	else if (channel >= 140 && channel <= 165)
		cal = rt2x00_eeprom_byte(rt2x00dev,
					 EEPROM_IQ_PHASE_CAL_TX1_CH140_TO_CH165_5G);
	else
		cal = 0;
	rt2800_bbp_write(rt2x00dev, 159, cal);

	/* FIXME: possible RX0, RX1 callibration ? */

	/* RF IQ compensation control */
	rt2800_bbp_write(rt2x00dev, 158, 0x04);
	cal = rt2x00_eeprom_byte(rt2x00dev, EEPROM_RF_IQ_COMPENSATION_CONTROL);
	rt2800_bbp_write(rt2x00dev, 159, cal != 0xff ? cal : 0);

	/* RF IQ imbalance compensation control */
	rt2800_bbp_write(rt2x00dev, 158, 0x03);
	cal = rt2x00_eeprom_byte(rt2x00dev,
				 EEPROM_RF_IQ_IMBALANCE_COMPENSATION_CONTROL);
	rt2800_bbp_write(rt2x00dev, 159, cal != 0xff ? cal : 0);
}

static void rt3883_bbp_adjust(struct rt2x00_dev *rt2x00dev,
			      struct rf_channel *rf)
{
	uint8_t bbp;

	bbp = (rf->channel > 14) ? 0x48 : 0x38;
	rt2800_bbp_write_with_rx_chain(rt2x00dev, 66, bbp);

	rt2800_bbp_write(rt2x00dev, 69, 0x12);

	if (rf->channel <= 14) {
		rt2800_bbp_write(rt2x00dev, 70, 0x0a);
	} else {
		/* Disable CCK packet detection */
		rt2800_bbp_write(rt2x00dev, 70, 0x00);
	}

	rt2800_bbp_write(rt2x00dev, 73, 0x10);

	if (rf->channel > 14) {
		rt2800_bbp_write(rt2x00dev, 62, 0x1d);
		rt2800_bbp_write(rt2x00dev, 63, 0x1d);
		rt2800_bbp_write(rt2x00dev, 64, 0x1d);
	} else {
		rt2800_bbp_write(rt2x00dev, 62, 0x2d);
		rt2800_bbp_write(rt2x00dev, 63, 0x2d);
		rt2800_bbp_write(rt2x00dev, 64, 0x2d);
	}
}

static void rt2800_config_channel(struct rt2x00_dev *rt2x00dev,
				  struct ieee80211_conf *conf,
				  struct rf_channel *rf,
				  struct channel_info *info)
{
	uint32_t reg;
	uint32_t tx_pin;
	uint8_t bbp, rfcsr;

    rt2x00_info(rt2x00dev, "Configuring channel\n");

    /*
     * Remove txpower stuff since we don't tx 
     */

	switch (rt2x00dev->chip.rt) {
	case RT3883:
		rt3883_bbp_adjust(rt2x00dev, rf);
		break;
	}

	switch (rt2x00dev->chip.rf) {
	case RF2020:
	case RF3020:
	case RF3021:
	case RF3022:
	case RF3320:
		rt2800_config_channel_rf3xxx(rt2x00dev, conf, rf, info);
		break;
	case RF3052:
		rt2800_config_channel_rf3052(rt2x00dev, conf, rf, info);
		break;
	case RF3053:
		rt2800_config_channel_rf3053(rt2x00dev, conf, rf, info);
		break;
	case RF3290:
		rt2800_config_channel_rf3290(rt2x00dev, conf, rf, info);
		break;
	case RF3322:
		rt2800_config_channel_rf3322(rt2x00dev, conf, rf, info);
		break;
	case RF3853:
		rt2800_config_channel_rf3853(rt2x00dev, conf, rf, info);
		break;
	case RF3070:
	case RF5350:
	case RF5360:
	case RF5362:
	case RF5370:
	case RF5372:
	case RF5390:
	case RF5392:
		rt2800_config_channel_rf53xx(rt2x00dev, conf, rf, info);
		break;
	case RF5592:
		rt2800_config_channel_rf55xx(rt2x00dev, conf, rf, info);
		break;
	case RF7620:
		rt2800_config_channel_rf7620(rt2x00dev, conf, rf, info);
		break;
	default:
		rt2800_config_channel_rf2xxx(rt2x00dev, conf, rf, info);
	}

	if (rt2x00_rf(rt2x00dev, RF3070) ||
	    rt2x00_rf(rt2x00dev, RF3290) ||
	    rt2x00_rf(rt2x00dev, RF3322) ||
	    rt2x00_rf(rt2x00dev, RF5350) ||
	    rt2x00_rf(rt2x00dev, RF5360) ||
	    rt2x00_rf(rt2x00dev, RF5362) ||
	    rt2x00_rf(rt2x00dev, RF5370) ||
	    rt2x00_rf(rt2x00dev, RF5372) ||
	    rt2x00_rf(rt2x00dev, RF5390) ||
	    rt2x00_rf(rt2x00dev, RF5392)) {
		rfcsr = rt2800_rfcsr_read(rt2x00dev, 30);
		if (rt2x00_rf(rt2x00dev, RF3322)) {
			rt2x00_set_field8(&rfcsr, RF3322_RFCSR30_TX_H20M,
					  conf_is_ht40(conf));
			rt2x00_set_field8(&rfcsr, RF3322_RFCSR30_RX_H20M,
					  conf_is_ht40(conf));
		} else {
			rt2x00_set_field8(&rfcsr, RFCSR30_TX_H20M,
					  conf_is_ht40(conf));
			rt2x00_set_field8(&rfcsr, RFCSR30_RX_H20M,
					  conf_is_ht40(conf));
		}
		rt2800_rfcsr_write(rt2x00dev, 30, rfcsr);

		rfcsr = rt2800_rfcsr_read(rt2x00dev, 3);
		rt2x00_set_field8(&rfcsr, RFCSR3_VCOCAL_EN, 1);
		rt2800_rfcsr_write(rt2x00dev, 3, rfcsr);
	}

	/*
	 * Change BBP settings
	 */

	if (rt2x00_rt(rt2x00dev, RT3352)) {
		rt2800_bbp_write(rt2x00dev, 62, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 63, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 64, 0x37 - rt2x00dev->lna_gain);

		rt2800_bbp_write(rt2x00dev, 27, 0x0);
		rt2800_bbp_write(rt2x00dev, 66, 0x26 + rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 27, 0x20);
		rt2800_bbp_write(rt2x00dev, 66, 0x26 + rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 86, 0x38);
		rt2800_bbp_write(rt2x00dev, 83, 0x6a);
	} else if (rt2x00_rt(rt2x00dev, RT3593)) {
		if (rf->channel > 14) {
			/* Disable CCK Packet detection on 5GHz */
			rt2800_bbp_write(rt2x00dev, 70, 0x00);
		} else {
			rt2800_bbp_write(rt2x00dev, 70, 0x0a);
		}

		if (conf_is_ht40(conf))
			rt2800_bbp_write(rt2x00dev, 105, 0x04);
		else
			rt2800_bbp_write(rt2x00dev, 105, 0x34);

		rt2800_bbp_write(rt2x00dev, 62, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 63, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 64, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 77, 0x98);
	} else if (rt2x00_rt(rt2x00dev, RT3883)) {
		rt2800_bbp_write(rt2x00dev, 62, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 63, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 64, 0x37 - rt2x00dev->lna_gain);

		if (rt2x00dev->default_ant.rx_chain_num > 1)
			rt2800_bbp_write(rt2x00dev, 86, 0x46);
		else
			rt2800_bbp_write(rt2x00dev, 86, 0);
	} else {
		rt2800_bbp_write(rt2x00dev, 62, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 63, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 64, 0x37 - rt2x00dev->lna_gain);
		rt2800_bbp_write(rt2x00dev, 86, 0);
	}

	if (rf->channel <= 14) {
		if (!rt2x00_rt(rt2x00dev, RT5390) &&
		    !rt2x00_rt(rt2x00dev, RT5392) &&
		    !rt2x00_rt(rt2x00dev, RT6352)) {
			if (rt2x00_has_cap_external_lna_bg(rt2x00dev)) {
				rt2800_bbp_write(rt2x00dev, 82, 0x62);
				rt2800_bbp_write(rt2x00dev, 82, 0x62);
				rt2800_bbp_write(rt2x00dev, 75, 0x46);
			} else {
				if (rt2x00_rt(rt2x00dev, RT3593))
					rt2800_bbp_write(rt2x00dev, 82, 0x62);
				else
					rt2800_bbp_write(rt2x00dev, 82, 0x84);
				rt2800_bbp_write(rt2x00dev, 75, 0x50);
			}
			if (rt2x00_rt(rt2x00dev, RT3593) ||
			    rt2x00_rt(rt2x00dev, RT3883))
				rt2800_bbp_write(rt2x00dev, 83, 0x8a);
		}

	} else {
		if (rt2x00_rt(rt2x00dev, RT3572))
			rt2800_bbp_write(rt2x00dev, 82, 0x94);
		else if (rt2x00_rt(rt2x00dev, RT3593) ||
			 rt2x00_rt(rt2x00dev, RT3883))
			rt2800_bbp_write(rt2x00dev, 82, 0x82);
		else if (!rt2x00_rt(rt2x00dev, RT6352))
			rt2800_bbp_write(rt2x00dev, 82, 0xf2);

		if (rt2x00_rt(rt2x00dev, RT3593) ||
		    rt2x00_rt(rt2x00dev, RT3883))
			rt2800_bbp_write(rt2x00dev, 83, 0x9a);

		if (rt2x00_has_cap_external_lna_a(rt2x00dev))
			rt2800_bbp_write(rt2x00dev, 75, 0x46);
		else
			rt2800_bbp_write(rt2x00dev, 75, 0x50);
	}

	reg = rt2800_register_read(rt2x00dev, TX_BAND_CFG);
	rt2x00_set_field32(&reg, TX_BAND_CFG_HT40_MINUS, conf_is_ht40_minus(conf));
	rt2x00_set_field32(&reg, TX_BAND_CFG_A, rf->channel > 14);
	rt2x00_set_field32(&reg, TX_BAND_CFG_BG, rf->channel <= 14);
	rt2800_register_write(rt2x00dev, TX_BAND_CFG, reg);

	if (rt2x00_rt(rt2x00dev, RT3572))
		rt2800_rfcsr_write(rt2x00dev, 8, 0);

	if (rt2x00_rt(rt2x00dev, RT6352)) {
		tx_pin = rt2800_register_read(rt2x00dev, TX_PIN_CFG);
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_RFRX_EN, 1);
	} else {
		tx_pin = 0;
	}

	switch (rt2x00dev->default_ant.tx_chain_num) {
	case 3:
		/* Turn on tertiary PAs */
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A2_EN,
				   rf->channel > 14);
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G2_EN,
				   rf->channel <= 14);
		/* fall-through */
	case 2:
		/* Turn on secondary PAs */
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A1_EN,
				   rf->channel > 14);
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G1_EN,
				   rf->channel <= 14);
		/* fall-through */
	case 1:
		/* Turn on primary PAs */
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A0_EN,
				   rf->channel > 14);
		if (rt2x00_has_cap_bt_coexist(rt2x00dev))
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G0_EN, 1);
		else
			rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G0_EN,
					   rf->channel <= 14);
		break;
	}

	switch (rt2x00dev->default_ant.rx_chain_num) {
	case 3:
		/* Turn on tertiary LNAs */
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_A2_EN,
				   rf->channel > 14);
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_G2_EN,
				   rf->channel <= 14);
		/* fall-through */
	case 2:
		/* Turn on secondary LNAs */
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_A1_EN,
				   rf->channel > 14);
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_G1_EN,
				   rf->channel <= 14);
		/* fall-through */
	case 1:
		/* Turn on primary LNAs */
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_A0_EN,
				   rf->channel > 14);
		rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_G0_EN,
				   rf->channel <= 14);
		break;
	}

	rt2x00_set_field32(&tx_pin, TX_PIN_CFG_RFTR_EN, 1);
	rt2x00_set_field32(&tx_pin, TX_PIN_CFG_TRSW_EN, 1);

	rt2800_register_write(rt2x00dev, TX_PIN_CFG, tx_pin);

	if (rt2x00_rt(rt2x00dev, RT3572)) {
		rt2800_rfcsr_write(rt2x00dev, 8, 0x80);

		/* AGC init */
		if (rf->channel <= 14)
			reg = 0x1c + (2 * rt2x00dev->lna_gain);
		else
			reg = 0x22 + ((rt2x00dev->lna_gain * 5) / 3);

		rt2800_bbp_write_with_rx_chain(rt2x00dev, 66, reg);
	}

	if (rt2x00_rt(rt2x00dev, RT3593)) {
		reg = rt2800_register_read(rt2x00dev, GPIO_CTRL);

		/* Band selection */
		if (rt2x00_is_usb(rt2x00dev) ||
		    rt2x00_is_pcie(rt2x00dev)) {
			/* GPIO #8 controls all paths */
			rt2x00_set_field32(&reg, GPIO_CTRL_DIR8, 0);
			if (rf->channel <= 14)
				rt2x00_set_field32(&reg, GPIO_CTRL_VAL8, 1);
			else
				rt2x00_set_field32(&reg, GPIO_CTRL_VAL8, 0);
		}

		/* LNA PE control. */
		if (rt2x00_is_usb(rt2x00dev)) {
			/* GPIO #4 controls PE0 and PE1,
			 * GPIO #7 controls PE2
			 */
			rt2x00_set_field32(&reg, GPIO_CTRL_DIR4, 0);
			rt2x00_set_field32(&reg, GPIO_CTRL_DIR7, 0);

			rt2x00_set_field32(&reg, GPIO_CTRL_VAL4, 1);
			rt2x00_set_field32(&reg, GPIO_CTRL_VAL7, 1);
		} else if (rt2x00_is_pcie(rt2x00dev)) {
			/* GPIO #4 controls PE0, PE1 and PE2 */
			rt2x00_set_field32(&reg, GPIO_CTRL_DIR4, 0);
			rt2x00_set_field32(&reg, GPIO_CTRL_VAL4, 1);
		}

		rt2800_register_write(rt2x00dev, GPIO_CTRL, reg);

		/* AGC init */
		if (rf->channel <= 14)
			reg = 0x1c + 2 * rt2x00dev->lna_gain;
		else
			reg = 0x22 + ((rt2x00dev->lna_gain * 5) / 3);

		rt2800_bbp_write_with_rx_chain(rt2x00dev, 66, reg);

		usleep_range(1000, 1500);
	}

	if (rt2x00_rt(rt2x00dev, RT3883)) {
		if (!conf_is_ht40(conf))
			rt2800_bbp_write(rt2x00dev, 105, 0x34);
		else
			rt2800_bbp_write(rt2x00dev, 105, 0x04);

		/* AGC init */
		if (rf->channel <= 14)
			reg = 0x2e + rt2x00dev->lna_gain;
		else
			reg = 0x20 + ((rt2x00dev->lna_gain * 5) / 3);

		rt2800_bbp_write_with_rx_chain(rt2x00dev, 66, reg);

		usleep_range(1000, 1500);
	}

	if (rt2x00_rt(rt2x00dev, RT5592) || rt2x00_rt(rt2x00dev, RT6352)) {
		reg = 0x10;
		if (!conf_is_ht40(conf)) {
			if (rt2x00_rt(rt2x00dev, RT6352) &&
			    rt2x00_has_cap_external_lna_bg(rt2x00dev)) {
				reg |= 0x5;
			} else {
				reg |= 0xa;
			}
		}
		rt2800_bbp_write(rt2x00dev, 195, 141);
		rt2800_bbp_write(rt2x00dev, 196, reg);

		/* AGC init.
		 * Despite the vendor driver using different values here for
		 * RT6352 chip, we use 0x1c for now. This may have to be changed
		 * once TSSI got implemented.
		 */
		reg = (rf->channel <= 14 ? 0x1c : 0x24) + 2*rt2x00dev->lna_gain;
		rt2800_bbp_write_with_rx_chain(rt2x00dev, 66, reg);

		rt2800_iq_calibrate(rt2x00dev, rf->channel);
	}

	bbp = rt2800_bbp_read(rt2x00dev, 4);
	rt2x00_set_field8(&bbp, BBP4_BANDWIDTH, 2 * conf_is_ht40(conf));
	rt2800_bbp_write(rt2x00dev, 4, bbp);

	bbp = rt2800_bbp_read(rt2x00dev, 3);
	rt2x00_set_field8(&bbp, BBP3_HT40_MINUS, conf_is_ht40_minus(conf));
	rt2800_bbp_write(rt2x00dev, 3, bbp);

	if (rt2x00_rt_rev(rt2x00dev, RT2860, REV_RT2860C)) {
		if (conf_is_ht40(conf)) {
			rt2800_bbp_write(rt2x00dev, 69, 0x1a);
			rt2800_bbp_write(rt2x00dev, 70, 0x0a);
			rt2800_bbp_write(rt2x00dev, 73, 0x16);
		} else {
			rt2800_bbp_write(rt2x00dev, 69, 0x16);
			rt2800_bbp_write(rt2x00dev, 70, 0x08);
			rt2800_bbp_write(rt2x00dev, 73, 0x11);
		}
	}

	usleep_range(1000, 1500);

	/*
	 * Clear channel statistic counters
	 */
	reg = rt2800_register_read(rt2x00dev, CH_IDLE_STA);
	reg = rt2800_register_read(rt2x00dev, CH_BUSY_STA);
	reg = rt2800_register_read(rt2x00dev, CH_BUSY_STA_SEC);

	/*
	 * Clear update flag
	 */
	if (rt2x00_rt(rt2x00dev, RT3352) ||
	    rt2x00_rt(rt2x00dev, RT5350)) {
		bbp = rt2800_bbp_read(rt2x00dev, 49);
		rt2x00_set_field8(&bbp, BBP49_UPDATE_FLAG, 0);
		rt2800_bbp_write(rt2x00dev, 49, bbp);
	}
}

static void rt2800_config_ps(struct rt2x00_dev *rt2x00dev,
			     struct rt2x00lib_conf *libconf)
{
	enum dev_state state =
	    (libconf->conf->flags & IEEE80211_CONF_PS) ?
		STATE_SLEEP : STATE_AWAKE;
	uint32_t reg;

	if (state == STATE_SLEEP) {
		rt2800_register_write(rt2x00dev, AUTOWAKEUP_CFG, 0);

		reg = rt2800_register_read(rt2x00dev, AUTOWAKEUP_CFG);
		rt2x00_set_field32(&reg, AUTOWAKEUP_CFG_AUTO_LEAD_TIME, 5);
		rt2x00_set_field32(&reg, AUTOWAKEUP_CFG_TBCN_BEFORE_WAKE,
				   libconf->conf->listen_interval - 1);
		rt2x00_set_field32(&reg, AUTOWAKEUP_CFG_AUTOWAKE, 1);
		rt2800_register_write(rt2x00dev, AUTOWAKEUP_CFG, reg);

		rt2x00dev->ops->lib->set_device_state(rt2x00dev, state);
	} else {
		reg = rt2800_register_read(rt2x00dev, AUTOWAKEUP_CFG);
		rt2x00_set_field32(&reg, AUTOWAKEUP_CFG_AUTO_LEAD_TIME, 0);
		rt2x00_set_field32(&reg, AUTOWAKEUP_CFG_TBCN_BEFORE_WAKE, 0);
		rt2x00_set_field32(&reg, AUTOWAKEUP_CFG_AUTOWAKE, 0);
		rt2800_register_write(rt2x00dev, AUTOWAKEUP_CFG, reg);

		rt2x00dev->ops->lib->set_device_state(rt2x00dev, state);
	}
}

void rt2800_config(struct rt2x00_dev *rt2x00dev,
		   struct rt2x00lib_conf *libconf,
		   const unsigned int flags)
{
	/* Always recalculate LNA gain before changing configuration */
	rt2800_config_lna_gain(rt2x00dev, libconf);

	if (flags & IEEE80211_CONF_CHANGE_CHANNEL) {
		rt2800_config_channel(rt2x00dev, libconf->conf,
				      &libconf->rf, &libconf->channel);
	}
	if (flags & IEEE80211_CONF_CHANGE_PS)
		rt2800_config_ps(rt2x00dev, libconf);
}
EXPORT_SYMBOL_GPL(rt2800_config);

/*
 * Radio control handlers.
 */
int rt2x00lib_enable_radio(struct rt2x00_dev *rt2x00dev)
{
	int status;

    /* 
     * Trimmed down for userspace
     */

	/*
	 * Don't enable the radio twice.
	 * And check if the hardware button has been disabled.
	 */
	if (test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return 0;

	/*
	 * Enable radio.
	 */
	status =
	    rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_ON);
	if (status)
		return status;

	rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_IRQ_ON);

	set_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags);

	return 0;
}

void rt2x00lib_disable_radio(struct rt2x00_dev *rt2x00dev)
{
	if (!test_and_clear_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

    /*
     * Trim down functionality for userspace
     */

	/*
	 * Disable radio.
	 */
	rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_OFF);
	rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_IRQ_OFF);
}

int rt2x00lib_start(struct rt2x00_dev *rt2x00dev)
{
	int retval;

    /*
     * Severely stripped down since we don't call initialize or the 2x00
     * firmware load since we have our own
     */

	if (test_bit(DEVICE_STATE_STARTED, &rt2x00dev->flags))
		return 0;

	rt2x00dev->intf_sta_count = 0;

	/* Enable the radio */
	retval = rt2x00lib_enable_radio(rt2x00dev);
	if (retval)
		return retval;

	set_bit(DEVICE_STATE_STARTED, &rt2x00dev->flags);

	return 0;
}

void rt2x00lib_stop(struct rt2x00_dev *rt2x00dev)
{
	if (!test_and_clear_bit(DEVICE_STATE_STARTED, &rt2x00dev->flags))
		return;

	/*
	 * Perhaps we can add something smarter here,
	 * but for now just disabling the radio should do.
	 */
	rt2x00lib_disable_radio(rt2x00dev);

	rt2x00dev->intf_sta_count = 0;
}

static void rt2x00link_antenna_reset(struct rt2x00_dev *rt2x00dev)
{
	ewma_rssi_init(&rt2x00dev->link.ant.rssi_ant);
}

void rt2x00link_start_tuner(struct rt2x00_dev *rt2x00dev)
{
    /* 
     * Userspace should never need this because we're only monitor
     */

    return;
}

void rt2x00link_reset_tuner(struct rt2x00_dev *rt2x00dev, bool antenna)
{
	struct link_qual *qual = &rt2x00dev->link.qual;
	uint8_t vgc_level = qual->vgc_level_reg;

	if (!test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	/*
	 * Reset link information.
	 * Both the currently active vgc level as well as
	 * the link tuner counter should be reset. Resetting
	 * the counter is important for devices where the
	 * device should only perform link tuning during the
	 * first minute after being enabled.
	 */
	rt2x00dev->link.count = 0;
	memset(qual, 0, sizeof(*qual));
	ewma_rssi_init(&rt2x00dev->link.avg_rssi);

	/*
	 * Restore the VGC level as stored in the registers,
	 * the driver can use this to determine if the register
	 * must be updated during reset or not.
	 */
	qual->vgc_level_reg = vgc_level;

	/*
	 * Reset the link tuner.
	 */
	rt2x00dev->ops->lib->reset_tuner(rt2x00dev, qual);

	if (antenna)
		rt2x00link_antenna_reset(rt2x00dev);
}

void rt2x00lib_config_antenna(struct rt2x00_dev *rt2x00dev,
			      struct antenna_setup config)
{
    /* 
     * Modified for userspace - ant is always the default ant
     */

	struct antenna_setup *def = &rt2x00dev->default_ant;

    config.rx = def->rx;
    config.tx = def->tx;

	/*
	 * Antenna setup changes require the RX to be disabled,
	 * else the changes will be ignored by the device.
	 */
	if (test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
        rt2x00dev->ops->lib->stop_queue(rt2x00dev);

	/*
	 * Write new antenna setup to device and reset the link tuner.
	 * The latter is required since we need to recalibrate the
	 * noise-sensitivity ratio for the new setup.
	 */
	rt2x00dev->ops->lib->config_ant(rt2x00dev, &config);

	rt2x00link_reset_tuner(rt2x00dev, true);

	if (test_bit(DEVICE_STATE_ENABLED_RADIO, &rt2x00dev->flags))
        rt2x00dev->ops->lib->start_queue(rt2x00dev);
}

static uint16_t rt2x00ht_center_channel(struct rt2x00_dev *rt2x00dev,
				   struct ieee80211_conf *conf)
{
	struct hw_mode_spec *spec = &rt2x00dev->spec;
	int center_channel;
	uint16_t i;

	/*
	 * Initialize center channel to current channel.
	 */
	center_channel = spec->channels[conf->chandef.chan->hw_value].channel;

	/*
	 * Adjust center channel to HT40+ and HT40- operation.
	 */
	if (conf_is_ht40_plus(conf))
		center_channel += 2;
	else if (conf_is_ht40_minus(conf))
		center_channel -= (center_channel == 14) ? 1 : 2;

	for (i = 0; i < spec->num_channels; i++)
		if (spec->channels[i].channel == center_channel)
			return i;

	WARN_ON(1);
	return conf->chandef.chan->hw_value;
}

void rt2x00lib_config(struct rt2x00_dev *rt2x00dev,
		      struct ieee80211_conf *conf,
		      unsigned int ieee80211_flags)
{
	struct rt2x00lib_conf libconf;
	uint16_t hw_value;

    rt2x00_info(rt2x00dev, "rt2x00lib_config flags %u\n", ieee80211_flags);

	memset(&libconf, 0, sizeof(libconf));

	libconf.conf = conf;

	if (ieee80211_flags & IEEE80211_CONF_CHANGE_CHANNEL) {
		if (!conf_is_ht(conf))
			set_bit(CONFIG_HT_DISABLED, &rt2x00dev->flags);
		else
			clear_bit(CONFIG_HT_DISABLED, &rt2x00dev->flags);

		if (conf_is_ht40(conf)) {
			set_bit(CONFIG_CHANNEL_HT40, &rt2x00dev->flags);
			hw_value = rt2x00ht_center_channel(rt2x00dev, conf);
		} else {
			clear_bit(CONFIG_CHANNEL_HT40, &rt2x00dev->flags);
			hw_value = conf->chandef.chan->hw_value;
		}

		memcpy(&libconf.rf,
		       &rt2x00dev->spec.channels[hw_value],
		       sizeof(libconf.rf));

		memcpy(&libconf.channel,
		       &rt2x00dev->spec.channels_info[hw_value],
		       sizeof(libconf.channel));

		/* Used for VCO periodic calibration */
		rt2x00dev->rf_channel = libconf.rf.channel;
	}

	/*
	 * Start configuration.
	 */
	rt2x00dev->ops->lib->config(rt2x00dev, &libconf, ieee80211_flags);

    clear_bit(CONFIG_POWERSAVING, &rt2x00dev->flags);
    clear_bit(CONFIG_MONITORING, &rt2x00dev->flags);

	rt2x00dev->curr_band = conf->chandef.chan->band;
	rt2x00dev->curr_freq = conf->chandef.chan->center_freq;

}

int rt2800_probe_rt(struct rt2x00_dev *rt2x00dev)
{
	uint32_t reg;
	uint32_t rt;
	uint32_t rev;

	if (rt2x00_rt(rt2x00dev, RT3290))
		reg = rt2800_register_read(rt2x00dev, MAC_CSR0_3290);
	else
		reg = rt2800_register_read(rt2x00dev, MAC_CSR0);

	rt = rt2x00_get_field32(reg, MAC_CSR0_CHIPSET);
	rev = rt2x00_get_field32(reg, MAC_CSR0_REVISION);

	switch (rt) {
	case RT2860:
	case RT2872:
	case RT2883:
	case RT3070:
	case RT3071:
	case RT3090:
	case RT3290:
	case RT3352:
	case RT3390:
	case RT3572:
	case RT3593:
	case RT3883:
	case RT5350:
	case RT5390:
	case RT5392:
	case RT5592:
		break;
	default:
		rt2x00_err(rt2x00dev, "Invalid RT chipset 0x%04x, rev %04x detected\n",
			   rt, rev);
		return -ENODEV;
	}

	if (rt == RT5390 && rt2x00_is_soc(rt2x00dev))
		rt = RT6352;

	rt2x00_set_rt(rt2x00dev, rt, rev);

	return 0;
}


