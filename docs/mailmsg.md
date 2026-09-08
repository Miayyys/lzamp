# MailMsg V7 Protocol

MailMsg is LZAMP's shared-memory control protocol. Frames reside in shared memory; the notification
backend only tells the peer that a priority may contain new messages. The current backend uses
RK3588 mailbox0, but the ring protocol does not depend on mailbox hardware.

## Queue Model

Linux-to-CPU3 and CPU3-to-Linux each contain four independent SPSC rings. Every ring has eight
physical slots. One slot distinguishes full from empty, leaving seven usable frames.

| Priority | Class | Transport feedback |
| ---: | --- | --- |
| 0 | critical | ACK or NACK |
| 1 | control | ACK or NACK |
| 2 | normal | no automatic feedback |
| 3 | best-effort | no automatic feedback |

A reliable priority means that the receiver reports frame validation; it does not provide automatic
retransmission or confirm completion of the requested operation. A full ring returns an error
immediately without blocking or overwriting old frames. The caller chooses retry, downgrade, or drop.

## Shared Frame

| Field | Size | Meaning |
| --- | ---: | --- |
| `generation` | 4 B | Nonzero session generation created by Linux |
| `type` | 4 B | Message type |
| `sequence` | 4 B | Sender's outbound sequence |
| `length` | 4 B | Valid payload length |
| `payload` | 28 B | Inline data |
| `crc32` | 4 B | CRC-32/ISO-HDLC over generation through payload |
| `commit` | 4 B | Complete-frame publication marker, written last |

The producer writes the message and CRC, performs cache publication and barriers, writes `commit`
last, and then notifies the peer. The consumer performs acquire/invalidate operations and validates
commit, generation, length, and CRC before releasing the slot. CRC checks content integrity; commit
prevents a consumer from accepting a partially published frame.

The Linux userspace `mailmsg_user_frame` is a fixed 48-byte record with a 32-byte payload. It is not
the same structure as the shared frame. The kernel converts between them, and userspace must not
interpret physical shared memory directly.

## Sessions and Feedback

After initialization, CPU3 sends `SESSION_READY(generation, version)`. Frames from a different
generation are stale and are not delivered into the new session.

ACK/NACK payloads identify the original request sequence and status. PONG and peripheral RESULT use
new outbound sequences and carry the request sequence for correlation. One reliable request may
produce both ACK and RESULT; consequently, a seven-slot return ring can hold at most three complete
two-frame responses without concurrent consumption. This is not a throughput limit.

## Notification Semantics

The four mailbox0 channels map directly to priorities 0–3. CMD/DATA carry doorbell metadata, never
the business payload. Notification results are:

- `SENT`: a doorbell was submitted.
- `COALESCED`: a notification is already pending; the frame may still have entered the ring.
- `FAILED`: the notification backend failed and preserves the underlying error.

The receiver drains the indicated ring on wakeup and retains fallback scanning. Interrupt count must
not be interpreted as message count. Replacing mailbox with SGI or another backend may preserve the
protocol ABI only if it preserves priority mapping, cache visibility, and publication order.

## Peripheral RPC and Bulk Data

V7 defines PING/PONG, ACK/NACK, STOP, SESSION_READY, GPIO compatibility messages, and generic
`PERIPHERAL_REQUEST`/`PERIPHERAL_RESULT`. Generic payloads consist of five little-endian u32 fields
and up to eight inline data bytes.

Peripheral RPC uses priority 1. ACK confirms receipt of the request frame; RESULT status determines
the operation outcome. Zephyr currently performs the operation before publishing ACK and RESULT, so
a timed-out side-effecting request must not be retried blindly.

Sensor blocks, video, and other bulk data should not be fragmented into many control frames. Future
data-plane queues will place data in separate shared regions and send buffer identity, offset, length,
format, and ownership through MailMsg. That ABI is not yet frozen.

Implementation references: [`mailmsg.h`](../mailmsg/mailmsg.h),
[`mailmsg_endpoint.h`](../mailmsg/mailmsg_endpoint.h), and
[`mailmsg_notify.h`](../mailmsg/mailmsg_notify.h).
