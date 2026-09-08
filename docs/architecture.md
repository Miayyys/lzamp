# LZAMP System Architecture

This document defines the current LZAMP system boundaries and component relationships. See the
MailMsg protocol document for wire-format details. Do not derive board wiring from this document alone.

## Purpose

LZAMP runs Linux and Zephyr concurrently on one RK3588S:

- Linux provides general applications, networking, storage, NPU inference, and the natural-language Agent.
- Zephyr exclusively owns one Cortex-A55 core and performs constrained real-time peripheral operations.
- MailMsg carries control messages between the two systems.
- Shared regions are reserved for future sensor streams and bulk data, but are not yet a stable data-plane ABI.

This is asymmetric multiprocessing, not a Linux thread or two complete virtual machines. Linux and
Zephyr share physical memory while retaining independent execution environments.

```text
Natural-language request
          |
          v
Linux Agent -- allowlisted tool --> /dev/mailmsg-p0..p3
     |                                      |
     | NPU inference                        | shared-memory frames
     v                                      v
   RKLLM                           bidirectional MailMsg rings
                                              |
                                   mailbox0 notifications only
                                              |
                                              v
                                    Zephyr / A55 CPU3
                                              |
                                GPIO, UART, I2C, PWM, ADC
```

## CPU Ownership and Lifecycle

RK3588S has eight application cores. The current device tree removes physical A55 CPU3 (MPIDR
`0x300`) from the Linux topology, so Linux reports seven CPUs and Zephyr exclusively owns CPU3.

Linux starts CPU3 through PSCI `CPU_ON`. Zephyr initializes itself, binds the current MailMsg session,
and sends `SESSION_READY`; only then does Linux enter `active`. Seeing CPU3 in the `on` state alone
does not prove that MailMsg is ready.

```text
Linux STOP_REQUEST
        |
        v
Zephyr checks state -- reject --> STOP_REFUSED
        | accept
        v
send STOP_READY -> stop peripheral services -> PSCI CPU_OFF
        |
        v
Linux observes CPU3=off and marks the session offline
        |
        v
rearm clears local state before another image load and start
```

Start and stop timeouts are explicit terminal states. A late response must not silently reactivate a
session, and Linux must never overwrite the image while CPU3 is still running.

## 32 MiB Reserved Memory

The Linux device tree marks physical addresses `0x50000000`–`0x51ffffff` as a 32 MiB `no-map`
region so that the normal Linux page allocator cannot use it.

| Address range | Size | Current purpose |
| --- | ---: | --- |
| `0x50000000`–`0x503fefff` | 4092 KiB | Zephyr link and runtime region |
| `0x503ff000`–`0x503fffff` | 4 KiB | MailMsg control page, rings, and observations |
| `0x50400000`–`0x504fffff` | 1 MiB | Reserved priority 0 data region |
| `0x50500000`–`0x505fffff` | 1 MiB | Reserved priority 1 data region |
| `0x50600000`–`0x509fffff` | 4 MiB | Reserved priority 2 data region |
| `0x50a00000`–`0x519fffff` | 16 MiB | Reserved priority 3 data region |
| `0x51a00000`–`0x51ffffff` | 6 MiB | Reserved for future expansion |

Reserved means unavailable to the normal Linux allocator; it does not mean that the corresponding
data queues are implemented. The only stable data plane today is the small-message MailMsg rings in
the control page. Future bulk data should live in separate shared buffers, with MailMsg carrying type,
location, length, and ownership metadata.

The uploaded Zephyr image is currently padded to 128 KiB. This is not the Zephyr memory limit: the
linkable region is `0x3ff000` bytes. Upload size, link limit, and the complete carveout are distinct.

## MailMsg and Notifications

MailMsg payloads reside in shared memory. Four Rockchip `mailbox0` channels notify priorities 0–3;
mailbox CMD/DATA registers do not carry the message body. A send has two independent outcomes:

1. Whether the frame was committed to its shared-memory ring.
2. Whether a notification was sent or coalesced behind an already-pending notification.

Notification coalescing does not imply message loss, and successful notification does not imply
business-level completion. A consumer drains the relevant ring after wakeup and performs fallback
scans for coalesced or lost wakeups. Memory barriers, cache maintenance, CRC, and the final `commit`
write are all part of protocol correctness.

The notification backend is abstracted from ring logic. It may later be replaced by SGI or another
doorbell while preserving publication order, priority mapping, and shared-memory visibility.

## Peripheral Ownership

Linux and Zephyr must not operate the same peripheral without an explicit sharing protocol. LZAMP
uses two ownership modes:

- Controller-exclusive: one side owns the complete UART, I2C, PWM bank, or SARADC controller.
- Line-exclusive, controller-shared: Linux retains GPIO3 bank ownership while Zephyr may operate only
  explicitly assigned lines and may not alter bank-wide clocks, reset, or interrupts.

CRU remains a shared system controller. Zephyr may touch only assigned leaf clock and reset bits using
Rockchip hiword-mask writes; shared PLLs and unmasked register read-modify-write are forbidden.

The default `i2c7-gpio` and alternative `spi0` profiles reuse header pins and are mutually exclusive
at boot. Switching profiles requires matching Linux DT and Zephyr builds. The authoritative resource
list is [`config/peripheral-ownership.yaml`](../config/peripheral-ownership.yaml).

A DT reservation does not prove driver operation, electrical correctness, or physical-device support:

- GPIO, UART, and I2C header pins use 3.3 V.
- The current PWM7 and SARADC header pins are treated as 1.8 V boundaries.
- GPIO line numbers are not physical header pin numbers.

## Agent Safety Boundary

The language model proposes a tool decision; it cannot execute a shell or access arbitrary hardware
addresses. The Linux Agent validates an allowlisted tool name, parameter ranges, and data lengths
before sending a priority 1 peripheral RPC. Unknown tools, extra fields, and out-of-range values are
rejected.

Model API availability does not prove MailMsg or peripheral availability. A MailMsg ACK confirms only
transport-level frame acceptance; the independent RESULT status determines peripheral-operation
success. Side-effecting operations must not be retried blindly after a timeout because the remote side
may already have executed them.

## Current Scope

The current board has exercised seven-core Linux, CPU3 lifecycle management, four MailMsg priorities,
NPU/RKLLM integration, and representative GPIO, UART, PWM, ADC, and I2C hardware paths. SPI has only
host-side software coverage. The bulk shared-data area remains an address plan, not an implemented
video or sensor-stream transport.

These results demonstrate representative paths on the current hardware/software combination. They do
not establish hard real-time behavior, long-term stability, throughput guarantees, or product safety.
Another RK3588/RK3588S board requires a fresh review of memory, pinmux, clocks, reset, power, and
existing Linux consumers.
