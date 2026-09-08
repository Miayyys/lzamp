/* SPDX-License-Identifier: MIT */
/*
 * Bounded single-priority MailMsg in-flight window benchmark.
 *
 * One persistent O_RDWR descriptor owns the priority's sole reader.  The
 * process fills up to --window requests, drains responses, and refills until
 * the duration expires.  This measures the current end-to-end userspace ABI;
 * it is not a hard-real-time or raw shared-memory benchmark.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mailmsg_user.h"

struct request_slot {
	uint32_t value;
	uint64_t start_ns;
	int active;
};

struct result_set {
	uint64_t *latency_ns;
	size_t count;
	size_t capacity;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
		perror("clock_gettime");
		exit(1);
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

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

static unsigned long parse_ulong(const char *text, const char *name,
				 unsigned long maximum)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 0);
	if (errno || *end || value > maximum) {
		fprintf(stderr, "invalid %s: %s\n", name, text);
		exit(2);
	}
	return value;
}

static int compare_u64(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static void append_latency(struct result_set *results, uint64_t latency_ns)
{
	uint64_t *new_values;
	size_t new_capacity;

	if (results->count == results->capacity) {
		new_capacity = results->capacity ? results->capacity * 2 : 4096;
		new_values = realloc(results->latency_ns,
				     new_capacity * sizeof(*new_values));
		if (!new_values) {
			perror("realloc");
			exit(1);
		}
		results->latency_ns = new_values;
		results->capacity = new_capacity;
	}
	results->latency_ns[results->count++] = latency_ns;
}

static struct request_slot *find_free_slot(struct request_slot *slots,
					    size_t window)
{
	size_t index;

	for (index = 0; index < window; index++)
		if (!slots[index].active)
			return &slots[index];
	return NULL;
}

static struct request_slot *find_value(struct request_slot *slots,
				       size_t window, uint32_t value)
{
	size_t index;

	for (index = 0; index < window; index++)
		if (slots[index].active && slots[index].value == value)
			return &slots[index];
	return NULL;
}

static int percentile_index(size_t count, unsigned int percentile)
{
	uint64_t rank;

	if (!count)
		return 0;
	rank = ((uint64_t)count * percentile + 99U) / 100U;
	if (!rank)
		rank = 1;
	return (int)(rank - 1U);
}

int main(int argc, char **argv)
{
	struct mailmsg_user_frame frame = { 0 };
	struct request_slot *slots;
	struct request_slot *slot;
	struct result_set results = { 0 };
	struct pollfd pollfd;
	char path[32];
	unsigned long priority, window, duration_seconds, base_value;
	uint64_t start_ns, deadline_ns, drain_deadline_ns, now_ns;
	uint64_t attempts = 0, write_ok = 0, enospc = 0, eagain = 0;
	uint64_t write_other = 0, ack = 0, nack = 0, pong = 0;
	uint64_t duplicate = 0, unknown = 0, read_error = 0;
	uint64_t outstanding = 0, max_inflight = 0, timeouts = 0;
	uint32_t next_value, pong_value;
	int fd, poll_ret;
	ssize_t io_ret;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s <priority 0..3> <window 1..4096> "
			"<duration-seconds 1..3600> <base-value>\n", argv[0]);
		return 2;
	}
	priority = parse_ulong(argv[1], "priority", MAILMSG_PRIORITY_COUNT - 1U);
	window = parse_ulong(argv[2], "window", 4096);
	duration_seconds = parse_ulong(argv[3], "duration", 3600);
	base_value = parse_ulong(argv[4], "base value", UINT32_MAX - 1000000U);
	if (!window || !duration_seconds) {
		fprintf(stderr, "window and duration must be non-zero\n");
		return 2;
	}

	slots = calloc(window, sizeof(*slots));
	if (!slots) {
		perror("calloc");
		return 1;
	}
	snprintf(path, sizeof(path), "/dev/mailmsg-p%lu", priority);
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		free(slots);
		return 1;
	}
	pollfd.fd = fd;
	pollfd.events = POLLIN;
	pollfd.revents = 0;
	/* poll() claims this priority's sole reader before any request is sent. */
	poll_ret = poll(&pollfd, 1, 0);
	if (poll_ret < 0 || (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
		if (poll_ret < 0)
			perror("initial poll");
		else
			fprintf(stderr, "initial poll revents=%#x\n", pollfd.revents);
		close(fd);
		free(slots);
		return 1;
	}

	next_value = (uint32_t)base_value;
	start_ns = monotonic_ns();
	deadline_ns = start_ns + (uint64_t)duration_seconds * 1000000000ULL;

	for (;;) {
		now_ns = monotonic_ns();
		if (now_ns < deadline_ns) {
			while (outstanding < window) {
				slot = find_free_slot(slots, window);
				if (!slot)
					break;
				memset(&frame, 0, sizeof(frame));
				frame.priority = (mailmsg_u32)priority;
				frame.type = MAILMSG_MSG_PING;
				frame.length = sizeof(mailmsg_u32);
				store_u32(frame.payload, next_value);
				attempts++;
				io_ret = write(fd, &frame, sizeof(frame));
				if (io_ret == (ssize_t)sizeof(frame)) {
					slot->value = next_value++;
					slot->start_ns = monotonic_ns();
					slot->active = 1;
					write_ok++;
					outstanding++;
					if (outstanding > max_inflight)
						max_inflight = outstanding;
					continue;
				}
				if (io_ret < 0 && errno == ENOSPC) {
					enospc++;
					break;
				}
				if (io_ret < 0 && errno == EAGAIN) {
					eagain++;
					break;
				}
				write_other++;
				if (io_ret >= 0)
					fprintf(stderr, "short write: %zd\n", io_ret);
				else
					perror("mailmsg write");
				goto drain;
			}
		} else {
			/* The measurement window is over.  Stop producing new work,
			 * then drain responses once before applying the bounded drain
			 * timeout below.  Without this branch an incomplete response
			 * could leave the producer polling forever after the deadline.
			 */
			if (!outstanding)
				break;
			goto drain;
		}

		pollfd.events = POLLIN;
		pollfd.revents = 0;
		poll_ret = poll(&pollfd, 1, 10);
		if (poll_ret < 0) {
			if (errno == EINTR)
				continue;
			perror("mailmsg poll");
			read_error++;
			goto drain;
		}
		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "mailmsg poll revents=%#x\n", pollfd.revents);
			read_error++;
			goto drain;
		}
		if (!(pollfd.revents & POLLIN))
			continue;

		for (;;) {
			memset(&frame, 0, sizeof(frame));
			io_ret = read(fd, &frame, sizeof(frame));
			if (io_ret < 0 && errno == EAGAIN)
				break;
			if (io_ret != (ssize_t)sizeof(frame)) {
				if (io_ret < 0)
					perror("mailmsg read");
				else
					fprintf(stderr, "short read: %zd\n", io_ret);
				read_error++;
				goto drain;
			}
			if (frame.type == MAILMSG_MSG_ACK) {
				ack++;
				continue;
			}
			if (frame.type == MAILMSG_MSG_NACK) {
				nack++;
				continue;
			}
			if (frame.type != MAILMSG_MSG_PONG ||
			    frame.length < sizeof(mailmsg_u32)) {
				unknown++;
				continue;
			}
			pong_value = load_u32(frame.payload);
			if (!pong_value) {
				unknown++;
				continue;
			}
			slot = find_value(slots, window, pong_value - 1U);
			if (!slot) {
				duplicate++;
				continue;
			}
			append_latency(&results, monotonic_ns() - slot->start_ns);
			slot->active = 0;
			outstanding--;
			pong++;
		}
	}

