/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>

#include "mailmsg.h"

_Static_assert(sizeof(struct mailmsg_tx_full_observation) == 64,
	       "TX-full observation must retain its dedicated cache line");
_Static_assert(sizeof(struct mailmsg_shared) <= 0xe00,
	       "MailMsg shared ABI must fit the final status-page window");

static void no_op(const void *addr, uint32_t len)
{
	(void)addr;
	(void)len;
}

int main(void)
{
	struct mailmsg_memory_ops ops = { .publish = no_op, .acquire = no_op };
	struct mailmsg_ring ring = { 0 };
	struct mailmsg_message in = {
		.generation = 3,
		.type = MAILMSG_MSG_PING,
		.sequence = 7,
		.length = 2,
		.payload = { 0x12, 0x34 },
	};
	struct mailmsg_message out = { 0 };

	assert(mailmsg_priority_is_reliable(MAILMSG_PRIO_CRITICAL));
	assert(mailmsg_priority_is_reliable(MAILMSG_PRIO_CONTROL));
	assert(!mailmsg_priority_is_reliable(MAILMSG_PRIO_NORMAL));
	assert(!mailmsg_priority_is_reliable(MAILMSG_PRIO_BEST_EFFORT));
	assert(MAILMSG_TX_FULL_OBSERVATION_MAGIC == 0x4d46554cU);
	assert(MAILMSG_MSG_GPIO_READ_REQUEST == 9U);
	assert(MAILMSG_MSG_GPIO_READ_RESULT == 10U);
	assert(MAILMSG_GPIO_READ_RESULT_BYTES <= MAILMSG_PAYLOAD_BYTES);

	assert(mailmsg_ring_push(&ring, &ops, &in) == 0);
	assert(mailmsg_ring_count(&ring, &ops) == 1);
	assert(mailmsg_ring_pop(&ring, &ops, &out) == 0);
	assert(mailmsg_ring_count(&ring, &ops) == 0);
	assert(out.type == MAILMSG_MSG_PING);
	assert(out.generation == 3);
	assert(out.sequence == 7);
	assert(out.length == 2);
	assert(memcmp(out.payload, in.payload, in.length) == 0);
	assert(mailmsg_ring_pop(&ring, &ops, &out) == MAILMSG_RING_EMPTY);

	memset(&in, 0, sizeof(in));
	in.generation = 3;
	in.type = MAILMSG_MSG_GPIO_READ_RESULT;
	in.sequence = 8;
	in.length = MAILMSG_GPIO_READ_RESULT_BYTES;
	in.payload[MAILMSG_GPIO_RESULT_SEQUENCE_OFFSET] = 7;
	in.payload[MAILMSG_GPIO_RESULT_LINE_OFFSET] = 25;
	in.payload[MAILMSG_GPIO_RESULT_VALUE_OFFSET] = 1;
	assert(mailmsg_ring_push(&ring, &ops, &in) == MAILMSG_RING_OK);
	assert(mailmsg_ring_pop(&ring, &ops, &out) == MAILMSG_RING_OK);
	assert(out.type == MAILMSG_MSG_GPIO_READ_RESULT);
	assert(out.length == MAILMSG_GPIO_READ_RESULT_BYTES);
	assert(memcmp(out.payload, in.payload, in.length) == 0);

	assert(mailmsg_ring_push(&ring, &ops, &in) == MAILMSG_RING_OK);
	ring.slot[2].payload[0] ^= 0xffU;
	assert(mailmsg_ring_pop(&ring, &ops, &out) == MAILMSG_RING_BAD_CRC);
	assert(ring.consumer == 3);
	in.sequence = 8;
	assert(mailmsg_ring_push(&ring, &ops, &in) == MAILMSG_RING_OK);
	assert(mailmsg_ring_pop(&ring, &ops, &out) == MAILMSG_RING_OK);
	assert(out.sequence == 8);

	/* Eight physical slots leave capacity for seven committed frames.  The
	 * producer gets FULL immediately; it must not wait or overwrite a slot. */
	for (in.sequence = 9; in.sequence < 16; in.sequence++)
		assert(mailmsg_ring_push(&ring, &ops, &in) == MAILMSG_RING_OK);
	assert(mailmsg_ring_count(&ring, &ops) == MAILMSG_RING_SLOTS - 1);
	in.sequence = 16;
	assert(mailmsg_ring_push(&ring, &ops, &in) == MAILMSG_RING_FULL);
	assert(mailmsg_ring_pop(&ring, &ops, &out) == MAILMSG_RING_OK);
	assert(mailmsg_ring_push(&ring, &ops, &in) == MAILMSG_RING_OK);
	ring.producer = MAILMSG_RING_SLOTS;
	assert(mailmsg_ring_count(&ring, &ops) == MAILMSG_RING_INVALID);
	return 0;
}
