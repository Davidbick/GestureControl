"""Create the frozen, hand-authored out-of-template FunctionGemma challenge set.

No network, no GPU, no BLE. The 49 positive cases span every supported
(target, gesture, action) combination once. 12 negative cases are not
representable by the single-map tool or omit mandatory information.
"""
from __future__ import annotations

import json
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Each gesture has 3 COMPUTER tests (tap, toggle, none) and 4 LIGHT tests
# (on, off, toggle, none). All sentences were drafted independently of the
# templated v3 generator. The labels are explicit and reviewed; they are NOT
# training examples and must not be used for tuning this checkpoint.
CASES = [
    # SWIPE_UP
    ("On MyoPC, a quick upwards sweep should type the letter W one time.", "COMPUTER", "SWIPE_UP", "KEY_TAP", "W"),
    ("In a game on my PC, moving my wrist sharply toward the ceiling should latch the W key, and repeating it should let go.", "COMPUTER", "SWIPE_UP", "KEY_TOGGLE", "W"),
    ("Cancel whatever my upward hand sweep does on the computer.", "COMPUTER", "SWIPE_UP", "NONE", None),
    ("A short upward wrist sweep needs to illuminate my MyoLight lamp.", "LIGHT", "SWIPE_UP", "LIGHT_ON", None),
    ("On the smart lamp, a swift motion of my hand toward the ceiling should extinguish it.", "LIGHT", "SWIPE_UP", "LIGHT_OFF", None),
    ("I'd like the MyoLight to alternate between lit and unlit each time my hand sweeps upward.", "LIGHT", "SWIPE_UP", "LIGHT_TOGGLE", None),
    ("For MyoLight, unassign the command linked to sweeping my wrist upward.", "LIGHT", "SWIPE_UP", "NONE", None),
    # SWIPE_DOWN
    ("Make a fast downward sweep of my hand send a single S keystroke to MyoPC.", "COMPUTER", "SWIPE_DOWN", "KEY_TAP", "S"),
    ("For my laptop, pulling my hand downward should alternate between holding S and releasing S.", "COMPUTER", "SWIPE_DOWN", "KEY_TOGGLE", "S"),
    ("I don't want my downward hand sweep controlling any computer key anymore.", "COMPUTER", "SWIPE_DOWN", "NONE", None),
    ("The lights should come alive when my hand moves rapidly toward the floor in MyoLight mode.", "LIGHT", "SWIPE_DOWN", "LIGHT_ON", None),
    ("With MyoLight selected, a downward sweep should kill the room lamp.", "LIGHT", "SWIPE_DOWN", "LIGHT_OFF", None),
    ("Use a short downward hand motion as the on-off switch for MyoLight.", "LIGHT", "SWIPE_DOWN", "LIGHT_TOGGLE", None),
    ("Remove the MyoLight binding for a downward sweep; it shouldn't trigger an action.", "LIGHT", "SWIPE_DOWN", "NONE", None),
    # SWIPE_LEFT
    ("When I wave my hand quickly toward my left, put one A into the computer.", "COMPUTER", "SWIPE_LEFT", "KEY_TAP", "A"),
    ("A quick sweep toward the left on my gaming PC should lock the A key down until I repeat the motion.", "COMPUTER", "SWIPE_LEFT", "KEY_TOGGLE", "A"),
    ("For the laptop, detach the action associated with sweeping my hand to the left.", "COMPUTER", "SWIPE_LEFT", "NONE", None),
    ("Illuminate the smart lamp whenever my hand makes a sharp motion toward the left.", "LIGHT", "SWIPE_LEFT", "LIGHT_ON", None),
    ("I'd like moving my hand quickly toward my left to black out MyoLight.", "LIGHT", "SWIPE_LEFT", "LIGHT_OFF", None),
    ("Make a left-going hand flick act like pressing the MyoLight power switch each time.", "LIGHT", "SWIPE_LEFT", "LIGHT_TOGGLE", None),
    ("Clear the light's command for the leftward hand movement.", "LIGHT", "SWIPE_LEFT", "NONE", None),
    # SWIPE_RIGHT
    ("A little hand sweep toward my right should give my keyboard one D keypress.", "COMPUTER", "SWIPE_RIGHT", "KEY_TAP", "D"),
    ("On MyoPC, let a quick rightward motion pin D down; doing it again should release D.", "COMPUTER", "SWIPE_RIGHT", "KEY_TOGGLE", "D"),
    ("Please erase the computer shortcut that's triggered by my rightward sweep.", "COMPUTER", "SWIPE_RIGHT", "NONE", None),
    ("A sharp motion of my wrist toward the right should light up my desk lamp in MyoLight.", "LIGHT", "SWIPE_RIGHT", "LIGHT_ON", None),
    ("For the room lamp, hand sweeping to my right should put it into the off state.", "LIGHT", "SWIPE_RIGHT", "LIGHT_OFF", None),
    ("Every right-going swipe should invert whether my smart light has power.", "LIGHT", "SWIPE_RIGHT", "LIGHT_TOGGLE", None),
    ("Take away the MyoLight command for the sideways motion toward my right.", "LIGHT", "SWIPE_RIGHT", "NONE", None),
    # TENSE: full forearm contraction, not wrist curl or wrist extension
    ("When I stiffen all the muscles in my forearm together, type Q once on MyoPC.", "COMPUTER", "TENSE", "KEY_TAP", "Q"),
    ("For my gaming laptop, fully tightening the whole forearm should keep R pressed until my next full-forearm tighten.", "COMPUTER", "TENSE", "KEY_TOGGLE", "R"),
    ("Deactivate the MyoPC shortcut for contracting all my forearm muscles at once.", "COMPUTER", "TENSE", "NONE", None),
    ("The lamp should switch on when I deliberately tense my entire forearm together.", "LIGHT", "TENSE", "LIGHT_ON", None),
    ("If I make all the muscles of my forearm go rigid simultaneously, switch off MyoLight.", "LIGHT", "TENSE", "LIGHT_OFF", None),
    ("Let a whole-forearm squeeze reverse the on/off state of the smart lamp.", "LIGHT", "TENSE", "LIGHT_TOGGLE", None),
    ("For MyoLight, disconnect the full forearm muscle-tensing command.", "LIGHT", "TENSE", "NONE", None),
    # FLEX: fist and wrist bent inward, like a wrist curl
    ("On the computer, making a fist while bending my wrist inward should hit Space one time.", "COMPUTER", "FLEX", "KEY_TAP", "SPACE"),
    ("Curling my wrist toward my inner forearm as I close my fist should switch the Enter key between held and released on MyoPC.", "COMPUTER", "FLEX", "KEY_TOGGLE", "ENTER"),
    ("Remove the MyoPC control activated by closing my fist and curling my wrist in.", "COMPUTER", "FLEX", "NONE", None),
    ("As I curl my wrist inward into a fist, make the MyoLight lamp glow.", "LIGHT", "FLEX", "LIGHT_ON", None),
    ("Closing my hand tightly and bending it in toward the inside of my arm should shut off the smart light.", "LIGHT", "FLEX", "LIGHT_OFF", None),
    ("Make that inward wrist curl with a clenched fist flip the lamp power state.", "LIGHT", "FLEX", "LIGHT_TOGGLE", None),
    ("Stop the room lamp from doing anything when I make a fist and bend my wrist inward.", "LIGHT", "FLEX", "NONE", None),
    # EXTEND: wrist bent backward rather than simply spreading fingers
    ("When I bend my wrist backward, lifting my palm toward my forearm, press Tab once on my laptop.", "COMPUTER", "EXTEND", "KEY_TAP", "TAB"),
    ("On MyoPC, arching my hand back at the wrist should latch Space and unlatch it the next time.", "COMPUTER", "EXTEND", "KEY_TOGGLE", "SPACE"),
    ("Delete the computer binding for bending my hand sharply back at the wrist.", "COMPUTER", "EXTEND", "NONE", None),
    ("When my palm tilts backward almost perpendicular to my arm, light up MyoLight.", "LIGHT", "EXTEND", "LIGHT_ON", None),
    ("Shut down the desk light if I pull my wrist backward into an extension.", "LIGHT", "EXTEND", "LIGHT_OFF", None),
    ("For MyoLight, treat tilting my wrist way back as a light-switch press.", "LIGHT", "EXTEND", "LIGHT_TOGGLE", None),
    ("Unmap the lamp command that's connected to cocking my wrist backward.", "LIGHT", "EXTEND", "NONE", None),
]

