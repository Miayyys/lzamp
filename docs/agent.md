# Agent and Peripheral Tool API

`agent/mailmsg_agent.py` restricts model output to one registered tool call, validates its arguments,
and then asks Zephyr to execute it through `/dev/mailmsg-p1`. The model is a decision source, not a
trusted control plane. Unknown tools, extra fields, malformed values, and out-of-range arguments are
rejected before hardware access.

## Invocation

Use an explicit decision for tests and programmatic callers:

```sh
python3 agent/mailmsg_agent.py \
  --decision '{"name":"zephyr_gpio_read","arguments":{"line":25}}' \
  --device-root /dev --timeout-ms 2000
```

Natural-language mode connects to a local RKLLM service compatible with OpenAI Chat Completions:

```sh
python3 agent/mailmsg_agent.py \
  --server http://127.0.0.1:8080 \
  --prompt 'Read the raw MPU6500 acceleration and gyroscope values. Call one tool.' \
  --native-tools --api-timeout 120 --timeout-ms 2000
```

The program accepts exactly one JSON decision or one exact `<tool_call>` block. It does not extract
and execute commands from arbitrary prose. `--validate-only` validates without accessing devices;
`--chat` returns model text without executing tools.

## Registered Tools

| Tool | Main arguments | Current boundary |
| --- | --- | --- |
| `zephyr_gpio_config` | `line`, `direction`, `value` | GPIO3 lines 25/28/29 |
| `zephyr_gpio_write` | `line`, `value` | Assigned line; value 0 or 1 |
| `zephyr_gpio_read` | `line` | Assigned line |
| `zephyr_uart_write` | `port`, `data` | UART5/7; at most 8 UTF-8 bytes |
| `zephyr_uart_read` | `port`, `length` | UART5/7; at most 8 B |
| `zephyr_i2c_write` | `address`, `data_hex` | I2C7; 7-bit address; at most 8 B |
| `zephyr_i2c_read` | `address`, `length` | I2C7; at most 8 B |
| `zephyr_i2c_write_read` | `address`, `prefix_hex`, `length` | 1–3 B prefix; read at most 8 B |
| `zephyr_imu_read` | none | Fixed IMU at address `0x68` |
| `zephyr_spi_transfer` | `chip_select`, `mode`, `data_hex` | SPI0; full-duplex, at most 8 B |
| `zephyr_pwm_set` | `period_ns`, `duty_ns`, `polarity` | PWM7; duty must not exceed period |
| `zephyr_pwm_stop` | none | Stop PWM7 |
| `zephyr_adc_read` | `channel` | Schema permits 0–7; current header declares ADC3/4 |

The authoritative board-level allowlist remains
[`peripheral-ownership.yaml`](../config/peripheral-ownership.yaml). Schema validation reduces software
mistakes but does not replace voltage, wiring, device-address, or mechanical-safety checks.

## Results and Failures

A successful response includes the canonical tool name, validated arguments, business result,
MailMsg priority/window, and ACK/RESULT sequences. Failures use `ok:false` with stable error classes,
including model API failure, unregistered tool, invalid arguments, device-open failure, timeout, and
full queue. The caller owns retry policy; the Agent does not automatically replay side effects.

This is a single-tool routing example. It does not yet provide identity-based authorization,
persistent audit logs, workflow orchestration, or a production API. External deployment should use a
dedicated unprivileged account, device-node permissions, request authentication, and an explicit tool
policy.
