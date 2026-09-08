# Memory and Peripheral Ownership

LZAMP runs two operating systems concurrently. Every resource needs an explicit owner before either
side enables its driver. A DT node marked `reserved` or `disabled` expresses isolation intent; it does
not prove that a Zephyr driver works.

## Memory Ownership

Linux reserves 32 MiB beginning at `0x50000000` as `no-map`. The first 4 MiB contains the Zephyr
image, runtime, and MailMsg control page. The remaining 28 MiB is reserved for a data plane whose ABI
has not been frozen. Exact addresses are listed in [System Architecture](architecture.md#32-mib-reserved-memory)
and [`mailmsg_memory_layout.h`](../mailmsg/mailmsg_memory_layout.h).

Physical addresses, the control-page location, and the Zephyr link limit form a joint Linux/Zephyr
ABI. Any change must update the DT, Zephyr link and MMU configuration, drivers, and tests together.

## Controller Rules

- `controller-exclusive`: UART5, UART7, I2C7, SPI0, the PWM1 bank, and SARADC belong to one side.
  Linux must disable the corresponding node and consumers when ownership moves to Zephyr.
- `line-exclusive-controller-shared`: Linux retains the GPIO3 controller, while Zephyr may touch only
  assigned lines. Zephyr must not change bank-wide clock, reset, IRQ, or unmasked registers.
- CRU remains shared. Zephyr may operate assigned leaf gates and resets with Rockchip hiword-mask
  writes; shared PLLs do not belong to Zephyr.

## Current Profile

The default `i2c7-gpio` profile enables I2C7 and GPIO3 lines 25, 28, and 29. The alternative `spi0`
profile reuses the same header pin group, so the two profiles are mutually exclusive at boot. Linux
DT and Zephyr must select the same profile.

[`peripheral-ownership.yaml`](../config/peripheral-ownership.yaml) is the authoritative machine-readable
list of interfaces, voltages, pins, and validation labels. Physical header pins, GPIO lines, and
peripheral channels are separate numbering domains. PWM7 and SARADC3/4 are treated as 1.8 V
boundaries; use appropriate level translation before connecting a 3.3 V signal.

Porting to another board requires a new review of its schematic, pinmux, voltage domains, clock
parents, reset lines, Linux consumers, reserved-memory conflicts, and header voltages. A shared SoC
does not make board-level resource assignments portable.
