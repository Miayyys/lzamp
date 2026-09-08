/* SPDX-License-Identifier: MIT */
/* Minimal userspace smoke test for one priority-specific MailMsg device. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mailmsg_user.h"

static void store_u32(mailmsg_u8 *bytes, mailmsg_u32 value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
	bytes[2] = value >> 16;
	bytes[3] = value >> 24;
}

static mailmsg_u32 load_u32(const mailmsg_u8 *bytes)
{
	return (mailmsg_u32)bytes[0] |
	       ((mailmsg_u32)bytes[1] << 8) |
	       ((mailmsg_u32)bytes[2] << 16) |
	       ((mailmsg_u32)bytes[3] << 24);
}

int main(int argc, char **argv)
{
	struct mailmsg_user_frame frame = { 0 };
	struct pollfd pollfd;
	char path[32];
	char *end;
	unsigned long priority, value;
	int fd, expected, index;
	ssize_t ret;

	if (argc != 3 && (argc != 4 || strcmp(argv[3], "--no-read"))) {
		fprintf(stderr, "usage: %s <priority 0..3> <ping-value> [--no-read]\n",
			argv[0]);
		return 2;
	}
	priority = strtoul(argv[1], &end, 0);
	if (*end || priority >= MAILMSG_PRIORITY_COUNT) {
		fprintf(stderr, "invalid priority: %s\n", argv[1]);
		return 2;
	}
	value = strtoul(argv[2], &end, 0);
	if (*end || value > UINT32_MAX) {
		fprintf(stderr, "invalid value: %s\n", argv[2]);
		return 2;
	}

	snprintf(path, sizeof(path), "/dev/mailmsg-p%lu", priority);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	frame.priority = priority;
	frame.type = MAILMSG_MSG_PING;
	frame.length = sizeof(mailmsg_u32);
	store_u32(frame.payload, value);
	ret = write(fd, &frame, sizeof(frame));
	if (ret != sizeof(frame)) {
		if (ret >= 0)
			errno = EIO;
		perror("mailmsg write");
		close(fd);
		return 1;
	}

	expected = argc == 4 ? 0 :
		mailmsg_priority_is_reliable(priority) ? 2 : 1;
	for (index = 0; index < expected; index++) {
		pollfd.fd = fd;
		pollfd.events = POLLIN;
		pollfd.revents = 0;
		ret = poll(&pollfd, 1, 1000);
		if (ret <= 0) {
			if (!ret)
				fprintf(stderr, "mailmsg read timed out\n");
			else
				perror("mailmsg poll");
			close(fd);
			return 1;
		}
		if (!(pollfd.revents & POLLIN)) {
			fprintf(stderr, "mailmsg poll revents=%#x\n", pollfd.revents);
			close(fd);
			return 1;
		}
		memset(&frame, 0, sizeof(frame));
		ret = read(fd, &frame, sizeof(frame));
		if (ret != sizeof(frame)) {
			if (ret >= 0)
				errno = EIO;
			perror("mailmsg read");
			close(fd);
			return 1;
		}
		printf("priority=%u type=%u sequence=%u length=%u",
		       frame.priority, frame.type, frame.sequence, frame.length);
		if ((frame.type == MAILMSG_MSG_ACK || frame.type == MAILMSG_MSG_NACK) &&
		    frame.length >= MAILMSG_FEEDBACK_BYTES)
			printf(" peer_sequence=%u status=%d", load_u32(frame.payload),
			       (int32_t)load_u32(frame.payload + MAILMSG_FEEDBACK_STATUS_OFFSET));
		else if (frame.length >= sizeof(mailmsg_u32))
			printf(" value=%u", load_u32(frame.payload));
		putchar('\n');
	}

	close(fd);
	return 0;
}
