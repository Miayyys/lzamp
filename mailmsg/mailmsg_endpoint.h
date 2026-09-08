/* SPDX-License-Identifier: MIT */
#ifndef MAILMSG_ENDPOINT_H
#define MAILMSG_ENDPOINT_H

#include "mailmsg.h"
#include "mailmsg_notify.h"

enum mailmsg_endpoint_role {
	MAILMSG_ENDPOINT_LINUX = 0,
	MAILMSG_ENDPOINT_CPU3 = 1,
};

enum mailmsg_endpoint_result {
	MAILMSG_ENDPOINT_OK = 0,
	/* Keep endpoint errors disjoint from the small negative ring results. */
	MAILMSG_ENDPOINT_INVALID = -100,
	MAILMSG_ENDPOINT_PROTOCOL_MISMATCH = -101,
	MAILMSG_ENDPOINT_STALE_SESSION = -102,
	/* A committed frame was consumed but belongs to an older generation. */
	MAILMSG_ENDPOINT_STALE_FRAME = -103,
};

struct mailmsg_priority_stats {
	mailmsg_u32 tx_enqueued;
	mailmsg_u32 tx_full;
	mailmsg_u32 tx_high_water;
	mailmsg_u32 notify_sent;
	mailmsg_u32 notify_coalesced;
	mailmsg_u32 notify_failed;
	mailmsg_u32 rx_ok;
	mailmsg_u32 rx_incomplete;
	mailmsg_u32 rx_bad_crc;
	mailmsg_u32 rx_invalid;
	mailmsg_u32 rx_stale;
};

struct mailmsg_endpoint_stats {
	struct mailmsg_priority_stats priority[MAILMSG_PRIORITY_COUNT];
};

struct mailmsg_send_result {
	int queue_result;
	int notify_result;
	int notify_attempted;
	mailmsg_u32 sequence;
};

struct mailmsg_endpoint {
	struct mailmsg_shared *shared;
	struct mailmsg_ring *tx;
	struct mailmsg_ring *rx;
	const struct mailmsg_memory_ops *memory_ops;
	const struct mailmsg_notify_endpoint *notify;
	mailmsg_u32 next_sequence;
	mailmsg_u32 generation;
	struct mailmsg_endpoint_stats stats;
};

/*
 * Bind once for one endpoint lifetime.  Rebinding resets the local sequence
 * generator, so callers must not rebind while either direction still has
 * live frames.  A concrete notification backend is required; a polling-only
 * user can provide a no-op backend that explicitly returns SENT.
 */
int mailmsg_endpoint_bind(struct mailmsg_endpoint *endpoint,
			  struct mailmsg_shared *shared,
			  const struct mailmsg_memory_ops *memory_ops,
			  const struct mailmsg_notify_endpoint *notify,
			  enum mailmsg_endpoint_role role);

int mailmsg_endpoint_send(struct mailmsg_endpoint *endpoint,
			  enum mailmsg_priority priority,
			  mailmsg_u32 type, const mailmsg_u8 *payload,
			  mailmsg_u32 length,
			  struct mailmsg_send_result *result);

int mailmsg_endpoint_receive(struct mailmsg_endpoint *endpoint,
			     enum mailmsg_priority priority,
			     struct mailmsg_message *message);

int mailmsg_endpoint_has_received(struct mailmsg_endpoint *endpoint,
				  enum mailmsg_priority priority);

int mailmsg_endpoint_session_valid(struct mailmsg_endpoint *endpoint);

#endif
