# SPDX-License-Identifier: MIT
import contextlib
import io
import json
import os
import struct
import sys
import unittest
from unittest import mock


sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mailmsg_agent


class DecisionTest(unittest.TestCase):
    def test_exact_decision(self):
        decision = mailmsg_agent.parse_decision(
            '{"name":"zephyr_increment","arguments":{"value":41}}'
        )
        self.assertEqual(decision["arguments"]["value"], 41)

    def test_tool_call_wrapper(self):
        decision = mailmsg_agent.parse_decision(
            '<tool_call>{"name":"zephyr_increment","arguments":{"value":7}}</tool_call>'
        )
        self.assertEqual(decision["name"], "zephyr_increment")

    def test_native_tool_call_format(self):
        decision = mailmsg_agent.parse_decision(
            "<tool_call>\n"
            "<function=zephyr_increment>\n"
            "<parameter=value>\n"
            "41\n"
            "</parameter>\n"
            "</function>\n"
            "</tool_call>"
        )
        self.assertEqual(decision, {"name": "zephyr_increment", "arguments": {"value": 41}})

    def test_native_multi_parameter_tool_call(self):
        decision = mailmsg_agent.parse_decision(
            "<tool_call><function=zephyr_spi_transfer>"
            "<parameter=chip_select>1</parameter>"
            "<parameter=mode>3</parameter>"
            "<parameter=data_hex>a55a</parameter>"
            "</function></tool_call>"
        )
        self.assertEqual(decision, {"name": "zephyr_spi_transfer",
            "arguments": {"chip_select": 1, "mode": 3, "data_hex": "a55a"}})

    def test_imu_native_empty_argument_template_quirk_is_bounded(self):
        decision = mailmsg_agent.parse_decision(
            "<tool_call><function=zephyr_imu_read></parameter></function></tool_call>"
        )
        self.assertEqual(decision, {"name": "zephyr_imu_read", "arguments": {}})
        with self.assertRaises(mailmsg_agent.AgentError):
            mailmsg_agent.parse_decision(
                "<tool_call><function=zephyr_pwm_stop></parameter></function></tool_call>"
            )

    def test_native_tool_call_rejects_extra_prose(self):
        with self.assertRaises(mailmsg_agent.AgentError) as caught:
            mailmsg_agent.parse_decision(
                "ready\n<tool_call><function=zephyr_increment>"
                "<parameter=value>41</parameter></function></tool_call>"
            )
        self.assertEqual(caught.exception.code, "invalid_json")

    def test_rejects_unknown_tool(self):
        with self.assertRaises(mailmsg_agent.AgentError) as caught:
            mailmsg_agent.parse_decision(
                '{"name":"run_shell","arguments":{"value":1}}'
            )
        self.assertEqual(caught.exception.code, "tool_not_allowed")

    def test_known_tool_name_aliases_are_canonicalized(self):
        decision = mailmsg_agent.parse_decision(
            '{"name":"zephyr_ii_c_write_read","arguments":'
            '{"address":104,"prefix_hex":"43","length":6}}'
        )
        self.assertEqual(decision["name"], "zephyr_i2c_write_read")

        decision = mailmsg_agent.parse_decision(
            '{"name":"zephyr_imu_reading","arguments":{}}'
        )
        self.assertEqual(decision, {"name": "zephyr_imu_read", "arguments": {}})

        with self.assertRaises(mailmsg_agent.AgentError) as caught:
            mailmsg_agent.parse_decision('{"name":"zephyr_imu_write","arguments":{}}')
        self.assertEqual(caught.exception.code, "tool_not_allowed")

    def test_rejects_extra_argument(self):
        with self.assertRaises(mailmsg_agent.AgentError) as caught:
            mailmsg_agent.parse_decision(
                '{"name":"zephyr_increment","arguments":{"value":1,"command":"id"}}'
            )
        self.assertEqual(caught.exception.code, "invalid_decision")

    def test_rejects_boolean_and_overflow(self):
        for value in (True, -1, 0xFFFFFFFF):
            with self.subTest(value=value):
                with self.assertRaises(mailmsg_agent.AgentError):
                    mailmsg_agent.parse_decision(
                        json.dumps({"name": "zephyr_increment", "arguments": {"value": value}})
                    )

    def test_gpio_read_accepts_only_owned_lines(self):
        for line in (25, 28, 29):
            with self.subTest(line=line):
                self.assertEqual(
                    mailmsg_agent.parse_decision(
                        json.dumps({"name": "zephyr_gpio_read", "arguments": {"line": line}})
                    ),
                    {"name": "zephyr_gpio_read", "arguments": {"line": line}},
                )
        for line in (True, 24, 27, 30):
            with self.subTest(line=line):
                with self.assertRaises(mailmsg_agent.AgentError):
                    mailmsg_agent.parse_decision(
                        json.dumps({"name": "zephyr_gpio_read", "arguments": {"line": line}})
                    )

    def test_peripheral_tools_validate_bounds(self):
        cases = [
            ("zephyr_uart_write", {"port": 5, "data": "OK"}),
            ("zephyr_i2c_write_read", {"address": 0x48, "prefix_hex": "01", "length": 2}),
            ("zephyr_spi_transfer", {"chip_select": 1, "mode": 3, "data_hex": "a55a"}),
            ("zephyr_pwm_set", {"period_ns": 1000000, "duty_ns": 250000, "polarity": 0}),
            ("zephyr_adc_read", {"channel": 7}),
            ("zephyr_imu_read", {}),
        ]
        for name, arguments in cases:
            with self.subTest(name=name):
                decision = mailmsg_agent.parse_decision(json.dumps({"name": name, "arguments": arguments}))
                self.assertEqual(decision, {"name": name, "arguments": arguments})