# These requests should not produce an automatically executable mapping.
# The single-map schema does not expose a 'cannot do that' tool. A malformed
# output is NOT counted as a correct refusal; only an explicit abstention is.
NEGATIVES = [
    ("Make a left swipe perform something cool.", "missing_action"),
    ("Please change what happens on MyoPC when I gesture.", "missing_gesture_and_action"),
    ("Whenever I swipe up, have it press W.", "missing_target"),
    ("Make the lamp turn on when I gesture.", "missing_gesture"),
    ("For MyoPC, set an upward swipe to both tap A and hold D.", "multiple_actions"),
    ("For MyoLight, a swipe left should switch the lamp on and switch it off at the same time.", "contradictory_actions"),
    ("When I snap my fingers, make the light turn on.", "unsupported_gesture"),
    ("Have my computer rotate the screen when I tense my forearm.", "unsupported_action"),
    ("Set the lamp to purple when I sweep my hand right.", "unsupported_light_color"),
    ("For MyoLight, dim the lamp to 30 percent when I bend my wrist back.", "unsupported_brightness_in_simplified_tool"),
    ("If I make a fist, please open Minecraft on the computer.", "unsupported_app_launch"),
    ("Make a leftward swipe turn the light on and a rightward swipe turn it off, both in one command.", "requires_two_calls"),
]


def build():
    rows = []
    for i, (prompt, target, gesture, action, key) in enumerate(CASES, 1):
        args = {"target": target, "gesture": gesture, "action": action}
        if key is not None:
            args["key"] = key
        rows.append({"id": f"P{i:02d}", "kind": "mapping", "prompt": prompt,
                     "expected": {"name": "map_gesture", "arguments": args}})
    for i, (prompt, reason) in enumerate(NEGATIVES, 1):
        rows.append({"id": f"N{i:02d}", "kind": "unsupported", "prompt": prompt,
                     "reason": reason})
    return rows


def main():
    data = build()
    assert len(data) == 61
    seen = set()
    combinations = set()
    for row in data:
        p = row["prompt"].strip().casefold()
        assert p not in seen, f"Repeated prompt: {p}"
        seen.add(p)
        if row["kind"] == "mapping":
            a = row["expected"]["arguments"]
            combo = a["target"], a["gesture"], a["action"]
            assert combo not in combinations, f"Repeated combination: {combo}"
            combinations.add(combo)
    assert len(combinations) == 49
    out = HERE / "challenge_v1.json"
    out.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
    print(f"Created {out}: 49 valid commands, 12 unsupported/ambiguous requests")


if __name__ == "__main__":
    main()
