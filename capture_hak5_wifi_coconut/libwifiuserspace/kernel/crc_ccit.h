/* SPDX-License-Identifier: GPL-2.0 */

/* 
 * Userspace port (c) 2019 Hak5
 */

#ifndef __USERSPACE_CCIT_H__
#define __USERSPACE_CCIT_H__ 

#include <stdlib.h>

#include "kernel/types.h"

extern uint16_t const crc_ccitt_table[256];
extern uint16_t const crc_ccitt_false_table[256];

extern uint16_t crc_ccitt(uint16_t crc, const uint8_t *buffer, size_t len);
extern uint16_t crc_ccitt_false(uint16_t crc, const uint8_t *buffer, size_t len);

static inline uint16_t crc_ccitt_byte(uint16_t crc, const uint8_t c)
{
	return (crc >> 8) ^ crc_ccitt_table[(crc ^ c) & 0xff];
}

static inline uint16_t crc_ccitt_false_byte(uint16_t crc, const uint8_t c)
{
    return (crc << 8) ^ crc_ccitt_false_table[(crc >> 8) ^ c];
}

#endif /* ifndef USERSPACE_CCIT_H */

