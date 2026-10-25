import argparse
import asyncio
import itertools
import json
from pathlib import Path

VERSION = 1
UUID_COMMAND = "89ca2ebf-7c18-4a33-9877-df55001abce4"
UUID_RESPONSE = "89ca2ebf-7c18-4a33-9877-df55001abce5"
UUID_INFO = "89ca2ebf-7c18-4a33-9877-df55001abce6"

TARGETS = {"COMPUTER": 1, "LIGHT": 2}
DEVICES = {"COMPUTER": "MyoPC", "LIGHT": "MyoLight"}
GESTURES = {
    "SWIPE_UP": 1,
    "SWIPE_DOWN": 2,
    "SWIPE_LEFT": 3,
    "SWIPE_RIGHT": 4,
    "TENSE": 5,
    "EXTEND": 6,
    "FLEX": 7,
    "ALL": 0,
}
ACTIONS = {
    "NONE": 0,
    "KEY_TAP": 1,
    "MOUSE_LEFT_CLICK": 2,
    "MOUSE_RIGHT_CLICK": 3,
    "MOUSE_MIDDLE_CLICK": 4,
    "MOUSE_MOVE_UP": 5,
    "MOUSE_MOVE_DOWN": 6,
    "MOUSE_MOVE_LEFT": 7,
    "MOUSE_MOVE_RIGHT": 8,
    "MOUSE_SCROLL_UP": 9,
    "MOUSE_SCROLL_DOWN": 10,
    "LIGHT_TOGGLE": 11,
    "LIGHT_ON": 12,
    "LIGHT_OFF": 13,
    "LIGHT_SET_BRIGHTNESS": 14,
    "LIGHT_BRIGHTNESS_UP": 15,
    "LIGHT_BRIGHTNESS_DOWN": 16,
    "MOUSE_DOUBLE_CLICK": 17,
    "KEY_TOGGLE": 18,
}
KEYS = {
    "ENTER": 0x28,
    "ESCAPE": 0x29,
    "BACKSPACE": 0x2A,
    "TAB": 0x2B,
    "SPACE": 0x2C,
    "INSERT": 0x49,
    "HOME": 0x4A,
    "PAGE_UP": 0x4B,
    "DELETE": 0x4C,
    "END": 0x4D,
    "PAGE_DOWN": 0x4E,
    "RIGHT_ARROW": 0x4F,
    "LEFT_ARROW": 0x50,
    "DOWN_ARROW": 0x51,
    "UP_ARROW": 0x52,
}
MODIFIERS = {"CTRL": 0x01, "SHIFT": 0x02, "ALT": 0x04, "GUI": 0x08}
STATUS = {
    0: "OK",
    1: "BAD_VERSION",
    2: "WRONG_TARGET",
    3: "INVALID_GESTURE",
    4: "UNSUPPORTED_ACTION",
    5: "INVALID_PARAMETER",
    6: "BUSY",
    7: "INVALID_OPERATION",
}
TRANSACTIONS = itertools.cycle(range(1, 256))


def keycode_for(value):
    if not isinstance(value, str):
        raise ValueError("Keyboard key must be a string")
    key = value.upper()
    if key in KEYS:
        return KEYS[key]
    if len(key) == 1 and "A" <= key <= "Z":
        return ord(key) - ord("A") + 0x04
    if len(key) == 1 and "1" <= key <= "9":
        return ord(key) - ord("1") + 0x1E
    if key == "0":
        return 0x27
    if key.startswith("F") and key[1:].isdigit():
        number = int(key[1:])
        if 1 <= number <= 12:
            return 0x3A + number - 1
    raise ValueError(f"Unsupported keyboard key: {value}")


def keyname_for(code):
    for name, value in KEYS.items():
        if value == code:
            return name
    if 0x04 <= code <= 0x1D:
        return chr(code - 0x04 + ord("A"))
    if 0x1E <= code <= 0x26:
        return str(code - 0x1E + 1)
    if code == 0x27:
        return "0"
    if 0x3A <= code <= 0x45:
        return f"F{code - 0x3A + 1}"
    return None


