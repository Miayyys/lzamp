/* SPDX-License-Identifier: MIT */
/*
 * Hardware-neutral shared-memory protocol for the R1 AMP prototype.
 * The notification transport is deliberately outside this file.
 */
#ifndef MAILMSG_PROTOCOL_H
#define MAILMSG_PROTOCOL_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 mailmsg_u8;
typedef u32 mailmsg_u32;
typedef s32 mailmsg_s32;
#else
#include <stdint.h>
typedef uint8_t mailmsg_u8;
typedef uint32_t mailmsg_u32;
typedef int32_t mailmsg_s32;
#endif

#define MAILMSG_PROTOCOL_MAGIC 0x4d4d5347U /* "MMSG" */
#define MAILMSG_PROTOCOL_VERSION 7U
#define MAILMSG_PRIORITY_COUNT 4U
#define MAILMSG_RING_SLOTS 8U
#define MAILMSG_PAYLOAD_BYTES 28U
#define MAILMSG_USER_PAYLOAD_BYTES 32U
#define MAILMSG_RELIABLE_PRIORITY_MASK 0x3U
#define MAILMSG_TX_FULL_OBSERVATION_MAGIC 0x4d46554cU /* "MFUL" */

enum mailmsg_priority {
	MAILMSG_PRIO_CRITICAL = 0,
	MAILMSG_PRIO_CONTROL = 1,
	MAILMSG_PRIO_NORMAL = 2,
	MAILMSG_PRIO_BEST_EFFORT = 3,
};

enum mailmsg_message_type {
	MAILMSG_MSG_NOP = 0,
	MAILMSG_MSG_PING = 1,
	MAILMSG_MSG_PONG = 2,
	MAILMSG_MSG_ACK = 3,
	MAILMSG_MSG_NACK = 4,
	/* p0 lifecycle control.  READY/REFUSED use the feedback payload layout. */
	MAILMSG_MSG_STOP_REQUEST = 5,
	MAILMSG_MSG_STOP_READY = 6,
	MAILMSG_MSG_STOP_REFUSED = 7,
	/* CPU3 publishes READY only after binding this exact session generation. */
	MAILMSG_MSG_SESSION_READY = 8,
	/* p1 read-only GPIO service.  The Linux character ABI is unchanged. */
	MAILMSG_MSG_GPIO_READ_REQUEST = 9,
	MAILMSG_MSG_GPIO_READ_RESULT = 10,
	/* Generic bounded peripheral RPC.  See mailmsg_peripheral_operation. */
	MAILMSG_MSG_PERIPHERAL_REQUEST = 11,
	MAILMSG_MSG_PERIPHERAL_RESULT = 12,
};

enum mailmsg_peripheral_operation {
	MAILMSG_PERIPH_GPIO_CONFIG = 1,
	MAILMSG_PERIPH_GPIO_WRITE = 2,
	MAILMSG_PERIPH_GPIO_READ = 3,
	MAILMSG_PERIPH_UART_WRITE = 4,
	MAILMSG_PERIPH_UART_READ = 5,
	MAILMSG_PERIPH_I2C_WRITE = 6,
	MAILMSG_PERIPH_I2C_READ = 7,
	MAILMSG_PERIPH_I2C_WRITE_READ = 8,
	MAILMSG_PERIPH_SPI_TRANSFER = 9,
	MAILMSG_PERIPH_PWM_SET = 10,
	MAILMSG_PERIPH_PWM_STOP = 11,
	MAILMSG_PERIPH_ADC_READ = 12,
};

enum mailmsg_nack_reason {
	MAILMSG_NACK_BAD_CRC = 1,
	MAILMSG_NACK_INVALID_FRAME = 2,
};

enum mailmsg_stop_refused_reason {
	MAILMSG_STOP_REFUSED_INVALID_REQUEST = 1,
	MAILMSG_STOP_REFUSED_BUSY = 2,
};

enum mailmsg_ring_result {
	MAILMSG_RING_OK = 0,
	MAILMSG_RING_EMPTY = -1,
	MAILMSG_RING_FULL = -2,
	MAILMSG_RING_INCOMPLETE = -3,
	MAILMSG_RING_BAD_CRC = -4,
	MAILMSG_RING_INVALID = -5,
};

/* Reliability is fixed per priority, not selected by untrusted frame flags. */
static inline int mailmsg_priority_is_reliable(mailmsg_u32 priority)
{
	return priority < MAILMSG_PRIORITY_COUNT &&
		(MAILMSG_RELIABLE_PRIORITY_MASK & (1U << priority));
}

/* One producer and one consumer own each ring; commit is written last. */
struct mailmsg_message {
	/* Linux-owned session epoch, copied into every committed frame. */
	mailmsg_u32 generation;
	mailmsg_u32 type;
	mailmsg_u32 sequence;
	mailmsg_u32 length;
	mailmsg_u8 payload[MAILMSG_PAYLOAD_BYTES];
	mailmsg_u32 crc32;
	mailmsg_u32 commit;
};

/* ACK/NACK payload layout: original frame sequence, then status/reason. */
#define MAILMSG_FEEDBACK_SEQUENCE_OFFSET 0U
#define MAILMSG_FEEDBACK_STATUS_OFFSET 4U
#define MAILMSG_FEEDBACK_BYTES 8U

