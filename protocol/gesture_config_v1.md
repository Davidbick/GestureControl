# GestureControl configuration API v1

The model generates function calls described by `protocol/functiongemma_tools.json`. The host validates calls and writes **binary** commands to the appropriate receiver, not JSON. The FunctionGemma model is never run on either receiver.

## Devices and roles

- `COMPUTER` (target 1): nRF receiver advertising as `MyoPC`; has keyboard and mouse USB HID interfaces.
- `LIGHT` (target 2): Wio light receiver advertising as `MyoLight`; has LED PWM.
- Host (laptop or Pi) connects directly to the target receiver over BLE. The Wio trackball controller is not a configuration relay.
- Receivers support two concurrent BLE connections: one gesture controller and one configuration client. Configuration does not reset an active gesture session.

## GATT characteristics

Use the existing receiver service `89ca2ebf-7c18-4a33-9877-df55001abcde`.

| Characteristic | UUID | Access |
| --- | --- | --- |
| Command | `89ca2ebf-7c18-4a33-9877-df55001abce4` | Write with response |
| Response | `89ca2ebf-7c18-4a33-9877-df55001abce5` | Read |
| Info | `89ca2ebf-7c18-4a33-9877-df55001abce6` | Read |

Info is four bytes: `[version=1, target, gesture_count=7, capabilities]`. Capability bits: keyboard=1, mouse=2, light=4.

## Wire format

All messages are exactly 12 bytes. No extra report IDs or model tokens. Multibyte durations are little endian.

Request byte positions:

`[version, tx_id, operation, target, gesture, action, keycode, modifiers, amount, duration_lo, duration_hi, flags]`

Response:

`[version, tx_id, operation, target, gesture, status, action, keycode, modifiers, amount, duration_lo, duration_hi]`

Operations: GET=1, SET=2, RESET=3, RESET_ALL=4. For RESET_ALL, gesture=0. Other operations use gesture=1..7. All fields except version, tx_id, operation and target must be zero for RESET_ALL; GET and RESET likewise require empty action parameters.

Gestures: SWIPE_UP=1, SWIPE_DOWN=2, SWIPE_LEFT=3, SWIPE_RIGHT=4, TENSE=5 (full-arm tense), EXTEND=6 (EMG0-dominant flex), FLEX=7 (EMG1-dominant flex).

Actions: NONE=0, KEY_TAP=1, MOUSE_LEFT_CLICK=2, MOUSE_RIGHT_CLICK=3, MOUSE_MIDDLE_CLICK=4, MOUSE_MOVE_UP=5, MOUSE_MOVE_DOWN=6, MOUSE_MOVE_LEFT=7, MOUSE_MOVE_RIGHT=8, MOUSE_SCROLL_UP=9, MOUSE_SCROLL_DOWN=10, LIGHT_TOGGLE=11, LIGHT_ON=12, LIGHT_OFF=13, LIGHT_SET_BRIGHTNESS=14, LIGHT_BRIGHTNESS_UP=15, LIGHT_BRIGHTNESS_DOWN=16, MOUSE_DOUBLE_CLICK=17; KEY_TOGGLE=18.

Modifier bits: CTRL=1, SHIFT=2, ALT=4, GUI=8. Keys are USB HID usage IDs, not ASCII. Defaults: tap/click 35 ms; movement 40; scroll 1; brightness step 25. Duration valid 30..2000 ms; mouse movement 1..127; scrolling 1..10; light brightness set 0..100; brightness step 1..100. Unsupported action/target combinations are rejected.

Statuses: OK=0, BAD_VERSION=1, WRONG_TARGET=2, INVALID_GESTURE=3, UNSUPPORTED_ACTION=4, INVALID_PARAMETER=5, BUSY=6, INVALID_OPERATION=7.

After writing the command characteristic, read Response and verify version, transaction ID, operation, target, gesture and success status. GET returns the current mapping; SET returns the newly active mapping; RESET returns defaults; RESET_ALL restores all mappings and does not echo an individual mapping.

## Defaults

| Gesture | Computer | Light |
| --- | --- | --- |
| Up | KEY_TAP UP_ARROW | LIGHT_BRIGHTNESS_UP 25 |
| Down | KEY_TAP DOWN_ARROW | LIGHT_BRIGHTNESS_DOWN 25 |
| Left | KEY_TAP LEFT_ARROW | NONE |
| Right | KEY_TAP RIGHT_ARROW | NONE |
| TENSE | KEY_TAP ENTER | LIGHT_TOGGLE |
| EXTEND | NONE | NONE |
| FLEX | NONE | NONE |

KEY_TOGGLE is a computer-only key action using keycode and modifiers with zero duration. The same gesture toggles its key down/up; up to seven toggles can be assigned (the keyboard HID report supports up to six distinct non-modifier keys held simultaneously). KEY_TAP preserves active toggled keys. The HID worker releases all held keys on controller disconnect, USB disconnect, or SET/RESET operations (including RESET_ALL).

Both additional gestures can be configured on each receiver but are not produced by the current wearable. The gesture event remains two bytes, and the configuration packet remains 12 bytes at protocol version 1. Receivers with older firmware advertise only five gestures in Info, allowing Python to reject unsupported commands before writing.

Mappings live in **RAM only**: they survive BLE disconnects and Wio mode switches; power cycling the receiver restores defaults. `reset_gesture_mapping` explicitly restores one or all mappings. This does not reset physical light power state.

The current demo GATT permission is unauthenticated. Pairing and authorization are required before deployment outside supervised testing.