def _integer(value, name, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer from {minimum} to {maximum}")
    return value


def build_packet(call):
    if not isinstance(call, dict) or set(call) != {"name", "arguments"}:
        raise ValueError("Tool call must have name and arguments")
    name = call["name"]
    args = call["arguments"]
    if not isinstance(args, dict):
        raise ValueError("arguments must be an object")

    target = args.get("target")
    gesture = args.get("gesture")
    if not isinstance(target, str) or target not in TARGETS:
        raise ValueError(f"Unknown target: {target}")
    if not isinstance(gesture, str) or gesture not in GESTURES:
        raise ValueError(f"Unknown gesture: {gesture}")

    if name == "set_gesture_mapping":
        operation = 2
    elif name == "get_gesture_mapping":
        operation = 1
    elif name == "reset_gesture_mapping":
        operation = 4 if gesture == "ALL" else 3
    else:
        raise ValueError(f"Unknown function: {name}")

    if gesture == "ALL" and operation != 4:
        raise ValueError("ALL is only valid for reset")

    action = 0
    keycode = 0
    modifiers = 0
    amount = 0
    duration_ms = 0

    if operation == 2:
        action_name = args.get("action")
        if not isinstance(action_name, str) or action_name not in ACTIONS:
            raise ValueError(f"Unknown action: {action_name}")
        action = ACTIONS[action_name]

        computer_action = 1 <= action <= 10 or action in (17, 18)
        light_action = 11 <= action <= 16
        if target == "COMPUTER" and light_action:
            raise ValueError("Light action assigned to computer")
        if target == "LIGHT" and computer_action:
            raise ValueError("Computer action assigned to light")

        allowed = {"target", "gesture", "action"}
        clicks = {2, 3, 4, 17}
        movement = {5, 6, 7, 8}
        scrolling = {9, 10}
        brightness = {14, 15, 16}

        if action in (1, 18):
            allowed |= {"key", "modifiers"}
            if action == 1:
                allowed.add("duration_ms")
            if "key" not in args:
                raise ValueError("KEY_TAP requires key")
            keycode = keycode_for(args["key"])
            specified = args.get("modifiers", [])
            if (
                not isinstance(specified, list)
                or not all(isinstance(m, str) for m in specified)
                or len(specified) != len(set(specified))
            ):
                raise ValueError("modifiers must be a list of unique strings")
            for modifier in specified:
                if modifier not in MODIFIERS:
                    raise ValueError(f"Unknown modifier: {modifier}")
                modifiers |= MODIFIERS[modifier]
            if action == 1:
                duration_ms = _integer(args.get("duration_ms", 35),
                                       "duration_ms", 30, 2000)
        elif action in clicks:
            allowed.add("duration_ms")
            duration_ms = _integer(args.get("duration_ms", 35),
                                   "duration_ms", 30, 2000)
        elif action in movement:
            allowed.add("amount")
            amount = _integer(args.get("amount", 40), "amount", 1, 127)
        elif action in scrolling:
            allowed.add("amount")
            amount = _integer(args.get("amount", 1), "amount", 1, 10)
        elif action in brightness:
            allowed.add("amount")
            if action == 14 and "amount" not in args:
                raise ValueError("LIGHT_SET_BRIGHTNESS requires amount")
            default = 25 if action in (15, 16) else 0
            amount = _integer(args.get("amount", default), "amount",
                              0 if action == 14 else 1, 100)

        unexpected = set(args) - allowed
        if unexpected:
            raise ValueError(f"Unexpected parameters: {sorted(unexpected)}")
    elif set(args) != {"target", "gesture"}:
        raise ValueError("GET and RESET only accept target and gesture")

    transaction_id = next(TRANSACTIONS)
    packet = bytes([
        VERSION, transaction_id, operation, TARGETS[target],
        GESTURES[gesture], action, keycode, modifiers, amount,
        duration_ms & 0xFF, (duration_ms >> 8) & 0xFF, 0
    ])
    return packet, target


def parse_response(request, target, response):
    if len(response) != 12:
        raise RuntimeError("Unexpected configuration response length")
    if (response[0] != VERSION or response[1] != request[1] or
        response[2] != request[2] or response[3] != TARGETS[target] or
        response[4] != request[4]):
        raise RuntimeError("Configuration response mismatch")

    action_name = next(
        (name for name, code in ACTIONS.items() if code == response[6]),
        "UNKNOWN"
    )
    result = {
        "target": target,
        "gesture": next((name for name, code in GESTURES.items()
                         if code == request[4]), "ALL"),
        "status": STATUS.get(response[5], "UNKNOWN"),
        "action": action_name,
        "key": keyname_for(response[7]),
        "modifiers": [name for name, mask in MODIFIERS.items()
                      if response[8] & mask],
        "amount": response[9],
        "duration_ms": response[10] | (response[11] << 8),
    }
    if response[5] != 0:
        raise RuntimeError(f"Receiver rejected configuration: {result}")
    return result


async def execute_tool_call(call):
    """Dispatch a named call to the existing validated BLE configuration API."""
    if (
        not isinstance(call, dict)
        or set(call) != {"name", "arguments"}
        or not isinstance(call["arguments"], dict)
    ):
        raise ValueError("Tool call requires name and arguments")

    name = call["name"]
    args = call["arguments"]

    # Named batch uses the already-tested one-connection SET/GET executor.
    if name == "configure_gesture_batch":
        return await execute_batch(args)

    # A single SET gets identical readback verification to a batch.
    # Return the original single-mapping format for compatibility.
    if name == "set_gesture_mapping":
        batch = {
            "target": args.get("target"),
            "mappings": [
                {key: value for key, value in args.items() if key != "target"}
            ],
        }
        result = await execute_batch(batch)
        if not result["success"]:
            raise RuntimeError(
                result.get("error", "SET could not be verified")
            )
        return result["results"][0]["mapping"]

    # GET and RESET use their established protocol operations.
    packet, target = build_packet(call)

    from bleak import BleakClient, BleakScanner

    device = await BleakScanner.find_device_by_name(
        DEVICES[target], timeout=12.0
    )
    if device is None:
        raise ConnectionError(
            f"{DEVICES[target]} not found. Check receiver advertising, "
            "power, and available connection slots."
        )

    async with BleakClient(device, timeout=15.0) as client:
        info = bytes(await client.read_gatt_char(UUID_INFO))
        if len(info) != 4 or info[0] != VERSION or info[1] != TARGETS[target]:
            raise RuntimeError("Receiver protocol version or target mismatch")
        if packet[4] > info[2]:
            raise RuntimeError(
                f"Receiver supports gesture IDs 1-{info[2]}, "
                f"but requested gesture ID {packet[4]}. Update receiver firmware."
            )

        await client.write_gatt_char(UUID_COMMAND, packet, response=True)
        response = bytes(await client.read_gatt_char(UUID_RESPONSE))
        return parse_response(packet, target, response)


def build_batch(data):
    """Prevalidate 1-7 SET mappings, with a readback GET for each."""
    if not isinstance(data, dict) or set(data) != {"target", "mappings"}:
        raise ValueError("Batch requires exactly target and mappings")

    target = data["target"]
    mappings = data["mappings"]
    if not isinstance(target, str) or target not in TARGETS:
        raise ValueError(f"Unknown target: {target}")
    if not isinstance(mappings, list) or not 1 <= len(mappings) <= 7:
        raise ValueError("Batch must contain 1 to 7 mappings")

    seen = set()
    requests = []
    for index, mapping in enumerate(mappings, start=1):
        if not isinstance(mapping, dict) or "target" in mapping:
            raise ValueError(f"Mapping {index} must be an object without target")

        gesture = mapping.get("gesture")
        if not isinstance(gesture, str) or gesture not in GESTURES or gesture == "ALL":
            raise ValueError(f"Mapping {index}: invalid gesture {gesture}")
        if gesture in seen:
            raise ValueError(f"Duplicate gesture: {gesture}")
        seen.add(gesture)

        # Reuse the same checked encoder used by single SET commands.
        set_call = {
            "name": "set_gesture_mapping",
            "arguments": {"target": target, **mapping},
        }
        set_packet, _ = build_packet(set_call)
        get_packet, _ = build_packet({
            "name": "get_gesture_mapping",
            "arguments": {"target": target, "gesture": gesture},
        })
        requests.append({
            "gesture": gesture,
            "set_packet": set_packet,
            "get_packet": get_packet,
        })

    return target, requests


async def execute_batch(data):
    """Apply a batch via one BLE connection; verify each mapping with GET."""
    from bleak import BleakClient, BleakScanner

    target, requests = build_batch(data)  # all-or-nothing validation
    result = {
        "target": target,
        "device": DEVICES[target],
        "requested": len(requests),
        "verified": 0,
        "success": False,
        "results": [],
    }

    try:
        device = await BleakScanner.find_device_by_name(
            DEVICES[target], timeout=12.0
        )
        if device is None:
            raise ConnectionError(
                f"{DEVICES[target]} not found. Check receiver power, "
                "advertising, and available BLE connection slots."
            )

        async with BleakClient(device, timeout=15.0) as client:
            info = bytes(await client.read_gatt_char(UUID_INFO))
            if (
                len(info) != 4
                or info[0] != VERSION
                or info[1] != TARGETS[target]
            ):
                raise RuntimeError("Receiver protocol version or target mismatch")

            required_gesture_id = max(
                entry["set_packet"][4] for entry in requests
            )
            if required_gesture_id > info[2]:
                raise RuntimeError(
                    f"Receiver supports gesture IDs 1-{info[2]}, "
                    f"but this batch requires gesture ID {required_gesture_id}. "
                    "Update receiver firmware."
                )

            for index, entry in enumerate(requests, start=1):
                gesture = entry["gesture"]
                set_packet = entry["set_packet"]
                get_packet = entry["get_packet"]

                try:
                    await client.write_gatt_char(
                        UUID_COMMAND, set_packet, response=True
                    )
                    set_response = bytes(
                        await client.read_gatt_char(UUID_RESPONSE)
                    )
                    parse_response(set_packet, target, set_response)

                    # Wire request bytes 5-10 encode the desired mapping;
                    # response bytes 6-11 must echo its active value.
                    expected = set_packet[5:11]
                    if set_response[6:12] != expected:
                        raise RuntimeError("SET response mapping mismatch")

                    await client.write_gatt_char(
                        UUID_COMMAND, get_packet, response=True
                    )
                    get_response = bytes(
                        await client.read_gatt_char(UUID_RESPONSE)
                    )
                    readback = parse_response(
                        get_packet, target, get_response
                    )
                    if get_response[6:12] != expected:
                        raise RuntimeError("GET readback mapping mismatch")

                    result["results"].append({
                        "gesture": gesture,
                        "status": "VERIFIED",
                        "mapping": readback,
                    })
                    result["verified"] += 1
                    print(
                        f"[{index}/{len(requests)}] {gesture}: VERIFIED",
                        flush=True,
                    )

                except Exception as exc:
                    # A failed write/readback may have changed the receiver.
                    # Do not claim a rollback or that this gesture is unchanged.
                    result["results"].append({
                        "gesture": gesture,
                        "status": "UNVERIFIED",
                        "error": str(exc),
                    })
                    result["error"] = f"Stopped at {gesture}: {exc}"
                    print(
                        f"[{index}/{len(requests)}] {gesture}: UNVERIFIED ({exc})",
                        flush=True,
                    )
                    break

    except Exception as exc:
        result.setdefault("error", str(exc))

    result["success"] = (
        result["verified"] == result["requested"]
        and "error" not in result
    )
    return result


def main():
    parser = argparse.ArgumentParser(
        description="Run GestureControl configuration commands over BLE"
    )
    parser.add_argument("json_file", type=Path)
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Validate and print packets without Bluetooth",
    )
    args = parser.parse_args()

    data = json.loads(args.json_file.read_text(encoding="utf-8"))

    # Preserve the legacy standalone batch JSON format.
    if isinstance(data, dict) and "mappings" in data and "name" not in data:
        data = {"name": "configure_gesture_batch", "arguments": data}

    is_batch = (
        isinstance(data, dict)
        and data.get("name") == "configure_gesture_batch"
    )

    if args.dry_run:
        if is_batch:
            target, requests = build_batch(data["arguments"])
            output = {
                "target": target,
                "device": DEVICES[target],
                "count": len(requests),
                "packets": [
                    {
                        "gesture": req["gesture"],
                        "set_hex": req["set_packet"].hex(" "),
                        "get_hex": req["get_packet"].hex(" "),
                    }
                    for req in requests
                ],
            }
        else:
            packet, target = build_packet(data)
            output = {
                "target": target,
                "device": DEVICES[target],
                "packet_hex": packet.hex(" "),
                "packet_bytes": list(packet),
            }
        print(json.dumps(output, indent=2))
        return

    result = asyncio.run(execute_tool_call(data))
    print(json.dumps(result, indent=2))

    if is_batch and not result["success"]:
        raise SystemExit(1)

if __name__ == "__main__":
    main()
