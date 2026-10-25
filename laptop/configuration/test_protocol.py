import asyncio
import importlib.util
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("ble_config.py")
spec = importlib.util.spec_from_file_location("ble_config", MODULE_PATH)
ble_config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ble_config)


def call(name, **kwargs):
    return {"name": name, "arguments": kwargs}


class PacketTests(unittest.TestCase):
    def test_scroll_up(self):
        packet, target = ble_config.build_packet(
            call("set_gesture_mapping", target="COMPUTER",
                 gesture="SWIPE_UP", action="MOUSE_SCROLL_UP", amount=3))
        self.assertEqual(target, "COMPUTER")
        self.assertEqual(list(packet)[2:], [2, 1, 1, 9, 0, 0, 3, 0, 0, 0])
        self.assertEqual(len(packet), 12)

    def test_keyboard_modifiers(self):
        packet, _ = ble_config.build_packet(
            call("set_gesture_mapping", target="COMPUTER",
                 gesture="SWIPE_LEFT", action="KEY_TAP", key="A",
                 modifiers=["CTRL", "SHIFT"]))
        self.assertEqual(packet[6], 0x04)
        self.assertEqual(packet[7], 0x03)
        self.assertEqual(packet[9], 35)

    def test_brightness(self):
        packet, _ = ble_config.build_packet(
            call("set_gesture_mapping", target="LIGHT",
                 gesture="SWIPE_UP", action="LIGHT_SET_BRIGHTNESS",
                 amount=75))
        self.assertEqual(packet[3], 2)
        self.assertEqual(packet[5], 14)
        self.assertEqual(packet[8], 75)

    def test_key_toggle(self):
        packet, target = ble_config.build_packet(
            call("set_gesture_mapping", target="COMPUTER",
                 gesture="SWIPE_UP", action="KEY_TOGGLE", key="W"))
        self.assertEqual(target, "COMPUTER")
        self.assertEqual(packet[5], 18)
        self.assertEqual(packet[6], 0x1A)
        self.assertEqual(packet[9:11], bytes(2))

    def test_toggle_rejects_duration(self):
        with self.assertRaises(ValueError):
            ble_config.build_packet(
                call("set_gesture_mapping", target="COMPUTER",
                     gesture="SWIPE_UP", action="KEY_TOGGLE",
                     key="W", duration_ms=100))

    def test_reset_all(self):
        packet, _ = ble_config.build_packet(
            call("reset_gesture_mapping", target="COMPUTER", gesture="ALL"))
        self.assertEqual(packet[2], 4)
        self.assertEqual(packet[4], 0)
        self.assertEqual(packet[5:], bytes(7))

    def test_get(self):
        packet, _ = ble_config.build_packet(
            call("get_gesture_mapping", target="COMPUTER",
                 gesture="TENSE"))
        self.assertEqual((packet[2], packet[4]), (1, 5))

    def test_extend_and_flex_configuration(self):
        packet, target = ble_config.build_packet(
            call("set_gesture_mapping", target="COMPUTER",
                 gesture="EXTEND", action="KEY_TOGGLE", key="E"))
        self.assertEqual((target, packet[4], packet[5]),
                         ("COMPUTER", 6, 18))
        self.assertEqual(len(packet), 12)

        packet, target = ble_config.build_packet(
            call("set_gesture_mapping", target="LIGHT",
                 gesture="FLEX", action="LIGHT_TOGGLE"))
        self.assertEqual((target, packet[4], packet[5]),
                         ("LIGHT", 7, 11))
        self.assertEqual(len(packet), 12)

    def test_get_and_reset_new_gestures(self):
        packet, _ = ble_config.build_packet(
            call("get_gesture_mapping", target="COMPUTER", gesture="FLEX"))
        self.assertEqual((packet[2], packet[4]), (1, 7))

        packet, _ = ble_config.build_packet(
            call("reset_gesture_mapping", target="LIGHT", gesture="EXTEND"))
        self.assertEqual((packet[2], packet[4]), (3, 6))

    def test_wrong_target(self):
        with self.assertRaises(ValueError):
            ble_config.build_packet(
                call("set_gesture_mapping", target="LIGHT",
                     gesture="SWIPE_UP", action="KEY_TAP", key="A"))

    def test_unused_parameters(self):
        with self.assertRaises(ValueError):
            ble_config.build_packet(
                call("set_gesture_mapping", target="COMPUTER",
                     gesture="TENSE", action="MOUSE_LEFT_CLICK", key="A"))

    def test_out_of_range(self):
        with self.assertRaises(ValueError):
            ble_config.build_packet(
                call("set_gesture_mapping", target="LIGHT",
                     gesture="TENSE", action="LIGHT_SET_BRIGHTNESS",
                     amount=150))

    def test_missing_key(self):
        with self.assertRaises(ValueError):
            ble_config.build_packet(
                call("set_gesture_mapping", target="COMPUTER",
                     gesture="TENSE", action="KEY_TAP"))

    def test_response(self):
        request, target = ble_config.build_packet(
            call("set_gesture_mapping", target="COMPUTER",
                 gesture="SWIPE_RIGHT", action="KEY_TAP", key="ENTER"))
        response = bytes([1, request[1], 2, 1, 4, 0, 1, 0x28, 0, 0, 35, 0])
        result = ble_config.parse_response(request, target, response)
        self.assertEqual(result["action"], "KEY_TAP")
        self.assertEqual(result["key"], "ENTER")



