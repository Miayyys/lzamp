/* SPDX-License-Identifier: MIT */
#ifndef MAILMSG_NOTIFY_H
#define MAILMSG_NOTIFY_H

#include "mailmsg.h"

/*
 * Transport-neutral doorbell interface.
 *
 * A notification says only that the remote side should inspect its shared
 * memory queue.  It neither transports a MailMsg frame nor confirms that the
 * remote side has consumed one.
 */
enum mailmsg_notify_result {
	/* A new hardware/software notification was submitted. */
	MAILMSG_NOTIFY_SENT = 0,
	/* An equivalent notification is already pending; the frame is accepted. */
	MAILMSG_NOTIFY_COALESCED = 1,
};

/* The caller can separate protocol outcome from the backend errno. */
enum mailmsg_notify_state {
	MAILMSG_NOTIFY_STATE_SENT,
	MAILMSG_NOTIFY_STATE_COALESCED,
	MAILMSG_NOTIFY_STATE_FAILED,
};

/* Negative errno-style values retain the concrete notification failure. */
static inline enum mailmsg_notify_state
mailmsg_notify_state_from_result(int result)
{
	if (result == MAILMSG_NOTIFY_SENT)
		return MAILMSG_NOTIFY_STATE_SENT;
	if (result == MAILMSG_NOTIFY_COALESCED)
		return MAILMSG_NOTIFY_STATE_COALESCED;
	return MAILMSG_NOTIFY_STATE_FAILED;
}

static inline int mailmsg_notify_accepted(int result)
{
	return result == MAILMSG_NOTIFY_SENT ||
	       result == MAILMSG_NOTIFY_COALESCED;
}

struct mailmsg_notify_ops {
	int (*notify)(void *context, enum mailmsg_priority priority);
	int (*enable_rx)(void *context, enum mailmsg_priority priority);
};

struct mailmsg_notify_endpoint {
	const struct mailmsg_notify_ops *ops;
	void *context;
};

static inline int mailmsg_notify(const struct mailmsg_notify_endpoint *endpoint,
				enum mailmsg_priority priority)
{
	if (!endpoint || !endpoint->ops || !endpoint->ops->notify ||
	    priority >= MAILMSG_PRIORITY_COUNT)
		return -1;
	return endpoint->ops->notify(endpoint->context, priority);
}

#endif
