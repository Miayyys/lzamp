/* SPDX-License-Identifier: MIT */
#ifndef MAILMSG_MAILBOX0_ADAPTER_H
#define MAILMSG_MAILBOX0_ADAPTER_H

#include "mailmsg_notify.h"

/*
 * Platform glue supplies one mailbox doorbell primitive.  It normalizes a
 * successfully submitted doorbell to MAILMSG_NOTIFY_SENT and a busy pending
 * hardware channel to MAILMSG_NOTIFY_COALESCED.  Other negative errno-style
 * values remain notification failures.
 */
typedef int (*mailmsg_mailbox_send_fn)(void *context, mailmsg_u32 channel,
				      mailmsg_u32 command, mailmsg_u32 data);

struct mailmsg_mailbox0 {
	void *context;
	mailmsg_mailbox_send_fn send;
};

/* mailbox0 logical ch0..ch3 directly represent protocol priorities 0..3. */
int mailmsg_mailbox0_notify(void *context, enum mailmsg_priority priority);

extern const struct mailmsg_notify_ops mailmsg_mailbox0_notify_ops;

#endif
