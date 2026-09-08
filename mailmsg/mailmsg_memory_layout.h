/* SPDX-License-Identifier: MIT */
#ifndef MAILMSG_MEMORY_LAYOUT_H
#define MAILMSG_MEMORY_LAYOUT_H

/*
 * LZAMP AMP DRAM layout.  MailMsg remains the control plane; the four data
 * regions are reserved for later zero-copy queues and are not part of the
 * current MailMsg ring ABI.
 */
#define LZAMP_AMP_BASE_ADDR             0x50000000UL
#define LZAMP_AMP_TOTAL_SIZE            0x02000000UL

#define LZAMP_ZEPHYR_REGION_ADDR        0x50000000UL
#define LZAMP_ZEPHYR_REGION_SIZE        0x00400000UL
#define LZAMP_MAILMSG_CONTROL_ADDR      0x503ff000UL
#define LZAMP_MAILMSG_CONTROL_SIZE      0x00001000UL
#define LZAMP_ZEPHYR_LINKABLE_SIZE      0x003ff000UL

#define LZAMP_DATA_P0_ADDR              0x50400000UL
#define LZAMP_DATA_P0_SIZE              0x00100000UL
#define LZAMP_DATA_P1_ADDR              0x50500000UL
#define LZAMP_DATA_P1_SIZE              0x00100000UL
#define LZAMP_DATA_P2_ADDR              0x50600000UL
#define LZAMP_DATA_P2_SIZE              0x00400000UL
#define LZAMP_DATA_P3_ADDR              0x50a00000UL
#define LZAMP_DATA_P3_SIZE              0x01000000UL

#define LZAMP_DATA_FUTURE_ADDR          0x51a00000UL
#define LZAMP_DATA_FUTURE_SIZE          0x00600000UL

#if LZAMP_MAILMSG_CONTROL_ADDR + LZAMP_MAILMSG_CONTROL_SIZE != LZAMP_DATA_P0_ADDR
#error "LZAMP control page must end at the P0 boundary"
#endif
#if LZAMP_DATA_P0_ADDR + LZAMP_DATA_P0_SIZE != LZAMP_DATA_P1_ADDR
#error "LZAMP P0 and P1 regions must be adjacent"
#endif
#if LZAMP_DATA_P1_ADDR + LZAMP_DATA_P1_SIZE != LZAMP_DATA_P2_ADDR
#error "LZAMP P1 and P2 regions must be adjacent"
#endif
#if LZAMP_DATA_P2_ADDR + LZAMP_DATA_P2_SIZE != LZAMP_DATA_P3_ADDR
#error "LZAMP P2 and P3 regions must be adjacent"
#endif
#if LZAMP_DATA_P3_ADDR + LZAMP_DATA_P3_SIZE != LZAMP_DATA_FUTURE_ADDR
#error "LZAMP P3 and future regions must be adjacent"
#endif
#if LZAMP_DATA_FUTURE_ADDR + LZAMP_DATA_FUTURE_SIZE != \
	LZAMP_AMP_BASE_ADDR + LZAMP_AMP_TOTAL_SIZE
#error "LZAMP AMP layout must fill the 32 MiB carveout"
#endif

#endif
