# Validation Status and Reproduction Boundaries

This page distinguishes paths observed to work on the current board from production guarantees. It
is not a certification report, and another board using the same SoC must not reuse this DT or wiring
without review.

## Exercised Paths

The representative environment uses Linux `6.12.69-lzamp+` with seven Linux CPUs, Zephyr on MPIDR
`0x300`, and MailMsg V7:

- CPU3 start, SESSION_READY, controlled stop, rearm, and reader wakeup after unexpected offline.
- Bidirectional requests on all four priorities; ACK/NACK on priorities 0/1, CRC rejection, and full-ring errors.
- Event-driven worker wakeup and ring draining through mailbox notifications.
- RKNPU 0.9.8, RKLLM inference, and natural-language Agent calls into Zephyr.
- Six bidirectional loopback combinations across GPIO3 lines 25, 28, and 29.
- TX/RX loopback on UART5 and UART7.
- PWM7 LED blinking and visible duty-cycle changes.
- ADC3 light-sensor readings that changed with illumination.
- I2C7 reads from address `0x68`: WHO_AM_I `0x70`, acceleration, and gyroscope data.

Host-side `make test` covers protocol frames and rings, endpoints, notification abstraction, mailbox
mapping, peripheral logic, and Agent argument validation. `make publication-check` checks publication
metadata and the kernel gitlink. Neither command replaces target-board testing.

## Known Boundaries

- Each direction and priority uses an eight-slot SPSC ring with seven usable frames. Concurrent window
  results depend on ACK/RESULT amplification, consumption speed, and benchmark behavior; they are not
  a fixed throughput limit.
- Existing results are short functional and limited-concurrency tests, not endurance, worst-case
  latency, or hard real-time evidence.
- Priorities 0/1 provide ACK/NACK but no automatic retransmission, idempotency key, or exactly-once semantics.
- The 28 MiB bulk-data area has no implemented, frozen data-plane ABI.
- SPI0 has host software coverage but no external-device physical test.
- ADC reference voltage, linearity, sample rate, and long-term noise remain uncalibrated.
- Wi-Fi, Bluetooth, display, and audio are outside MailMsg peripheral acceptance scope.

## Recommended Reproduction Order

1. Run `make test` and `make publication-check`.
2. Build matching Linux, DTB, and Zephyr artifacts.
3. Verify seven-core Linux and reserved memory before starting Zephyr.
4. Exercise PING, all priorities, full-ring behavior, controlled stop, and rearm.
5. Connect peripherals one at a time using the voltage and pinmux ownership list.
6. Finally run RKLLM and peripheral calls together while recording kernel logs and error counters.

Target-side scripts live under [`tests/board/`](../tests/board/). Some require root, matching firmware,
and physical wiring. Read their arguments first; defaults are not safe assumptions for arbitrary boards.