drain:
	drain_deadline_ns = monotonic_ns() + 5000000000ULL;
	while (outstanding && monotonic_ns() < drain_deadline_ns) {
		pollfd.events = POLLIN;
		pollfd.revents = 0;
		poll_ret = poll(&pollfd, 1, 20);
		if (poll_ret <= 0)
			continue;
		if (!(pollfd.revents & POLLIN))
			break;
		for (;;) {
			memset(&frame, 0, sizeof(frame));
			io_ret = read(fd, &frame, sizeof(frame));
			if (io_ret < 0 && errno == EAGAIN)
				break;
			if (io_ret != (ssize_t)sizeof(frame)) {
				read_error++;
				break;
			}
			if (frame.type == MAILMSG_MSG_ACK) {
				ack++;
				continue;
			}
			if (frame.type == MAILMSG_MSG_NACK) {
				nack++;
				continue;
			}
			if (frame.type != MAILMSG_MSG_PONG ||
			    frame.length < sizeof(mailmsg_u32)) {
				unknown++;
				continue;
			}
			pong_value = load_u32(frame.payload);
			if (!pong_value) {
				unknown++;
				continue;
			}
			slot = find_value(slots, window, pong_value - 1U);
			if (!slot) {
				duplicate++;
				continue;
			}
			append_latency(&results, monotonic_ns() - slot->start_ns);
			slot->active = 0;
			outstanding--;
			pong++;
		}
	}
	timeouts = outstanding;
	close(fd);

	qsort(results.latency_ns, results.count, sizeof(*results.latency_ns),
	      compare_u64);
	printf("priority=%lu window=%lu duration_s=%lu attempts=%llu "
	       "write_ok=%llu enospc=%llu eagain=%llu write_other=%llu "
	       "pong=%llu ack=%llu nack=%llu duplicate=%llu unknown=%llu "
	       "timeout=%llu lost=%llu max_inflight=%llu throughput_rps=%.3f\n",
	       priority, window, duration_seconds,
	       (unsigned long long)attempts, (unsigned long long)write_ok,
	       (unsigned long long)enospc, (unsigned long long)eagain,
	       (unsigned long long)write_other, (unsigned long long)pong,
	       (unsigned long long)ack, (unsigned long long)nack,
	       (unsigned long long)duplicate, (unsigned long long)unknown,
	       (unsigned long long)timeouts,
	       (unsigned long long)(write_ok > pong ? write_ok - pong : 0),
	       (unsigned long long)max_inflight,
	       (double)pong / (double)duration_seconds);
	if (results.count) {
		printf("latency_us count=%zu min=%.3f p50=%.3f p95=%.3f "
		       "p99=%.3f max=%.3f\n",
		       results.count, results.latency_ns[0] / 1000.0,
		       results.latency_ns[percentile_index(results.count, 50)] / 1000.0,
		       results.latency_ns[percentile_index(results.count, 95)] / 1000.0,
		       results.latency_ns[percentile_index(results.count, 99)] / 1000.0,
		       results.latency_ns[results.count - 1] / 1000.0);
	}

	free(results.latency_ns);
	free(slots);
	if (eagain || write_other || read_error || nack || duplicate || unknown)
		return 1;
	if (mailmsg_priority_is_reliable((mailmsg_u32)priority)) {
		if (timeouts || ack != write_ok || pong != write_ok)
			return 1;
	}
	/* p2/p3 are intentionally unreliable: a saturated response ring may
	 * lose PONGs, which is reported as lost/timeout for the caller to decide. */
	return 0;
}
