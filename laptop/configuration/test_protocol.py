import importlib.util
import unittest
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


if __name__ == "__main__":
    unittest.main()
