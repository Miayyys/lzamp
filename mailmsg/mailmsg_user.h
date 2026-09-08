/* SPDX-License-Identifier: MIT */
/*
 * Minimal userspace record ABI for one MailMsg priority endpoint.
 *
 * A process opens one of /dev/mailmsg-p0 through /dev/mailmsg-p3.  A write
 * submits a frame for that same priority; it returns -ENOSPC if the local
 * transmit ring is full.  A read returns one received frame from that one
 * priority.  Many file descriptors may write, but only one file descriptor
 * per priority may consume with read/poll at a time; a second consumer gets
 * -EBUSY (or EPOLLERR from poll).  The record deliberately contains only
 * protocol data: scheduling, retry, and delivery policy remain with the
 * caller.
 */
#ifndef MAILMSG_USER_H
#define MAILMSG_USER_H

#include "mailmsg.h"

struct mailmsg_user_frame {
	mailmsg_u32 priority;
	mailmsg_u32 type;
	mailmsg_u32 sequence;
	mailmsg_u32 length;
	/* Keep the userspace record ABI at 48 bytes while protocol V6 reserves
	 * four bytes in each shared-memory frame for its session generation. */
	mailmsg_u8 payload[MAILMSG_USER_PAYLOAD_BYTES];
};

#endif /* MAILMSG_USER_H */
