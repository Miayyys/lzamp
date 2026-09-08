/* SPDX-License-Identifier: MIT */
#include "mailmsg_endpoint.h"

static mailmsg_u32 mailmsg_endpoint_next_sequence(struct mailmsg_endpoint *endpoint)
{
	endpoint->next_sequence++;
	if (!endpoint->next_sequence)
		endpoint->next_sequence++;
	return endpoint->next_sequence;
}

static void mailmsg_endpoint_reset_stats(struct mailmsg_endpoint *endpoint)
{
	mailmsg_u32 priority;

	for (priority = 0; priority < MAILMSG_PRIORITY_COUNT; priority++)
		endpoint->stats.priority[priority] =
			(struct mailmsg_priority_stats){ 0 };
}

int mailmsg_endpoint_session_valid(struct mailmsg_endpoint *endpoint)
{
	if (!endpoint || !endpoint->shared || !endpoint->memory_ops ||
	    !endpoint->memory_ops->acquire || !endpoint->generation)
		return MAILMSG_ENDPOINT_INVALID;

	endpoint->memory_ops->acquire(&endpoint->shared->magic,
				      sizeof(endpoint->shared->magic) +
				      sizeof(endpoint->shared->version) +
				      sizeof(endpoint->shared->generation));
	if (endpoint->shared->magic != MAILMSG_PROTOCOL_MAGIC ||
	    endpoint->shared->version != MAILMSG_PROTOCOL_VERSION)
		return MAILMSG_ENDPOINT_PROTOCOL_MISMATCH;
	if (endpoint->shared->generation != endpoint->generation)
		return MAILMSG_ENDPOINT_STALE_SESSION;
	return MAILMSG_ENDPOINT_OK;
}

int mailmsg_endpoint_bind(struct mailmsg_endpoint *endpoint,
			  struct mailmsg_shared *shared,
			  const struct mailmsg_memory_ops *memory_ops,
			  const struct mailmsg_notify_endpoint *notify,
			  enum mailmsg_endpoint_role role)
{
	if (!endpoint || !shared || !memory_ops || !memory_ops->publish ||
	    !memory_ops->acquire || !notify || !notify->ops ||
	    !notify->ops->notify || role < MAILMSG_ENDPOINT_LINUX ||
	    role > MAILMSG_ENDPOINT_CPU3)
		return MAILMSG_ENDPOINT_INVALID;

	memory_ops->acquire(shared, sizeof(shared->magic) + sizeof(shared->version) +
			    sizeof(shared->generation));
	if (shared->magic != MAILMSG_PROTOCOL_MAGIC ||
	    shared->version != MAILMSG_PROTOCOL_VERSION)
		return MAILMSG_ENDPOINT_PROTOCOL_MISMATCH;
	if (!shared->generation)
		return MAILMSG_ENDPOINT_STALE_SESSION;

	endpoint->shared = shared;
	endpoint->memory_ops = memory_ops;
	endpoint->notify = notify;
	endpoint->next_sequence = 0;
	endpoint->generation = shared->generation;
	mailmsg_endpoint_reset_stats(endpoint);
	if (role == MAILMSG_ENDPOINT_LINUX) {
		endpoint->tx = shared->linux_to_cpu3;
		endpoint->rx = shared->cpu3_to_linux;
	} else {
		endpoint->tx = shared->cpu3_to_linux;
		endpoint->rx = shared->linux_to_cpu3;
	}
	return MAILMSG_ENDPOINT_OK;
}

int mailmsg_endpoint_send(struct mailmsg_endpoint *endpoint,
			  enum mailmsg_priority priority,
			  mailmsg_u32 type, const mailmsg_u8 *payload,
			  mailmsg_u32 length,
			  struct mailmsg_send_result *result)
{
	struct mailmsg_message message = { 0 };
	struct mailmsg_priority_stats *stats;
	mailmsg_u32 index;
	int queue_result, depth, session_result;

	if (!endpoint || !endpoint->tx || !endpoint->memory_ops || !result ||
	    priority >= MAILMSG_PRIORITY_COUNT ||
	    length > MAILMSG_PAYLOAD_BYTES || (length && !payload))
		return MAILMSG_ENDPOINT_INVALID;

	result->queue_result = MAILMSG_RING_INVALID;
	result->notify_result = -1;
	result->notify_attempted = 0;
	result->sequence = 0;
	session_result = mailmsg_endpoint_session_valid(endpoint);
	if (session_result)
		return session_result;
	stats = &endpoint->stats.priority[priority];

	message.generation = endpoint->generation;
	message.type = type;
	message.sequence = mailmsg_endpoint_next_sequence(endpoint);
	message.length = length;
	for (index = 0; index < length; index++)
		message.payload[index] = payload[index];

	queue_result = mailmsg_ring_push(&endpoint->tx[priority],
					 endpoint->memory_ops, &message);
	result->queue_result = queue_result;
	if (queue_result) {
		if (queue_result == MAILMSG_RING_FULL)
			stats->tx_full++;
		/* The generated value was never committed and is not a frame ID. */
		result->sequence = 0;
		return queue_result;
	}
	stats->tx_enqueued++;
	depth = mailmsg_ring_count(&endpoint->tx[priority], endpoint->memory_ops);
	if (depth > 0 && (mailmsg_u32)depth > stats->tx_high_water)
		stats->tx_high_water = depth;
	result->sequence = message.sequence;

	result->notify_attempted = 1;
	result->notify_result = mailmsg_notify(endpoint->notify, priority);
	if (result->notify_result == MAILMSG_NOTIFY_SENT)
		stats->notify_sent++;
	else if (result->notify_result == MAILMSG_NOTIFY_COALESCED)
		stats->notify_coalesced++;
	else
		stats->notify_failed++;
	return MAILMSG_ENDPOINT_OK;
}

int mailmsg_endpoint_receive(struct mailmsg_endpoint *endpoint,
			     enum mailmsg_priority priority,
			     struct mailmsg_message *message)
{
	struct mailmsg_priority_stats *stats;
	int ret;

	if (!endpoint || !endpoint->rx || !endpoint->memory_ops || !message ||
	    priority >= MAILMSG_PRIORITY_COUNT)
		return MAILMSG_ENDPOINT_INVALID;
	ret = mailmsg_endpoint_session_valid(endpoint);
	if (ret)
		return ret;

	ret = mailmsg_ring_pop(&endpoint->rx[priority], endpoint->memory_ops,
			       message);
	stats = &endpoint->stats.priority[priority];
	if (!ret && message->generation != endpoint->generation) {
		stats->rx_stale++;
		ret = MAILMSG_ENDPOINT_STALE_FRAME;
	}
	if (!ret)
		stats->rx_ok++;
	else if (ret == MAILMSG_RING_INCOMPLETE)
		stats->rx_incomplete++;
	else if (ret == MAILMSG_RING_BAD_CRC)
		stats->rx_bad_crc++;
	else if (ret == MAILMSG_RING_INVALID)
		stats->rx_invalid++;
	return ret;
}

int mailmsg_endpoint_has_received(struct mailmsg_endpoint *endpoint,
				  enum mailmsg_priority priority)
{
	if (!endpoint || !endpoint->rx || !endpoint->memory_ops ||
	    priority >= MAILMSG_PRIORITY_COUNT)
		return MAILMSG_RING_INVALID;
	if (mailmsg_endpoint_session_valid(endpoint))
		return MAILMSG_RING_INVALID;

	return mailmsg_ring_has_data(&endpoint->rx[priority],
				     endpoint->memory_ops);
}