class ImuExecutionTest(unittest.TestCase):
    def test_imu_read_uses_fixed_i2c7_registers_and_decodes_signed_values(self):
        replies = [
            {"ok": True, "transport": {"result_sequence": 1},
             "result": {"data_hex": "1d682bf8da881030"}},
            {"ok": True, "transport": {"result_sequence": 2},
             "result": {"data_hex": "ff28040dfe15"}},
        ]
        with mock.patch.object(mailmsg_agent, "execute_zephyr_peripheral", side_effect=replies) as rpc:
            result = mailmsg_agent.execute_zephyr_imu_read("/test", 123)

        self.assertEqual(result["result"]["accelerometer_raw"],
                         {"x": 7528, "y": 11256, "z": -9592})
        self.assertEqual(result["result"]["temperature_raw"], 4144)
        self.assertEqual(result["result"]["gyroscope_raw"],
                         {"x": -216, "y": 1037, "z": -491})
        self.assertEqual(
            [call.args[:2] for call in rpc.call_args_list],
            [
                ("zephyr_i2c_write_read", {"address": 104, "prefix_hex": "3b", "length": 8}),
                ("zephyr_i2c_write_read", {"address": 104, "prefix_hex": "43", "length": 6}),
            ],
        )

    def test_peripheral_tools_reject_unsafe_arguments(self):
        cases = [
            ("zephyr_uart_write", {"port": 6, "data": "OK"}),
            ("zephyr_i2c_write", {"address": 0x80, "data_hex": "00"}),
            ("zephyr_i2c_write", {"address": 0x48, "data_hex": "xyz"}),
            ("zephyr_spi_transfer", {"chip_select": 0, "mode": 4, "data_hex": "00"}),
            ("zephyr_pwm_set", {"period_ns": 10, "duty_ns": 11, "polarity": 0}),
            ("zephyr_adc_read", {"channel": 8}),
        ]
        for name, arguments in cases:
            with self.subTest(name=name), self.assertRaises(mailmsg_agent.AgentError):
                mailmsg_agent.parse_decision(json.dumps({"name": name, "arguments": arguments}))


class FrameTest(unittest.TestCase):
    def test_user_record_is_48_bytes(self):
        data = mailmsg_agent.pack_frame(1, mailmsg_agent.MAILMSG_MSG_PING, 41)
        self.assertEqual(len(data), 48)
        priority, message_type, sequence, length, payload = struct.unpack("<IIII32s", data)
        self.assertEqual((priority, message_type, sequence, length), (1, 1, 0, 4))
        self.assertEqual(struct.unpack_from("<I", payload)[0], 41)

    def test_unpack_rejects_wrong_priority(self):
        data = mailmsg_agent.MAILMSG_FRAME.pack(2, 2, 1, 4, struct.pack("<I", 42) + bytes(28))
        with self.assertRaises(mailmsg_agent.AgentError) as caught:
            mailmsg_agent.unpack_frame(data)
        self.assertEqual(caught.exception.code, "wrong_priority")

    def test_gpio_read_request_record(self):
        data = mailmsg_agent.pack_frame(
            1, mailmsg_agent.MAILMSG_MSG_GPIO_READ_REQUEST, 25
        )
        priority, message_type, sequence, length, payload = struct.unpack("<IIII32s", data)
        self.assertEqual((priority, message_type, sequence, length), (1, 9, 0, 4))
        self.assertEqual(struct.unpack_from("<I", payload)[0], 25)

    def test_peripheral_request_record(self):
        data = mailmsg_agent.pack_peripheral_frame(
            mailmsg_agent.PERIPH_SPI_TRANSFER, 1, 3, 0, bytes.fromhex("a55a")
        )
        priority, message_type, sequence, length, payload = struct.unpack("<IIII32s", data)
        self.assertEqual((priority, message_type, sequence, length), (1, 11, 0, 28))
        operation, arg0, arg1, arg2, size, inline = struct.unpack("<IIIII8s", payload[:28])
        self.assertEqual((operation, arg0, arg1, arg2, size), (9, 1, 3, 0, 2))
        self.assertEqual(inline[:size], bytes.fromhex("a55a"))

    def test_private_peripheral_timeout_is_not_host_enoent(self):
        code, message = mailmsg_agent.peripheral_status_error(-2)
        self.assertEqual(code, "peripheral_timeout")
        self.assertIn("timed out", message)
        self.assertNotIn("file", message.lower())

    def test_unknown_peripheral_status_stays_generic(self):
        self.assertEqual(
            mailmsg_agent.peripheral_status_error(-1234),
            ("peripheral_failed", "Zephyr peripheral operation failed"),
        )


