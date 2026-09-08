#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Allowlisted RKLLM-to-MailMsg peripheral tool runner.

Every hardware operation is bounded to one MailMsg V7 frame. Bulk sensor data
belongs in the separate shared-buffer transport.
"""

import argparse
import errno
import json
import os
import re
import select
import struct
import sys
import time
import urllib.error
import urllib.request


MAILMSG_PRIORITY_CONTROL = 1
MAILMSG_MSG_PING = 1
MAILMSG_MSG_PONG = 2
MAILMSG_MSG_ACK = 3
MAILMSG_MSG_NACK = 4
MAILMSG_MSG_GPIO_READ_REQUEST = 9
MAILMSG_MSG_GPIO_READ_RESULT = 10
MAILMSG_MSG_PERIPHERAL_REQUEST = 11
MAILMSG_MSG_PERIPHERAL_RESULT = 12
MAILMSG_FRAME = struct.Struct("<IIII32s")
MAILMSG_PAYLOAD_LIMIT = 28
MAILMSG_PERIPHERAL_PAYLOAD = struct.Struct("<IIIII8s")

PERIPH_GPIO_CONFIG = 1
PERIPH_GPIO_WRITE = 2
PERIPH_GPIO_READ = 3
PERIPH_UART_WRITE = 4
PERIPH_UART_READ = 5
PERIPH_I2C_WRITE = 6
PERIPH_I2C_READ = 7
PERIPH_I2C_WRITE_READ = 8
PERIPH_SPI_TRANSFER = 9
PERIPH_PWM_SET = 10
PERIPH_PWM_STOP = 11
PERIPH_ADC_READ = 12

TOOL_INCREMENT = "zephyr_increment"
TOOL_GPIO_READ = "zephyr_gpio_read"
TOOL_GPIO_CONFIG = "zephyr_gpio_config"
TOOL_GPIO_WRITE = "zephyr_gpio_write"
TOOL_UART_WRITE = "zephyr_uart_write"
TOOL_UART_READ = "zephyr_uart_read"
TOOL_I2C_WRITE = "zephyr_i2c_write"
TOOL_I2C_READ = "zephyr_i2c_read"
TOOL_I2C_WRITE_READ = "zephyr_i2c_write_read"
TOOL_SPI_TRANSFER = "zephyr_spi_transfer"
TOOL_PWM_SET = "zephyr_pwm_set"
TOOL_PWM_STOP = "zephyr_pwm_stop"
TOOL_ADC_READ = "zephyr_adc_read"
TOOL_IMU_READ = "zephyr_imu_read"
GPIO_READ_LINES = (25, 28, 29)
UART_PORTS = (5, 7)

# This is a deliberately small semantic binding, not a general I2C scanner.
# It is backed by the physical MPU6500-class module verified on I2C7 at 0x68.
IMU_I2C_ADDRESS = 0x68

# Model output is untrusted.  Only spellings observed from the local model are
# normalized; unknown names still fail the allowlist check below.
TOOL_NAME_ALIASES = {
    "zephyr_ii_c_write_read": TOOL_I2C_WRITE_READ,
    "zephyr_iic_write_read": TOOL_I2C_WRITE_READ,
    "zephyr_imu_reading": TOOL_IMU_READ,
}

# The local RKLLM native template sometimes emits this lone closing tag for an
# empty-argument function. It is accepted only for this read-only semantic
# tool, never as a general malformed-XML recovery rule.
EMPTY_NATIVE_PARAMETER_TOLERANCE = {TOOL_IMU_READ}
TOOL_INCREMENT_SCHEMA = {
    "type": "function",
    "function": {
        "name": TOOL_INCREMENT,
        "description": "Ask the Zephyr CPU3 test service to add one to an unsigned 32-bit integer.",
        "parameters": {
            "type": "object",
            "properties": {
                "value": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 4294967294,
                }
            },
            "required": ["value"],
            "additionalProperties": False,
        },
    },
}
TOOL_GPIO_READ_SCHEMA = {
    "type": "function",
    "function": {
        "name": TOOL_GPIO_READ,
        "description": "Read one Zephyr-owned RK3588 GPIO3 input line.",
        "parameters": {
            "type": "object",
            "properties": {
                "line": {
                    "type": "integer",
                    "enum": list(GPIO_READ_LINES),
                }
            },
            "required": ["line"],
            "additionalProperties": False,
        },
    },
}
def _integer_schema(minimum, maximum):
    return {"type": "integer", "minimum": minimum, "maximum": maximum}


def _tool_schema(name, description, properties, required):
    return {"type": "function", "function": {"name": name,
        "description": description, "parameters": {"type": "object",
        "properties": properties, "required": required,
        "additionalProperties": False}}}


TOOL_SCHEMAS = [
    TOOL_INCREMENT_SCHEMA, TOOL_GPIO_READ_SCHEMA,
    _tool_schema(TOOL_GPIO_CONFIG, "Configure an owned GPIO3 line as input or output.",
        {"line": {"type": "integer", "enum": list(GPIO_READ_LINES)},
         "direction": {"type": "string", "enum": ["input", "output"]},
         "value": _integer_schema(0, 1)}, ["line", "direction", "value"]),
    _tool_schema(TOOL_GPIO_WRITE, "Write an owned GPIO3 output line.",
        {"line": {"type": "integer", "enum": list(GPIO_READ_LINES)},
         "value": _integer_schema(0, 1)}, ["line", "value"]),
    _tool_schema(TOOL_UART_WRITE, "Write at most eight UTF-8 bytes to UART5 or UART7.",
        {"port": {"type": "integer", "enum": list(UART_PORTS)},
         "data": {"type": "string", "maxLength": 8}}, ["port", "data"]),
    _tool_schema(TOOL_UART_READ, "Read up to eight bytes from UART5 or UART7.",
        {"port": {"type": "integer", "enum": list(UART_PORTS)},
         "length": _integer_schema(1, 8)}, ["port", "length"]),
    _tool_schema(TOOL_I2C_WRITE, "Write up to eight hexadecimal bytes on I2C7.",
        {"address": _integer_schema(0, 127), "data_hex": {"type": "string", "maxLength": 16}},
        ["address", "data_hex"]),
    _tool_schema(TOOL_I2C_READ, "Read up to eight bytes from an I2C7 address.",
        {"address": _integer_schema(0, 127), "length": _integer_schema(1, 8)},
        ["address", "length"]),
    _tool_schema(TOOL_I2C_WRITE_READ, "Write a one-to-three-byte register prefix then read I2C7.",
        {"address": _integer_schema(0, 127), "prefix_hex": {"type": "string", "maxLength": 6},
         "length": _integer_schema(1, 8)}, ["address", "prefix_hex", "length"]),
    _tool_schema(TOOL_IMU_READ,
        "Read raw acceleration, temperature, and gyroscope data from the verified I2C7 MPU6500-class IMU.",
        {}, []),
    _tool_schema(TOOL_SPI_TRANSFER, "Full-duplex SPI0 transfer of up to eight hexadecimal bytes.",
        {"chip_select": _integer_schema(0, 1), "mode": _integer_schema(0, 3),
         "data_hex": {"type": "string", "maxLength": 16}},
        ["chip_select", "mode", "data_hex"]),
    _tool_schema(TOOL_PWM_SET, "Set PWM7 period, duty and polarity.",
        {"period_ns": _integer_schema(1, 4294967295),
         "duty_ns": _integer_schema(0, 4294967295), "polarity": _integer_schema(0, 1)},
        ["period_ns", "duty_ns", "polarity"]),
    _tool_schema(TOOL_PWM_STOP, "Stop PWM7 output.", {}, []),
    _tool_schema(TOOL_ADC_READ, "Read one RK3588 SARADC channel.",
        {"channel": _integer_schema(0, 7)}, ["channel"]),
]
ALLOWED_TOOLS = {item["function"]["name"] for item in TOOL_SCHEMAS}

NATIVE_TOOL_CALL_RE = re.compile(
    r"\A<tool_call>\s*"
    r"<function=(?P<name>[A-Za-z_][A-Za-z0-9_]*)>\s*"
    r"(?P<body>.*?)\s*</function>\s*</tool_call>\Z",
    re.DOTALL,
)
NATIVE_PARAMETER_RE = re.compile(
    r"\s*<parameter=(?P<name>[A-Za-z_][A-Za-z0-9_]*)>\s*"
    r"(?P<value>.*?)\s*</parameter>", re.DOTALL
)


class AgentError(Exception):
    """Expected validation, API, transport, or protocol failure."""

    def __init__(self, code, message, details=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details or {}

    def as_dict(self):
        result = {"ok": False, "error": self.code, "message": self.message}
        if self.details:
            result["details"] = self.details
        return result


def _strict_object(value, allowed, label):
    if not isinstance(value, dict):
        raise AgentError("invalid_decision", "%s must be an object" % label)
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise AgentError(
            "invalid_decision",
            "%s contains unsupported fields" % label,
            {"fields": unknown},
        )


def _canonical_tool_name(name):
    """Apply only explicit, non-privilege-expanding tool-name aliases."""
    if not isinstance(name, str):
        return name
    compact = re.sub(r"_+", "_", name.strip().lower())
    return TOOL_NAME_ALIASES.get(compact, compact)


def parse_decision(text):
    """Parse exact JSON or one exact RKLLM native tool-call block."""
    stripped = text.strip()
    native_match = NATIVE_TOOL_CALL_RE.fullmatch(stripped)
    if native_match:
        body = native_match.group("body")
        arguments = {}
        cursor = 0
        for parameter in NATIVE_PARAMETER_RE.finditer(body):
            if body[cursor:parameter.start()].strip():
                raise AgentError("invalid_json", "native tool call contains unsupported content")
            key = parameter.group("name")
            if key in arguments:
                raise AgentError("invalid_decision", "native tool call repeats a parameter")
            raw_value = parameter.group("value").strip()
            try:
                arguments[key] = json.loads(raw_value)
            except json.JSONDecodeError:
                arguments[key] = raw_value
            cursor = parameter.end()
        trailing = body[cursor:].strip()
        canonical_native_name = _canonical_tool_name(native_match.group("name"))
        if trailing == "</parameter>" and canonical_native_name in EMPTY_NATIVE_PARAMETER_TOLERANCE:
            trailing = ""
        if trailing:
            raise AgentError("invalid_json", "native tool call contains unsupported content")
        decision = {
            "name": native_match.group("name"),
            "arguments": arguments,
        }
    else:
        prefix = "<tool_call>"
        suffix = "</tool_call>"
        if stripped.startswith(prefix) and stripped.endswith(suffix):
            stripped = stripped[len(prefix):-len(suffix)].strip()
        try:
            decision = json.loads(stripped)
        except json.JSONDecodeError as exc:
            raise AgentError(
                "invalid_json",
                "model output is not one exact JSON tool call",
                {"line": exc.lineno, "column": exc.colno},
            ) from exc

    _strict_object(decision, ("name", "arguments"), "decision")
    name = _canonical_tool_name(decision.get("name"))
    if name not in ALLOWED_TOOLS:
        raise AgentError(
            "tool_not_allowed",
            "tool is not registered",
            {"name": decision.get("name")},
        )
    arguments = decision.get("arguments")
    if name == TOOL_INCREMENT:
        _strict_object(arguments, ("value",), "arguments")
        if "value" not in arguments:
            raise AgentError("invalid_arguments", "value is required")
        value = arguments["value"]
        if isinstance(value, bool) or not isinstance(value, int):
            raise AgentError("invalid_arguments", "value must be an integer")
        if value < 0 or value >= 0xFFFFFFFF:
            raise AgentError(
                "invalid_arguments",
                "value must be between 0 and 4294967294",
            )
        return {"name": name, "arguments": {"value": value}}

    schema = next(item["function"]["parameters"] for item in TOOL_SCHEMAS
                  if item["function"]["name"] == name)
    properties = schema["properties"]
    _strict_object(arguments, properties, "arguments")
    for field in schema["required"]:
        if field not in arguments:
            raise AgentError("invalid_arguments", "%s is required" % field)
    normalized = {}
    for field, rule in properties.items():
        if field not in arguments:
            continue
        value = arguments[field]
        if rule["type"] == "integer":
            if isinstance(value, bool) or not isinstance(value, int):
                raise AgentError("invalid_arguments", "%s must be an integer" % field)
            if "enum" in rule and value not in rule["enum"]:
                raise AgentError("invalid_arguments", "%s is outside the allowlist" % field)
            if value < rule.get("minimum", value) or value > rule.get("maximum", value):
                raise AgentError("invalid_arguments", "%s is outside the allowed range" % field)
        elif rule["type"] == "string":
            if not isinstance(value, str) or len(value) > rule.get("maxLength", len(value)):
                raise AgentError("invalid_arguments", "%s is not a valid string" % field)
            if "enum" in rule and value not in rule["enum"]:
                raise AgentError("invalid_arguments", "%s is outside the allowlist" % field)
        normalized[field] = value
    if name in (TOOL_I2C_WRITE, TOOL_SPI_TRANSFER):
        normalized["data_hex"] = _validate_hex(normalized["data_hex"], "data_hex", 1, 8)
    elif name == TOOL_I2C_WRITE_READ:
        normalized["prefix_hex"] = _validate_hex(normalized["prefix_hex"], "prefix_hex", 1, 3)
    elif name == TOOL_UART_WRITE:
        encoded = normalized["data"].encode("utf-8")
        if not encoded or len(encoded) > 8:
            raise AgentError("invalid_arguments", "data must encode to between one and eight bytes")
    elif name == TOOL_PWM_SET and normalized["duty_ns"] > normalized["period_ns"]:
        raise AgentError("invalid_arguments", "duty_ns must not exceed period_ns")
    return {"name": name, "arguments": normalized}


def _validate_hex(value, field, minimum_bytes, maximum_bytes):
    if len(value) % 2 or not re.fullmatch(r"[0-9a-fA-F]*", value):
        raise AgentError("invalid_arguments", "%s must contain complete hexadecimal bytes" % field)
    size = len(value) // 2
    if size < minimum_bytes or size > maximum_bytes:
        raise AgentError("invalid_arguments", "%s must contain %d to %d bytes" %
                         (field, minimum_bytes, maximum_bytes))
    return value.lower()


def pack_frame(priority, message_type, value):
    payload = struct.pack("<I", value) + bytes(28)
    return MAILMSG_FRAME.pack(priority, message_type, 0, 4, payload)


def pack_peripheral_frame(operation, arg0=0, arg1=0, arg2=0, data=b""):
    if len(data) > 8:
        raise AgentError("invalid_arguments", "peripheral inline data exceeds eight bytes")
    payload = MAILMSG_PERIPHERAL_PAYLOAD.pack(
        operation, arg0, arg1, arg2, len(data), data.ljust(8, b"\0")
    )
    return MAILMSG_FRAME.pack(MAILMSG_PRIORITY_CONTROL,
                              MAILMSG_MSG_PERIPHERAL_REQUEST, 0,
                              len(payload), payload + bytes(4))


def unpack_frame(data):
    if len(data) != MAILMSG_FRAME.size:
        raise AgentError(
            "short_frame",
            "MailMsg returned an unexpected record size",
            {"expected": MAILMSG_FRAME.size, "actual": len(data)},
        )
    priority, message_type, sequence, length, payload = MAILMSG_FRAME.unpack(data)
    if priority != MAILMSG_PRIORITY_CONTROL:
        raise AgentError(
            "wrong_priority",
            "received a frame from an unexpected priority",
            {"priority": priority},
        )
    if length > MAILMSG_PAYLOAD_LIMIT:
        raise AgentError(
            "invalid_frame",
            "received payload length exceeds the MailMsg V7 limit",
            {"length": length},
        )
    return {
        "priority": priority,
        "type": message_type,
        "sequence": sequence,
        "length": length,
        "payload": payload[:length],
    }


def _read_u32(payload, offset=0):
    if len(payload) < offset + 4:
        raise AgentError("invalid_frame", "MailMsg payload is too short")
    return struct.unpack_from("<I", payload, offset)[0]


def _peripheral_arguments(name, arguments):
    if name == TOOL_GPIO_CONFIG:
        return (PERIPH_GPIO_CONFIG, arguments["line"],
                int(arguments["direction"] == "output"), arguments["value"], b"")
    if name == TOOL_GPIO_WRITE:
        return PERIPH_GPIO_WRITE, arguments["line"], arguments["value"], 0, b""
    if name == TOOL_GPIO_READ:
        return PERIPH_GPIO_READ, arguments["line"], 0, 0, b""
    if name == TOOL_UART_WRITE:
        return PERIPH_UART_WRITE, arguments["port"], 0, 0, arguments["data"].encode("utf-8")
    if name == TOOL_UART_READ:
        return PERIPH_UART_READ, arguments["port"], arguments["length"], 0, b""
    if name == TOOL_I2C_WRITE:
        return PERIPH_I2C_WRITE, arguments["address"], 0, 0, bytes.fromhex(arguments["data_hex"])
    if name == TOOL_I2C_READ:
        return PERIPH_I2C_READ, arguments["address"], arguments["length"], 0, b""
    if name == TOOL_I2C_WRITE_READ:
        return (PERIPH_I2C_WRITE_READ, arguments["address"], arguments["length"], 0,
                bytes.fromhex(arguments["prefix_hex"]))
    if name == TOOL_SPI_TRANSFER:
        return (PERIPH_SPI_TRANSFER, arguments["chip_select"], arguments["mode"], 0,
                bytes.fromhex(arguments["data_hex"]))
    if name == TOOL_PWM_SET:
        return (PERIPH_PWM_SET, arguments["period_ns"], arguments["duty_ns"],
                arguments["polarity"], b"")
    if name == TOOL_PWM_STOP:
        return PERIPH_PWM_STOP, 0, 0, 0, b""
    if name == TOOL_ADC_READ:
        return PERIPH_ADC_READ, arguments["channel"], 0, 0, b""
    raise AgentError("tool_not_allowed", "tool is not a peripheral RPC", {"name": name})


def execute_zephyr_peripheral(name, arguments, device_root="/dev", timeout_ms=1000):
    """Execute one bounded, reliable MailMsg V7 peripheral operation."""
    operation, arg0, arg1, arg2, data = _peripheral_arguments(name, arguments)
    path = os.path.join(device_root, "mailmsg-p1")
    flags = os.O_RDWR | os.O_NONBLOCK
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise AgentError("mailmsg_open_failed", os.strerror(exc.errno),
                         {"path": path, "errno": exc.errno}) from exc
    try:
        try:
            written = os.write(fd, pack_peripheral_frame(operation, arg0, arg1, arg2, data))
        except OSError as exc:
            code = "queue_full" if exc.errno == errno.ENOSPC else "mailmsg_write_failed"
            raise AgentError(code, os.strerror(exc.errno), {"errno": exc.errno}) from exc
        if written != MAILMSG_FRAME.size:
            raise AgentError("short_write", "MailMsg accepted only part of the request",
                             {"expected": MAILMSG_FRAME.size, "actual": written})
        poller = select.poll()
        poller.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
        deadline = time.monotonic() + timeout_ms / 1000.0
        ack = result = None
        while ack is None or result is None:
            remaining_ms = max(0, int((deadline - time.monotonic()) * 1000))
            if remaining_ms == 0:
                raise AgentError("timeout", "timed out waiting for Zephyr ACK/result",
                                 {"ack_received": ack is not None,
                                  "result_received": result is not None})
            events = poller.poll(remaining_ms)
            if not events:
                continue
            event = events[0][1]
            if event & (select.POLLERR | select.POLLHUP) and not event & select.POLLIN:
                raise AgentError("mailmsg_offline", "MailMsg session went offline")
            try:
                frame = unpack_frame(os.read(fd, MAILMSG_FRAME.size))
            except BlockingIOError:
                continue
            if frame["type"] == MAILMSG_MSG_ACK:
                if frame["length"] < 8:
                    raise AgentError("invalid_ack", "ACK payload is too short")
                status = _read_u32(frame["payload"], 4)
                if status != 0:
                    raise AgentError("ack_failed", "Zephyr returned a nonzero ACK status",
                                     {"status": status})
                ack = {"sequence": frame["sequence"], "peer_sequence": _read_u32(frame["payload"])}
            elif frame["type"] == MAILMSG_MSG_NACK:
                reason = _read_u32(frame["payload"], 4) if frame["length"] >= 8 else None
                raise AgentError("nack", "Zephyr rejected the request", {"reason": reason})
            elif frame["type"] == MAILMSG_MSG_PERIPHERAL_RESULT:
                if frame["length"] != MAILMSG_PERIPHERAL_PAYLOAD.size:
                    raise AgentError("invalid_peripheral_result", "peripheral result must be 28 bytes")
                peer, status, result_op, value, length, result_data = \
                    MAILMSG_PERIPHERAL_PAYLOAD.unpack(frame["payload"])
                status = struct.unpack("<i", struct.pack("<I", status))[0]
                if result_op != operation or length > 8:
                    raise AgentError("invalid_peripheral_result", "operation or length mismatch")
                result = {"sequence": frame["sequence"], "peer_sequence": peer,
                          "status": status, "value": value,
                          "data_hex": result_data[:length].hex()}
            else:
                raise AgentError("unexpected_frame", "received an unexpected MailMsg type",
                                 {"type": frame["type"]})
        if ack["peer_sequence"] != result["peer_sequence"]:
            raise AgentError("sequence_mismatch", "ACK and result refer to different requests")
        if result["status"]:
            code, message = peripheral_status_error(result["status"])
            raise AgentError(code, message,
                             {"status": result["status"], "operation": operation})
        return {"ok": True, "tool": name, "arguments": arguments,
                "result": {"value": result["value"], "data_hex": result["data_hex"]},
                "transport": {"priority": MAILMSG_PRIORITY_CONTROL, "window": 1,
                    "ack_sequence": ack["sequence"],
                    "ack_peer_sequence": ack["peer_sequence"],
                    "result_sequence": result["sequence"]}}
    finally:
        os.close(fd)


def _signed16(data, offset):
    return struct.unpack_from(">h", data, offset)[0]


def execute_zephyr_imu_read(device_root="/dev", timeout_ms=1000):
    """Read the fixed MPU6500-class IMU through bounded I2C7 RPCs.

    The public semantic tool deliberately exposes neither bus addresses nor
    register prefixes.  Each underlying request still uses the normal p1
    one-in-flight transport and its existing validation.
    """
    accel_temp = execute_zephyr_peripheral(
        TOOL_I2C_WRITE_READ,
        {"address": IMU_I2C_ADDRESS, "prefix_hex": "3b", "length": 8},
        device_root, timeout_ms,
    )
    gyro = execute_zephyr_peripheral(
        TOOL_I2C_WRITE_READ,
        {"address": IMU_I2C_ADDRESS, "prefix_hex": "43", "length": 6},
        device_root, timeout_ms,
    )
    accel_bytes = bytes.fromhex(accel_temp["result"]["data_hex"])
    gyro_bytes = bytes.fromhex(gyro["result"]["data_hex"])
    if len(accel_bytes) != 8 or len(gyro_bytes) != 6:
        raise AgentError("invalid_imu_data", "IMU returned an unexpected data length")
    return {
        "ok": True,
        "tool": TOOL_IMU_READ,
        "arguments": {},
        "result": {
            "accelerometer_raw": {
                "x": _signed16(accel_bytes, 0), "y": _signed16(accel_bytes, 2),
                "z": _signed16(accel_bytes, 4),
            },
            "temperature_raw": _signed16(accel_bytes, 6),
            "gyroscope_raw": {
                "x": _signed16(gyro_bytes, 0), "y": _signed16(gyro_bytes, 2),
                "z": _signed16(gyro_bytes, 4),
            },
            "accel_temperature_hex": accel_bytes.hex(),
            "gyroscope_hex": gyro_bytes.hex(),
        },
        "transport": {
            "priority": MAILMSG_PRIORITY_CONTROL,
            "window": 1,
            "operations": [accel_temp["transport"], gyro["transport"]],
        },
    }


def peripheral_status_error(status):
    """Translate Zephyr service status without treating private values as host errno."""
    known = {
        -1: ("peripheral_invalid_request", "Zephyr rejected the peripheral request"),
        -2: ("peripheral_timeout", "Zephyr peripheral polling timed out"),
        -19: ("peripheral_unavailable", "peripheral is unavailable in the active pin profile"),
        -22: ("peripheral_invalid_argument", "Zephyr rejected a peripheral argument"),
    }
    return known.get(status, ("peripheral_failed", "Zephyr peripheral operation failed"))


def execute_zephyr_increment(value, device_root="/dev", timeout_ms=1000):
    """Execute exactly one reliable p1 request with at most one in flight."""
    path = os.path.join(device_root, "mailmsg-p1")
    flags = os.O_RDWR | os.O_NONBLOCK
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise AgentError(
            "mailmsg_open_failed",
            os.strerror(exc.errno),
            {"path": path, "errno": exc.errno},
        ) from exc

    try:
        request = pack_frame(MAILMSG_PRIORITY_CONTROL, MAILMSG_MSG_PING, value)
        try:
            written = os.write(fd, request)
        except OSError as exc:
            code = "queue_full" if exc.errno == errno.ENOSPC else "mailmsg_write_failed"
            raise AgentError(code, os.strerror(exc.errno), {"errno": exc.errno}) from exc
        if written != MAILMSG_FRAME.size:
            raise AgentError(
                "short_write",
                "MailMsg accepted only part of the request",
                {"expected": MAILMSG_FRAME.size, "actual": written},
            )

        poller = select.poll()
        poller.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
        deadline = time.monotonic() + timeout_ms / 1000.0
        ack = None
        pong = None
        while ack is None or pong is None:
            remaining_ms = max(0, int((deadline - time.monotonic()) * 1000))
            if remaining_ms == 0:
                raise AgentError(
                    "timeout",
                    "timed out waiting for Zephyr ACK/PONG",
                    {"ack_received": ack is not None, "pong_received": pong is not None},
                )
            events = poller.poll(remaining_ms)
            if not events:
                continue
            event = events[0][1]
            if event & (select.POLLERR | select.POLLHUP) and not event & select.POLLIN:
                raise AgentError("mailmsg_offline", "MailMsg session went offline")
            try:
                frame = unpack_frame(os.read(fd, MAILMSG_FRAME.size))
            except BlockingIOError:
                continue

            if frame["type"] == MAILMSG_MSG_ACK:
                if frame["length"] < 8:
                    raise AgentError("invalid_ack", "ACK payload is too short")
                status = _read_u32(frame["payload"], 4)
                if status != 0:
                    raise AgentError("ack_failed", "Zephyr returned a nonzero ACK status", {"status": status})
                ack = {"sequence": frame["sequence"], "peer_sequence": _read_u32(frame["payload"])}
            elif frame["type"] == MAILMSG_MSG_NACK:
                reason = _read_u32(frame["payload"], 4) if frame["length"] >= 8 else None
                raise AgentError("nack", "Zephyr rejected the request", {"reason": reason})
            elif frame["type"] == MAILMSG_MSG_PONG:
                result = _read_u32(frame["payload"])
                if result != value + 1:
                    raise AgentError(
                        "unexpected_result",
                        "Zephyr returned the wrong increment result",
                        {"expected": value + 1, "actual": result},
                    )
                pong = {"sequence": frame["sequence"], "value": result}
            else:
                raise AgentError(
                    "unexpected_frame",
                    "received an unexpected MailMsg type",
                    {"type": frame["type"]},
                )
        return {
            "ok": True,
            "tool": TOOL_INCREMENT,
            "arguments": {"value": value},
            "result": {"value": pong["value"]},
            "transport": {
                "priority": MAILMSG_PRIORITY_CONTROL,
                "window": 1,
                "ack_sequence": ack["sequence"],
                "ack_peer_sequence": ack["peer_sequence"],
                "pong_sequence": pong["sequence"],
            },
        }
    finally:
        os.close(fd)


def execute_zephyr_gpio_read(line, device_root="/dev", timeout_ms=1000):
    """Read one allowlisted GPIO over reliable priority 1."""
    if line not in GPIO_READ_LINES:
        raise AgentError("invalid_arguments", "line must be one of 25, 28, or 29")
    path = os.path.join(device_root, "mailmsg-p1")
    flags = os.O_RDWR | os.O_NONBLOCK
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise AgentError(
            "mailmsg_open_failed",
            os.strerror(exc.errno),
            {"path": path, "errno": exc.errno},
        ) from exc

    try:
        request = pack_frame(
            MAILMSG_PRIORITY_CONTROL, MAILMSG_MSG_GPIO_READ_REQUEST, line
        )
        try:
            written = os.write(fd, request)
        except OSError as exc:
            code = "queue_full" if exc.errno == errno.ENOSPC else "mailmsg_write_failed"
            raise AgentError(code, os.strerror(exc.errno), {"errno": exc.errno}) from exc
        if written != MAILMSG_FRAME.size:
            raise AgentError(
                "short_write",
                "MailMsg accepted only part of the request",
                {"expected": MAILMSG_FRAME.size, "actual": written},
            )

        poller = select.poll()
        poller.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
        deadline = time.monotonic() + timeout_ms / 1000.0
        ack = None
        result = None
        while ack is None or result is None:
            remaining_ms = max(0, int((deadline - time.monotonic()) * 1000))
            if remaining_ms == 0:
                raise AgentError(
                    "timeout",
                    "timed out waiting for Zephyr ACK/GPIO result",
                    {"ack_received": ack is not None, "result_received": result is not None},
                )
            events = poller.poll(remaining_ms)
            if not events:
                continue
            event = events[0][1]
            if event & (select.POLLERR | select.POLLHUP) and not event & select.POLLIN:
                raise AgentError("mailmsg_offline", "MailMsg session went offline")
            try:
                frame = unpack_frame(os.read(fd, MAILMSG_FRAME.size))
            except BlockingIOError:
                continue

            if frame["type"] == MAILMSG_MSG_ACK:
                if frame["length"] < 8:
                    raise AgentError("invalid_ack", "ACK payload is too short")
                status = _read_u32(frame["payload"], 4)
                if status != 0:
                    raise AgentError("ack_failed", "Zephyr returned a nonzero ACK status", {"status": status})
                ack = {"sequence": frame["sequence"], "peer_sequence": _read_u32(frame["payload"])}
            elif frame["type"] == MAILMSG_MSG_NACK:
                reason = _read_u32(frame["payload"], 4) if frame["length"] >= 8 else None
                raise AgentError("nack", "Zephyr rejected the request", {"reason": reason})
            elif frame["type"] == MAILMSG_MSG_GPIO_READ_RESULT:
                if frame["length"] != 12:
                    raise AgentError("invalid_gpio_result", "GPIO result payload must be 12 bytes")
                result_line = _read_u32(frame["payload"], 4)
                value = _read_u32(frame["payload"], 8)
                if result_line != line or value not in (0, 1):
                    raise AgentError(
                        "invalid_gpio_result",
                        "Zephyr returned an invalid GPIO sample",
                        {"line": result_line, "value": value},
                    )
                result = {
                    "sequence": frame["sequence"],
                    "peer_sequence": _read_u32(frame["payload"]),
                    "line": result_line,
                    "value": value,
                }
            else:
                raise AgentError(
                    "unexpected_frame",
                    "received an unexpected MailMsg type",
                    {"type": frame["type"]},
                )

        if ack["peer_sequence"] != result["peer_sequence"]:
            raise AgentError(
                "sequence_mismatch",
                "ACK and GPIO result refer to different requests",
                {"ack": ack["peer_sequence"], "result": result["peer_sequence"]},
            )
        return {
            "ok": True,
            "tool": TOOL_GPIO_READ,
            "arguments": {"line": line},
            "result": {"line": result["line"], "value": result["value"]},
            "transport": {
                "priority": MAILMSG_PRIORITY_CONTROL,
                "window": 1,
                "ack_sequence": ack["sequence"],
                "ack_peer_sequence": ack["peer_sequence"],
                "result_sequence": result["sequence"],
            },
        }
    finally:
        os.close(fd)


def build_model_request(model, prompt, native_tools=False):
    if native_tools:
        system_prompt = (
            "Choose exactly one registered tool. Return only one "
            "<tool_call>{JSON}</tool_call> block and no prose."
        )
    else:
        system_prompt = (
            "You are a tool decision router. Choose exactly one registered "
            "tool from this JSON schema list: %s. Return exactly one object "
            "with name and arguments, and no prose."
            % json.dumps(TOOL_SCHEMAS, separators=(",", ":"))
        )
    if native_tools:
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt},
        ]
    else:
        # The vendor RKLLM Flask example forwards only the newest user/tool
        # message when native tools are absent.  Put the complete policy in
        # that message so the model actually receives it.
        messages = [
            {
                "role": "user",
                "content": "%s\nRequest: %s" % (system_prompt, prompt),
            }
        ]

    body = {
        "model": model,
        "messages": messages,
        "stream": False,
        "temperature": 0.0,
        "top_k": 1,
        "max_tokens": 128,
        "enable_thinking": False,
    }
    if native_tools:
        body["tools"] = TOOL_SCHEMAS
    return body


def build_chat_request(model, prompt):
    return {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "stream": False,
        "temperature": 0.0,
        "top_k": 1,
        "max_tokens": 128,
        "enable_thinking": False,
    }


def request_model_content(server, body, timeout_s):
    url = server.rstrip("/") + "/v1/chat/completions"
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json", "Authorization": "not_required"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            reply = json.load(response)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
        raise AgentError("model_api_failed", str(exc), {"url": url}) from exc
    try:
        return reply["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        raise AgentError("invalid_model_response", "model API response has no assistant content") from exc


def request_model_decision(server, model, prompt, timeout_s, native_tools=False):
    body = build_model_request(model, prompt, native_tools)
    return request_model_content(server, body, timeout_s)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--decision", help="exact JSON or one <tool_call> block")
    source.add_argument("--prompt", help="ask the RKLLM API to choose the tool call")
    source.add_argument(
        "--chat-smoke",
        metavar="PROMPT",
        help="request plain text only; never validate or execute a tool",
    )
    parser.add_argument("--server", default="http://127.0.0.1:8080")
    parser.add_argument("--model", default="rkllm")
    parser.add_argument("--device-root", default="/dev")
    parser.add_argument("--timeout-ms", type=int, default=1000)
    parser.add_argument("--api-timeout", type=float, default=120.0)
    parser.add_argument(
        "--native-tools",
        action="store_true",
        help="use the server's native Function Calling template (requires a compatible model)",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="validate the model decision without opening or writing a MailMsg device",
    )
    parser.add_argument(
        "--print-model-output",
        action="store_true",
        help="print the untrusted model tool-decision text to stderr before validation",
    )
    args = parser.parse_args(argv)

    try:
        if args.chat_smoke is not None:
            raw = request_model_content(
                args.server,
                build_chat_request(args.model, args.chat_smoke),
                args.api_timeout,
            )
            if not isinstance(raw, str) or not raw.strip():
                raise AgentError("empty_model_response", "model returned no text")
            print(
                json.dumps(
                    {"ok": True, "executed": False, "response": raw},
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
            )
            return 0

        raw = args.decision
        if raw is None:
            raw = request_model_decision(
                args.server, args.model, args.prompt, args.api_timeout, args.native_tools
            )
            if args.print_model_output:
                print("model_output=" + raw, file=sys.stderr)
        decision = parse_decision(raw)
        if args.validate_only:
            print(
                json.dumps(
                    {"ok": True, "executed": False, "decision": decision},
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
            )
            return 0
        if decision["name"] == TOOL_INCREMENT:
            result = execute_zephyr_increment(
                decision["arguments"]["value"], args.device_root, args.timeout_ms
            )
        elif decision["name"] == TOOL_GPIO_READ:
            result = execute_zephyr_gpio_read(
                decision["arguments"]["line"], args.device_root, args.timeout_ms
            )
        elif decision["name"] == TOOL_IMU_READ:
            result = execute_zephyr_imu_read(args.device_root, args.timeout_ms)
        else:
            result = execute_zephyr_peripheral(
                decision["name"], decision["arguments"],
                args.device_root, args.timeout_ms
            )
        result["decision"] = decision
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        return 0
    except AgentError as exc:
        print(json.dumps(exc.as_dict(), ensure_ascii=False, separators=(",", ":")), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
