/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <string.h>

#include "mailmsg_endpoint.h"
#include "mailmsg_user.h"

_Static_assert(sizeof(struct mailmsg_user_frame) == 48,
	       "MailMsg userspace frame ABI must remain fixed");

static int notify_result;
static int notify_count;
static enum mailmsg_priority notified_priority;

static void memory_noop(const void *addr, mailmsg_u32 len)
{
	(void)addr;
	(void)len;
}

static int notify_fake(void *context, enum mailmsg_priority priority)
{
	(void)context;
	notify_count++;
	notified_priority = priority;
	return notify_result;
}

static const struct mailmsg_memory_ops memory_ops = {
	.publish = memory_noop,
	.acquire = memory_noop,
};

static const struct mailmsg_notify_ops notify_ops = {
	.notify = notify_fake,
};

static const struct mailmsg_notify_endpoint notify_endpoint = {
	.ops = &notify_ops,
};

int main(void)
{
	struct mailmsg_shared shared;
	struct mailmsg_endpoint linux_endpoint;
	struct mailmsg_endpoint cpu3_endpoint;
	struct mailmsg_send_result send_result;
	struct mailmsg_message message;
	mailmsg_u8 payload[4] = { 41, 0, 0, 0 };
	mailmsg_u32 index;

	memset(&shared, 0, sizeof(shared));
	shared.magic = MAILMSG_PROTOCOL_MAGIC;
	shared.version = MAILMSG_PROTOCOL_VERSION;
	shared.generation = 7;
	assert(mailmsg_endpoint_bind(&linux_endpoint, &shared, &memory_ops,
				     &notify_endpoint,
				     MAILMSG_ENDPOINT_LINUX) == 0);
	assert(mailmsg_endpoint_bind(&cpu3_endpoint, &shared, &memory_ops,
				     &notify_endpoint,
				     MAILMSG_ENDPOINT_CPU3) == 0);

	notify_result = MAILMSG_NOTIFY_SENT;
	assert(mailmsg_endpoint_send(&linux_endpoint, MAILMSG_PRIO_CONTROL,
				     MAILMSG_MSG_PING, payload, sizeof(payload),
				     &send_result) == 0);
	assert(send_result.queue_result == MAILMSG_RING_OK);
	assert(send_result.notify_attempted);
	assert(send_result.notify_result == MAILMSG_NOTIFY_SENT);
	assert(send_result.sequence == 1);
	assert(linux_endpoint.generation == 7);
	assert(linux_endpoint.stats.priority[MAILMSG_PRIO_CONTROL].tx_enqueued == 1);
	assert(linux_endpoint.stats.priority[MAILMSG_PRIO_CONTROL].tx_high_water == 1);
	assert(linux_endpoint.stats.priority[MAILMSG_PRIO_CONTROL].notify_sent == 1);
	assert(notify_count == 1);
	assert(notified_priority == MAILMSG_PRIO_CONTROL);
	assert(mailmsg_endpoint_has_received(&cpu3_endpoint,
				     MAILMSG_PRIO_CONTROL));
	assert(mailmsg_endpoint_receive(&cpu3_endpoint, MAILMSG_PRIO_CONTROL,
					&message) == 0);
	assert(!mailmsg_endpoint_has_received(&cpu3_endpoint,
				      MAILMSG_PRIO_CONTROL));
	assert(message.type == MAILMSG_MSG_PING);
	assert(message.generation == 7);
	assert(message.sequence == 1);
	assert(message.payload[0] == 41);
	assert(cpu3_endpoint.stats.priority[MAILMSG_PRIO_CONTROL].rx_ok == 1);

	notify_result = MAILMSG_NOTIFY_COALESCED;
	assert(mailmsg_endpoint_send(&cpu3_endpoint, MAILMSG_PRIO_CRITICAL,
				     MAILMSG_MSG_PONG, payload, sizeof(payload),
				     &send_result) == 0);
	assert(send_result.notify_result == MAILMSG_NOTIFY_COALESCED);
	assert(cpu3_endpoint.stats.priority[MAILMSG_PRIO_CRITICAL].notify_coalesced == 1);
	assert(mailmsg_endpoint_receive(&linux_endpoint, MAILMSG_PRIO_CRITICAL,
					&message) == 0);
	assert(message.type == MAILMSG_MSG_PONG);
	assert(message.generation == 7);

	/* A committed old-session frame is released but never delivered. */
	shared.cpu3_to_linux[MAILMSG_PRIO_CONTROL].slot[0] =
		(struct mailmsg_message) {
			.generation = 6,
			.type = MAILMSG_MSG_PONG,
			.sequence = 99,
			.length = 0,
		};
	shared.cpu3_to_linux[MAILMSG_PRIO_CONTROL].slot[0].crc32 =
		mailmsg_frame_crc32(
			&shared.cpu3_to_linux[MAILMSG_PRIO_CONTROL].slot[0]);
	shared.cpu3_to_linux[MAILMSG_PRIO_CONTROL].slot[0].commit = 99;
	shared.cpu3_to_linux[MAILMSG_PRIO_CONTROL].producer = 1;
	assert(mailmsg_endpoint_receive(&linux_endpoint, MAILMSG_PRIO_CONTROL,
					&message) == MAILMSG_ENDPOINT_STALE_FRAME);
	assert(linux_endpoint.stats.priority[MAILMSG_PRIO_CONTROL].rx_stale == 1);

	/* Seven usable slots; FULL does not attempt another notification. */
	notify_count = 0;
	for (index = 0; index < MAILMSG_RING_SLOTS - 1; index++)
		assert(mailmsg_endpoint_send(&linux_endpoint, MAILMSG_PRIO_NORMAL,
					     MAILMSG_MSG_PING, payload,
					     sizeof(payload), &send_result) == 0);
	assert(notify_count == MAILMSG_RING_SLOTS - 1);
	assert(mailmsg_endpoint_send(&linux_endpoint, MAILMSG_PRIO_NORMAL,
				     MAILMSG_MSG_PING, payload, sizeof(payload),
				     &send_result) == MAILMSG_RING_FULL);
	assert(send_result.queue_result == MAILMSG_RING_FULL);
	assert(!send_result.notify_attempted);
	assert(send_result.sequence == 0);
	assert(notify_count == MAILMSG_RING_SLOTS - 1);
	assert(linux_endpoint.stats.priority[MAILMSG_PRIO_NORMAL].tx_full == 1);
	assert(linux_endpoint.stats.priority[MAILMSG_PRIO_NORMAL].tx_high_water ==
	       MAILMSG_RING_SLOTS - 1);

	notify_result = -16;
	assert(mailmsg_endpoint_send(&linux_endpoint, MAILMSG_PRIO_BEST_EFFORT,
				     MAILMSG_MSG_PING, payload, sizeof(payload),
				     &send_result) == MAILMSG_ENDPOINT_OK);
	assert(send_result.queue_result == MAILMSG_RING_OK);
	assert(send_result.notify_attempted);
	assert(send_result.notify_result == -16);
	assert(send_result.sequence != 0);
	assert(linux_endpoint.stats.priority[MAILMSG_PRIO_BEST_EFFORT].notify_failed == 1);

	shared.generation = 8;
	assert(mailmsg_endpoint_session_valid(&linux_endpoint) ==
	       MAILMSG_ENDPOINT_STALE_SESSION);
	assert(mailmsg_endpoint_send(&linux_endpoint, MAILMSG_PRIO_CRITICAL,
				     MAILMSG_MSG_PING, payload, sizeof(payload),
				     &send_result) == MAILMSG_ENDPOINT_STALE_SESSION);
	shared.generation = 7;
	assert(mailmsg_endpoint_session_valid(&linux_endpoint) == MAILMSG_ENDPOINT_OK);

	assert(mailmsg_endpoint_bind(&linux_endpoint, &shared, &memory_ops, NULL,
				     MAILMSG_ENDPOINT_LINUX) ==
	       MAILMSG_ENDPOINT_INVALID);
	assert(mailmsg_endpoint_bind(&linux_endpoint, &shared, &memory_ops,
				     &notify_endpoint,
				     (enum mailmsg_endpoint_role)-1) ==
	       MAILMSG_ENDPOINT_INVALID);

	shared.version++;
	assert(mailmsg_endpoint_bind(&linux_endpoint, &shared, &memory_ops,
				     &notify_endpoint,
				     MAILMSG_ENDPOINT_LINUX) ==
	       MAILMSG_ENDPOINT_PROTOCOL_MISMATCH);
	return 0;
}