class BatchTests(unittest.TestCase):
    @staticmethod
    def sample():
        return {
            "target": "COMPUTER",
            "mappings": [
                {"gesture": "SWIPE_UP", "action": "KEY_TOGGLE", "key": "W"},
                {"gesture": "SWIPE_DOWN", "action": "KEY_TAP", "key": "S"},
                {"gesture": "SWIPE_LEFT", "action": "KEY_TAP", "key": "A"},
                {"gesture": "SWIPE_RIGHT", "action": "KEY_TAP", "key": "D"},
                {"gesture": "TENSE", "action": "KEY_TAP", "key": "SPACE"},
                {"gesture": "EXTEND", "action": "KEY_TAP", "key": "E"},
                {"gesture": "FLEX", "action": "KEY_TAP", "key": "F"},
            ],
        }

    def test_build_seven_mappings(self):
        target, requests = ble_config.build_batch(self.sample())
        self.assertEqual(target, "COMPUTER")
        self.assertEqual(len(requests), 7)
        for request in requests:
            self.assertEqual(len(request["set_packet"]), 12)
            self.assertEqual(len(request["get_packet"]), 12)
            self.assertEqual(request["set_packet"][2], 2)
            self.assertEqual(request["get_packet"][2], 1)

    def test_five_mapping_batch_still_valid(self):
        batch = self.sample()
        batch["mappings"] = batch["mappings"][:5]
        _, requests = ble_config.build_batch(batch)
        self.assertEqual(len(requests), 5)

    def test_duplicate_rejected(self):
        batch = self.sample()
        batch["mappings"].append(batch["mappings"][0])
        # Test duplicates without exceeding the maximum batch size.
        batch["mappings"].pop()
        batch["mappings"][1] = dict(batch["mappings"][0])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            ble_config.build_batch(batch)

    def test_invalid_mapping_rejected(self):
        batch = self.sample()
        batch["mappings"][-1]["action"] = "LIGHT_TOGGLE"
        with self.assertRaises(ValueError):
            ble_config.build_batch(batch)

    def test_empty_batch_rejected(self):
        with self.assertRaises(ValueError):
            ble_config.build_batch({"target": "COMPUTER", "mappings": []})

    def test_all_seven_verified_one_connection(self):
        batch = self.sample()
        calls = {"connect": 0, "writes": 0}
        current = {}

        class FakeScanner:
            @staticmethod
            async def find_device_by_name(name, timeout):
                return "fake-device"

        class FakeClient:
            def __init__(self, device, timeout):
                calls["connect"] += 1

            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                pass

            async def write_gatt_char(self, uuid, data, response):
                calls["writes"] += 1
                self.request = bytes(data)
                if data[2] == 2:
                    current[data[4]] = data[5:11]

            async def read_gatt_char(self, uuid):
                if uuid == ble_config.UUID_INFO:
                    return bytes([1, 1, 7, 3])
                req = self.request
                mapping = current.get(req[4], bytes(6))
                return bytes([1, req[1], req[2], 1, req[4], 0]) + mapping

        fake_bleak = types.SimpleNamespace(
            BleakClient=FakeClient, BleakScanner=FakeScanner
        )
        with patch.dict(sys.modules, {"bleak": fake_bleak}):
            result = asyncio.run(ble_config.execute_batch(batch))

        self.assertTrue(result["success"], result)
        self.assertEqual(result["verified"], 7)
        self.assertEqual(calls["connect"], 1)
        self.assertEqual(calls["writes"], 14)