/* SESSION_READY payload: shared generation followed by protocol version. */
#define MAILMSG_SESSION_GENERATION_OFFSET 0U
#define MAILMSG_SESSION_VERSION_OFFSET 4U
#define MAILMSG_SESSION_BYTES 8U

/* GPIO_READ_REQUEST payload: owned GPIO line number. */
#define MAILMSG_GPIO_READ_LINE_OFFSET 0U
#define MAILMSG_GPIO_READ_REQUEST_BYTES 4U

/* GPIO_READ_RESULT payload: request sequence, line number, sampled level. */
#define MAILMSG_GPIO_RESULT_SEQUENCE_OFFSET 0U
#define MAILMSG_GPIO_RESULT_LINE_OFFSET 4U
#define MAILMSG_GPIO_RESULT_VALUE_OFFSET 8U
#define MAILMSG_GPIO_READ_RESULT_BYTES 12U

/* Bounded peripheral RPC: five u32 fields and up to eight inline bytes. */
#define MAILMSG_PERIPH_OP_OFFSET 0U
#define MAILMSG_PERIPH_ARG0_OFFSET 4U
#define MAILMSG_PERIPH_ARG1_OFFSET 8U
#define MAILMSG_PERIPH_ARG2_OFFSET 12U
#define MAILMSG_PERIPH_LENGTH_OFFSET 16U
#define MAILMSG_PERIPH_DATA_OFFSET 20U
#define MAILMSG_PERIPH_INLINE_BYTES 8U
#define MAILMSG_PERIPH_REQUEST_BYTES 28U

/* Result: request sequence, signed status, operation, value, len, data[8]. */
#define MAILMSG_PERIPH_RESULT_SEQUENCE_OFFSET 0U
#define MAILMSG_PERIPH_RESULT_STATUS_OFFSET 4U
#define MAILMSG_PERIPH_RESULT_OP_OFFSET 8U
#define MAILMSG_PERIPH_RESULT_VALUE_OFFSET 12U
#define MAILMSG_PERIPH_RESULT_LENGTH_OFFSET 16U
#define MAILMSG_PERIPH_RESULT_DATA_OFFSET 20U
#define MAILMSG_PERIPH_RESULT_BYTES 28U

/* CRC-32/ISO-HDLC over a frame's business fields, excluding crc32/commit. */
static inline mailmsg_u32 mailmsg_frame_crc32(const struct mailmsg_message *message)
{
	const mailmsg_u8 *bytes;
	mailmsg_u32 crc = 0xffffffffU;
	mailmsg_u32 index, bit;

	bytes = (const mailmsg_u8 *)&message->generation;
	for (index = 0; index < sizeof(message->generation) +
		     sizeof(message->type) +
		     sizeof(message->sequence) + sizeof(message->length) +
		     sizeof(message->payload); index++) {
		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
	}
	return ~crc;
}

struct mailmsg_ring {
	mailmsg_u32 producer;
	mailmsg_u32 consumer;
	struct mailmsg_message slot[MAILMSG_RING_SLOTS];
};

struct mailmsg_shared {
	mailmsg_u32 magic;
	mailmsg_u32 version;
	/* Non-zero Linux-owned epoch.  It changes at every successful start. */
	mailmsg_u32 generation;
	struct mailmsg_ring linux_to_cpu3[MAILMSG_PRIORITY_COUNT];
	struct mailmsg_ring cpu3_to_linux[MAILMSG_PRIORITY_COUNT];
};

/*
 * Optional diagnostics outside struct mailmsg_shared.  It is intentionally
 * kept separate from the ring ABI: a CPU3 test service may publish reverse
 * queue-full observations here, while a production endpoint is free to use
 * the return value from mailmsg_endpoint_send() directly.
 */
struct mailmsg_tx_full_observation {
	mailmsg_u32 magic;
	mailmsg_u32 commit;
	mailmsg_u32 commit_inv;
	mailmsg_u32 full_count;
	mailmsg_u32 last_priority;
	mailmsg_u32 last_type;
	mailmsg_s32 last_result;
	mailmsg_u8 reserved[64U - 28U];
} __attribute__((aligned(64)));

/*
 * Platform hooks are the only ordering/cache dependency of the protocol.
 * Linux and Zephyr supply their own implementations; mailbox/SGI never
 * appears in the queue ABI.
 */
struct mailmsg_memory_ops {
	void (*publish)(const void *addr, mailmsg_u32 len);
	void (*acquire)(const void *addr, mailmsg_u32 len);
};

int mailmsg_ring_push(struct mailmsg_ring *ring, const struct mailmsg_memory_ops *ops,
		     const struct mailmsg_message *message);
int mailmsg_ring_pop(struct mailmsg_ring *ring, const struct mailmsg_memory_ops *ops,
		    struct mailmsg_message *message);
int mailmsg_ring_has_data(struct mailmsg_ring *ring,
			  const struct mailmsg_memory_ops *ops);
int mailmsg_ring_count(struct mailmsg_ring *ring,
		       const struct mailmsg_memory_ops *ops);

#endif
