#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Continuously sample an exposed SARADC channel through MailMsg.

This is a low-rate validation tool.  It deliberately sends one direct,
allowlisted ``zephyr_adc_read`` request at a time instead of asking RKLLM to
choose a tool for every sample.  The latter would measure model/process
overhead rather than ADC repeatability.
"""

import argparse
import importlib.util
import json
import os
import sys
import time


EXPOSED_CHANNELS = (3, 4)
DEFAULT_INTERVAL_MS = 500.0
DEFAULT_TIMEOUT_MS = 2000


def load_agent(path):
    """Load the board's versioned MailMsg agent from an explicit path."""
    spec = importlib.util.spec_from_file_location("lzamp_mailmsg_agent", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load MailMsg agent: %s" % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    required = ("AgentError", "TOOL_ADC_READ", "execute_zephyr_peripheral")
    missing = [name for name in required if not hasattr(module, name)]
    if missing:
        raise RuntimeError("MailMsg agent is missing: %s" % ", ".join(missing))
    return module


def default_agent_path():
    configured = os.environ.get("MAILMSG_AGENT")
    if configured:
        return configured
    candidates = (
        "/userdata/lzamp/agent/mailmsg_agent-v7.py",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../agent/mailmsg_agent.py"),
    )
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return candidates[-1]


def emit(record):
    print(json.dumps(record, ensure_ascii=False, separators=(",", ":")), flush=True)


def sample_once(agent, channel, device_root, timeout_ms, index):
    started = time.monotonic_ns()
    wall_ns = time.time_ns()
    try:
        result = agent.execute_zephyr_peripheral(
            agent.TOOL_ADC_READ,
            {"channel": channel},
            device_root=device_root,
            timeout_ms=timeout_ms,
        )
    except agent.AgentError as exc:
        finished = time.monotonic_ns()
        return {
            "sample": index,
            "timestamp_ns": wall_ns,
            "channel": channel,
            "ok": False,
            "latency_us": (finished - started) / 1000.0,
            "error": exc.as_dict(),
        }, False

    finished = time.monotonic_ns()
    value = result.get("result", {}).get("value")
    if not isinstance(value, int) or not 0 <= value <= 0xFFF:
        return {
            "sample": index,
            "timestamp_ns": wall_ns,
            "channel": channel,
            "ok": False,
            "latency_us": (finished - started) / 1000.0,
            "error": {
                "code": "invalid_sample",
                "message": "Zephyr returned a value outside the 12-bit ADC range",
                "details": {"value": value},
            },
        }, False

    return {
        "sample": index,
        "timestamp_ns": wall_ns,
        "channel": channel,
        "ok": True,
        "value": value,
        "latency_us": (finished - started) / 1000.0,
        "transport": result.get("transport", {}),
    }, True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--agent",
        default=default_agent_path(),
        help="versioned MailMsg agent Python file (default: %(default)s)",
    )
    parser.add_argument(
        "--channel",
        type=int,
        choices=EXPOSED_CHANNELS,
        default=3,
        help="exposed R1 ADC channel: 3=CON3 pin24, 4=CON3 pin26",
    )
    parser.add_argument(
        "--interval-ms",
        type=float,
        default=DEFAULT_INTERVAL_MS,
        help="minimum interval between samples (default: %(default)s)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=0,
        help="number of samples; 0 means until Ctrl-C (default: 0)",
    )
    parser.add_argument("--timeout-ms", type=int, default=DEFAULT_TIMEOUT_MS)
    parser.add_argument("--device-root", default="/dev")
    parser.add_argument(
        "--max-errors",
        type=int,
        default=3,
        help="stop after this many errors; 0 means unlimited (default: %(default)s)",
    )
    args = parser.parse_args(argv)

    if args.interval_ms < 50.0:
        parser.error("--interval-ms must be at least 50 ms for the reset-per-read RPC")
    if args.count < 0:
        parser.error("--count must be non-negative")
    if args.timeout_ms <= 0:
        parser.error("--timeout-ms must be positive")
    if args.max_errors < 0:
        parser.error("--max-errors must be non-negative")
    if not os.path.isfile(args.agent):
        parser.error("MailMsg agent not found: %s" % args.agent)

    try:
        agent = load_agent(args.agent)
    except (OSError, RuntimeError, ImportError) as exc:
        print("adc-stream-test: %s" % exc, file=sys.stderr)
        return 2

    interval_ns = int(args.interval_ms * 1_000_000.0)
    successes = 0
    errors = 0
    next_deadline = time.monotonic_ns()
    index = 1
    interrupted = False
    try:
        while args.count == 0 or index <= args.count:
            now = time.monotonic_ns()
            if now < next_deadline:
                time.sleep((next_deadline - now) / 1_000_000_000.0)
            record, success = sample_once(
                agent, args.channel, args.device_root, args.timeout_ms, index
            )
            emit(record)
            if success:
                successes += 1
            else:
                errors += 1
                if args.max_errors and errors >= args.max_errors:
                    break
            index += 1
            next_deadline += interval_ns
            now = time.monotonic_ns()
            if next_deadline < now:
                next_deadline = now + interval_ns
    except KeyboardInterrupt:
        interrupted = True

    attempted = successes + errors
    print(
        "adc-stream-summary samples=%d success=%d errors=%d channel=%d interval_ms=%.3f%s"
        % (
            attempted,
            successes,
            errors,
            args.channel,
            args.interval_ms,
            " interrupted" if interrupted else "",
        ),
        file=sys.stderr,
    )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
