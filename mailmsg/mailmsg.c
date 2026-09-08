/* SPDX-License-Identifier: MIT */
#include "mailmsg.h"

static mailmsg_u32 mailmsg_next(mailmsg_u32 value)
{
	return (value + 1U) % MAILMSG_RING_SLOTS;
}

int mailmsg_ring_push(struct mailmsg_ring *ring, const struct mailmsg_memory_ops *ops,
		     const struct mailmsg_message *message)
{
	mailmsg_u32 producer = ring->producer;
	mailmsg_u32 next = mailmsg_next(producer);
	struct mailmsg_message *slot;

	if (!message->generation || !message->sequence ||
	    message->length > MAILMSG_PAYLOAD_BYTES)
		return MAILMSG_RING_INVALID;
	ops->acquire(&ring->consumer, sizeof(ring->consumer));
	if (next == ring->consumer)
		return MAILMSG_RING_FULL;

	slot = &ring->slot[producer];
	*slot = *message;
	slot->commit = 0;
	slot->crc32 = mailmsg_frame_crc32(slot);
	ops->publish(slot, sizeof(*slot));
	slot->commit = message->sequence;
	ops->publish(&slot->commit, sizeof(slot->commit));
	ring->producer = next;
	ops->publish(&ring->producer, sizeof(ring->producer));
	return 0;
}

int mailmsg_ring_pop(struct mailmsg_ring *ring, const struct mailmsg_memory_ops *ops,
		    struct mailmsg_message *message)
{
	mailmsg_u32 consumer = ring->consumer;
	struct mailmsg_message *slot;

	ops->acquire(&ring->producer, sizeof(ring->producer));
	if (consumer == ring->producer)
		return MAILMSG_RING_EMPTY;

	slot = &ring->slot[consumer];
	ops->acquire(slot, sizeof(*slot));
	if (slot->commit != slot->sequence)
		return MAILMSG_RING_INCOMPLETE;
	*message = *slot;
	ring->consumer = mailmsg_next(consumer);
	ops->publish(&ring->consumer, sizeof(ring->consumer));
	/* A committed slot is always released promptly.  Non-OK results leave
	 * message payload untrusted, but retain sequence for an optional NACK. */
	if (!message->generation || !message->sequence ||
	    message->length > MAILMSG_PAYLOAD_BYTES)
		return MAILMSG_RING_INVALID;
	if (message->crc32 != mailmsg_frame_crc32(message))
		return MAILMSG_RING_BAD_CRC;
	return 0;
}

int mailmsg_ring_has_data(struct mailmsg_ring *ring,
			  const struct mailmsg_memory_ops *ops)
{
	if (!ring || !ops || !ops->acquire)
		return MAILMSG_RING_INVALID;

	ops->acquire(&ring->producer, sizeof(ring->producer));
	return ring->producer != ring->consumer;
}

int mailmsg_ring_count(struct mailmsg_ring *ring,
		       const struct mailmsg_memory_ops *ops)
{
	mailmsg_u32 producer, consumer;

	if (!ring || !ops || !ops->acquire)
		return MAILMSG_RING_INVALID;

	ops->acquire(&ring->producer, sizeof(ring->producer));
	ops->acquire(&ring->consumer, sizeof(ring->consumer));
	producer = ring->producer;
	consumer = ring->consumer;
	if (producer >= MAILMSG_RING_SLOTS || consumer >= MAILMSG_RING_SLOTS)
		return MAILMSG_RING_INVALID;
	return producer >= consumer ? producer - consumer :
		MAILMSG_RING_SLOTS - consumer + producer;
}
