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

#ifndef __RT2800LIB_H__
#define __RT2800LIB_H__ 

#include "kernel/types.h"
#include "kernel/mac80211.h"
#include "kernel/ieee80211.h"

#include "rt2x00.h"
#include "rt2800.h"

/* RT2800 driver data structure */
struct rt2800_drv_data {
	uint8_t calibration_bw20;
	uint8_t calibration_bw40;
	char rx_calibration_bw20;
	char rx_calibration_bw40;
	char tx_calibration_bw20;
	char tx_calibration_bw40;
	uint8_t bbp25;
	uint8_t bbp26;
	uint8_t txmixer_gain_24g;
	uint8_t txmixer_gain_5g;
	uint8_t max_psdu;
	unsigned int tbtt_tick;
	unsigned int ampdu_factor_cnt[4];
};

struct rt2800_ops {
    uint32_t (*register_read)(struct rt2x00_dev *rt2x00dev,
            const unsigned int offset);
    uint32_t (*register_read_lock)(struct rt2x00_dev *rt2x00dev,
            const unsigned int offset);
    void (*register_write)(struct rt2x00_dev *rt2x00dev,
            const unsigned int offset, uint32_t value);
    void (*register_write_lock)(struct rt2x00_dev *rt2x00dev,
            const unsigned int offset, uint32_t value);

    void (*register_multiread)(struct rt2x00_dev *rt2x00dev,
            const unsigned int offset,
            void *value, const uint32_t length);
    void (*register_multiwrite)(struct rt2x00_dev *rt2x00dev,
            const unsigned int offset,
            const void *value, const uint32_t length);

    int (*regbusy_read)(struct rt2x00_dev *rt2x00dev,
            const unsigned int offset,
            const struct rt2x00_field32 field, uint32_t *reg);

    int (*read_eeprom)(struct rt2x00_dev *rt2x00dev);
    bool (*hwcrypt_disabled)(struct rt2x00_dev *rt2x00dev);

    int (*drv_write_firmware)(struct rt2x00_dev *rt2x00dev,
            const uint8_t *data, const size_t len);
    int (*drv_init_registers)(struct rt2x00_dev *rt2x00dev);
};

static inline uint32_t rt2800_register_read(struct rt2x00_dev *rt2x00dev,
        const unsigned int offset) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    return rt2800ops->register_read(rt2x00dev, offset);
}

static inline uint32_t rt2800_register_read_lock(struct rt2x00_dev *rt2x00dev,
        const unsigned int offset) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    return rt2800ops->register_read_lock(rt2x00dev, offset);
}

static inline void rt2800_register_write(struct rt2x00_dev *rt2x00dev,
        const unsigned int offset,
        uint32_t value) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    rt2800ops->register_write(rt2x00dev, offset, value);
}

static inline void rt2800_register_write_lock(struct rt2x00_dev *rt2x00dev,
        const unsigned int offset,
        uint32_t value) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    rt2800ops->register_write_lock(rt2x00dev, offset, value);
}

static inline void rt2800_register_multiread(struct rt2x00_dev *rt2x00dev,
        const unsigned int offset,
        void *value, const uint32_t length) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    rt2800ops->register_multiread(rt2x00dev, offset, value, length);
}

static inline void rt2800_register_multiwrite(struct rt2x00_dev *rt2x00dev,
        const unsigned int offset,
        const void *value,
        const uint32_t length) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    rt2800ops->register_multiwrite(rt2x00dev, offset, value, length);
}

static inline int rt2800_regbusy_read(struct rt2x00_dev *rt2x00dev,
        const unsigned int offset,
        const struct rt2x00_field32 field,
        uint32_t *reg) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    return rt2800ops->regbusy_read(rt2x00dev, offset, field, reg);
}

static inline int rt2800_read_eeprom(struct rt2x00_dev *rt2x00dev) {
	const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

	return rt2800ops->read_eeprom(rt2x00dev);
}

static inline bool rt2800_hwcrypt_disabled(struct rt2x00_dev *rt2x00dev) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    return rt2800ops->hwcrypt_disabled(rt2x00dev);
}