class ModelRequestTest(unittest.TestCase):
    def test_strict_json_mode_registers_tool_in_prompt_only(self):
        request = mailmsg_agent.build_model_request("rkllm", "increment 41")
        self.assertNotIn("tools", request)
        self.assertEqual(len(request["messages"]), 1)
        self.assertEqual(request["messages"][0]["role"], "user")
        self.assertIn("zephyr_increment", request["messages"][0]["content"])
        self.assertIn("increment 41", request["messages"][0]["content"])

    def test_native_mode_exposes_function_schema(self):
        request = mailmsg_agent.build_model_request(
            "rkllm", "increment 41", native_tools=True
        )
        self.assertEqual(request["tools"], mailmsg_agent.TOOL_SCHEMAS)
        self.assertEqual([m["role"] for m in request["messages"]], ["system", "user"])

    def test_chat_smoke_request_has_no_tools(self):
        request = mailmsg_agent.build_chat_request("rkllm", "Reply with READY")
        self.assertNotIn("tools", request)
        self.assertEqual(
            request["messages"],
            [{"role": "user", "content": "Reply with READY"}],
        )


class MainTest(unittest.TestCase):
    def test_chat_smoke_never_executes_mailmsg(self):
        output = io.StringIO()
        with mock.patch.object(
            mailmsg_agent,
            "request_model_content",
            return_value="READY",
        ), mock.patch.object(
            mailmsg_agent,
            "execute_zephyr_increment",
            side_effect=AssertionError("MailMsg must not execute"),
        ), contextlib.redirect_stdout(output):
            result = mailmsg_agent.main(["--chat-smoke", "Reply with READY"])

        self.assertEqual(result, 0)
        reply = json.loads(output.getvalue())
        self.assertTrue(reply["ok"])
        self.assertFalse(reply["executed"])
        self.assertEqual(reply["response"], "READY")

    def test_validate_only_never_executes_mailmsg(self):
        output = io.StringIO()
        with mock.patch.object(
            mailmsg_agent,
            "execute_zephyr_increment",
            side_effect=AssertionError("MailMsg must not execute"),
        ), contextlib.redirect_stdout(output):
            result = mailmsg_agent.main(
                [
                    "--decision",
                    '{"name":"zephyr_increment","arguments":{"value":41}}',
                    "--validate-only",
                ]
            )

        self.assertEqual(result, 0)
        reply = json.loads(output.getvalue())
        self.assertTrue(reply["ok"])
        self.assertFalse(reply["executed"])
        self.assertEqual(reply["decision"]["arguments"]["value"], 41)

    def test_validate_only_checks_model_output_without_execution(self):
        output = io.StringIO()
        with mock.patch.object(
            mailmsg_agent,
            "request_model_decision",
            return_value='<tool_call>{"name":"zephyr_increment","arguments":{"value":7}}</tool_call>',
        ) as request_decision, mock.patch.object(
            mailmsg_agent,
            "execute_zephyr_increment",
            side_effect=AssertionError("MailMsg must not execute"),
        ), contextlib.redirect_stdout(output):
            result = mailmsg_agent.main(
                ["--prompt", "increment 7", "--native-tools", "--validate-only"]
            )

        self.assertEqual(result, 0)
        request_decision.assert_called_once()
        self.assertEqual(json.loads(output.getvalue())["decision"]["arguments"]["value"], 7)


if __name__ == "__main__":
    unittest.main()