class ToolDispatchTests(unittest.TestCase):
    def test_named_batch_dispatch(self):
        batch = BatchTests.sample()
        expected = {"success": True, "verified": 7, "requested": 7}
        with patch.object(
            ble_config, "execute_batch",
            new_callable=AsyncMock, return_value=expected
        ) as mock_execute:
            result = asyncio.run(ble_config.execute_tool_call(
                call("configure_gesture_batch", **batch)
            ))
        self.assertEqual(result, expected)
        mock_execute.assert_awaited_once_with(batch)

    def test_single_set_uses_verified_batch(self):
        mapping = {
            "target": "COMPUTER", "gesture": "SWIPE_UP",
            "status": "OK", "action": "KEY_TOGGLE", "key": "W",
        }
        with patch.object(
            ble_config, "execute_batch", new_callable=AsyncMock,
            return_value={
                "success": True, "results": [
                    {"status": "VERIFIED", "mapping": mapping}
                ],
            },
        ) as mock_execute:
            result = asyncio.run(ble_config.execute_tool_call(call(
                "set_gesture_mapping", target="COMPUTER",
                gesture="SWIPE_UP", action="KEY_TOGGLE", key="W"
            )))
        self.assertEqual(result, mapping)
        self.assertEqual(
            mock_execute.await_args.args[0]["mappings"],
            [{"gesture": "SWIPE_UP", "action": "KEY_TOGGLE", "key": "W"}],
        )

    def test_single_set_failure(self):
        with patch.object(
            ble_config, "execute_batch", new_callable=AsyncMock,
            return_value={
                "success": False,
                "error": "Stopped at TENSE: Readback mismatch",
            },
        ):
            with self.assertRaisesRegex(RuntimeError, "Readback mismatch"):
                asyncio.run(ble_config.execute_tool_call(call(
                    "set_gesture_mapping", target="COMPUTER",
                    gesture="TENSE", action="KEY_TAP", key="SPACE"
                )))

    def test_invalid_unhashable_values(self):
        for field in ("target", "gesture", "action"):
            args = {
                "target": "COMPUTER", "gesture": "TENSE",
                "action": "KEY_TAP", "key": "SPACE",
            }
            args[field] = []
            with self.subTest(field=field), self.assertRaises(ValueError):
                ble_config.build_packet(call("set_gesture_mapping", **args))

        with self.assertRaises(ValueError):
            ble_config.build_packet(call(
                "set_gesture_mapping", target="COMPUTER",
                gesture="TENSE", action="KEY_TAP", key="SPACE",
                modifiers=[{}],
            ))

    def test_eight_mapping_batch_rejected(self):
        batch = BatchTests.sample()
        batch["mappings"].append({
            "gesture": "SWIPE_UP", "action": "KEY_TAP", "key": "Q",
        })
        with self.assertRaises(ValueError):
            ble_config.build_batch(batch)

    @staticmethod
    def _fake_ble(info_gestures=7, fail_on_set=None):
        """Simulate BLE receiver responses and preserve prior successful writes."""
        state = {"writes": 0, "current": {}, "connections": 0}

        class FakeScanner:
            @staticmethod
            async def find_device_by_name(name, timeout):
                return "fake-receiver"

        class FakeClient:
            def __init__(self, device, timeout):
                state["connections"] += 1
                self.request = None

            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                pass

            async def write_gatt_char(self, uuid, data, response):
                state["writes"] += 1
                self.request = bytes(data)
                if data[2] == 2:
                    if fail_on_set == data[4]:
                        raise OSError("Simulated SET timeout")
                    state["current"][data[4]] = data[5:11]

            async def read_gatt_char(self, uuid):
                if uuid == ble_config.UUID_INFO:
                    return bytes([1, 1, info_gestures, 3])
                req = self.request
                return (
                    bytes([1, req[1], req[2], 1, req[4], 0])
                    + state["current"].get(req[4], bytes(6))
                )

        return state, types.SimpleNamespace(
            BleakClient=FakeClient, BleakScanner=FakeScanner
        )

    def test_older_receiver_rejected_before_writes(self):
        state, fake_bleak = self._fake_ble(info_gestures=5)
        with patch.dict(sys.modules, {"bleak": fake_bleak}):
            result = asyncio.run(ble_config.execute_batch(
                BatchTests.sample()
            ))
        self.assertFalse(result["success"])
        self.assertEqual(result["verified"], 0)
        self.assertEqual(state["writes"], 0)
        self.assertIn("Update receiver firmware", result["error"])

    def test_partial_failure_reports_verified_prefix(self):
        batch = {
            "target": "COMPUTER",
            "mappings": [
                {"gesture": "SWIPE_UP", "action": "KEY_TAP", "key": "W"},
                {"gesture": "SWIPE_DOWN", "action": "KEY_TAP", "key": "S"},
                {"gesture": "TENSE", "action": "KEY_TAP", "key": "SPACE"},
            ],
        }
        state, fake_bleak = self._fake_ble(fail_on_set=2)
        with patch.dict(sys.modules, {"bleak": fake_bleak}):
            result = asyncio.run(ble_config.execute_batch(batch))
        self.assertFalse(result["success"])
        self.assertEqual(result["verified"], 1)
        self.assertEqual(
            [row["status"] for row in result["results"]],
            ["VERIFIED", "UNVERIFIED"],
        )
        self.assertIn(1, state["current"])
        self.assertNotIn(2, state["current"])
        self.assertEqual(state["connections"], 1)



if __name__ == "__main__":
    unittest.main()