static inline int rt2800_drv_write_firmware(struct rt2x00_dev *rt2x00dev,
        const uint8_t *data, const size_t len) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    return rt2800ops->drv_write_firmware(rt2x00dev, data, len);
}

static inline int rt2800_drv_init_registers(struct rt2x00_dev *rt2x00dev) {
    const struct rt2800_ops *rt2800ops = (const struct rt2800_ops *) rt2x00dev->ops->drv;

    return rt2800ops->drv_init_registers(rt2x00dev);
}

int rt2800_efuse_detect(struct rt2x00_dev *rt2x00dev);
int rt2800_read_eeprom_efuse(struct rt2x00_dev *rt2x00dev);

int rt2800_rfkill_poll(struct rt2x00_dev *rt2x00dev);
int rt2800_config_shared_key(struct rt2x00_dev *rt2x00dev,
        struct rt2x00lib_crypto *crypto,
        struct ieee80211_key_conf *key);
int rt2800_config_pairwise_key(struct rt2x00_dev *rt2x00dev,
        struct rt2x00lib_crypto *crypto,
        struct ieee80211_key_conf *key);
int rt2800_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
        struct ieee80211_sta *sta);
int rt2800_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
        struct ieee80211_sta *sta);
void rt2800_config_filter(struct rt2x00_dev *rt2x00dev,
        const unsigned int filter_flags);
void rt2800_config_ant(struct rt2x00_dev *rt2x00dev, struct antenna_setup *ant);
void rt2800_config(struct rt2x00_dev *rt2x00dev,
        struct rt2x00lib_conf *libconf,
        const unsigned int flags);
void rt2800_config_intf(struct rt2x00_dev *rt2x00dev, struct rt2x00_intf *intf,
			struct rt2x00intf_conf *conf, const unsigned int flags);
void rt2800_config_erp(struct rt2x00_dev *rt2x00dev, struct rt2x00lib_erp *erp,
		       uint32_t changed);
void rt2800_link_stats(struct rt2x00_dev *rt2x00dev, struct link_qual *qual);
void rt2800_reset_tuner(struct rt2x00_dev *rt2x00dev, struct link_qual *qual);
void rt2800_link_tuner(struct rt2x00_dev *rt2x00dev, struct link_qual *qual,
        const uint32_t count);
void rt2800_gain_calibration(struct rt2x00_dev *rt2x00dev);
void rt2800_vco_calibration(struct rt2x00_dev *rt2x00dev);

int rt2800_enable_radio(struct rt2x00_dev *rt2x00dev);
void rt2800_disable_radio(struct rt2x00_dev *rt2x00dev);

void rt2800_disable_wpdma(struct rt2x00_dev *rt2x00dev);

void rt2800_mcu_request(struct rt2x00_dev *rt2x00dev,
        const uint8_t command, const uint8_t token,
        const uint8_t arg0, const uint8_t arg1);

int rt2800_wait_csr_ready(struct rt2x00_dev *rt2x00dev);
int rt2800_wait_wpdma_ready(struct rt2x00_dev *rt2x00dev);

int rt2800_check_firmware(struct rt2x00_dev *rt2x00dev,
        const uint8_t *data, const size_t len);
int rt2800_load_firmware(struct rt2x00_dev *rt2x00dev,
        const uint8_t *data, const size_t len);

int rt2800_validate_eeprom(struct rt2x00_dev *rt2x00dev);
int rt2800_init_eeprom(struct rt2x00_dev *rt2x00dev);

int rt2800_probe_rt(struct rt2x00_dev *rt2x00dev);

int rt2800_init_registers(struct rt2x00_dev *rt2x00dev);

static inline bool rt2800_clk_is_20mhz(struct rt2x00_dev *rt2x00dev)
{
    /*
     * clk is for SOC only, and we don't want to deal with all the kernel
     * stuff.  We never have a SOC chip in USB.
     */
    return false;
}


#endif /* ifndef RT2800LIB_H */

