"""Reproducibly generate natural-language FunctionGemma single-mapping data.

245 balanced training examples (five per gesture/action combination) and
49 held-out utterances (one per combination). Held-out sentence templates
are never used in training. Both sets are validated against the existing
GestureControl BLE packet encoder; this program never connects to BLE.

Run: python laptop/functiongemma/generate_natural_data.py
"""
import argparse
import json
import random
from collections import Counter
from pathlib import Path

from train_single import expected_call, signature

HERE = Path(__file__).resolve().parent
GESTURES = (
    "SWIPE_UP", "SWIPE_DOWN", "SWIPE_LEFT", "SWIPE_RIGHT",
    "TENSE", "EXTEND", "FLEX",
)
ACTIONS = {
    "COMPUTER": ("KEY_TAP", "KEY_TOGGLE", "NONE"),
    "LIGHT": ("LIGHT_ON", "LIGHT_OFF", "LIGHT_TOGGLE", "NONE"),
}
KEYS = ("W", "A", "S", "D", "Q", "E", "R", "SPACE", "ENTER", "TAB")

# All forms must fit after "I"; the corresponding noun forms fit after "make".
GESTURE_WORDS = {
    "SWIPE_UP": (
        ("swipe up", "swiping up"),
        ("swipe upward", "an upward swipe"),
        ("flick up", "flicking up"),
        ("move my hand up", "moving my hand up"),
        ("swipe my hand upward", "a swipe upward"),
        ("flick my hand upwards", "an upward hand flick"),
    ),
    "SWIPE_DOWN": (
        ("swipe down", "swiping down"),
        ("swipe downward", "a downward swipe"),
        ("flick down", "flicking down"),
        ("move my hand down", "moving my hand down"),
        ("swipe my hand downward", "a swipe downward"),
        ("flick my hand downwards", "a downward hand flick"),
    ),
    "SWIPE_LEFT": (
        ("swipe left", "swiping left"),
        ("swipe to the left", "a leftward swipe"),
        ("flick left", "flicking left"),
        ("move my hand left", "moving my hand left"),
        ("swipe my hand leftward", "a swipe to the left"),
        ("flick my hand left", "a leftward hand flick"),
    ),
    "SWIPE_RIGHT": (
        ("swipe right", "swiping right"),
        ("swipe to the right", "a rightward swipe"),
        ("flick right", "flicking right"),
        ("move my hand right", "moving my hand right"),
        ("swipe my hand rightward", "a swipe to the right"),
        ("flick my hand right", "a rightward hand flick"),
    ),
    "TENSE": (
        ("tense my arm", "tensing my arm"),
        ("tense up my arm", "an arm tense"),
        ("tighten my arm muscles", "tightening my arm muscles"),
        ("tense my whole arm", "a full-arm tense"),
        ("do a tense gesture", "the tense gesture"),
        ("contract my whole arm", "a full-arm muscle tense"),
    ),
    "EXTEND": (
        ("extend my arm", "extending my arm"),
        ("extend", "an extend gesture"),
        ("do the extend gesture", "the extend gesture"),
        ("activate extend", "activating extend"),
        ("extend my hand", "extending my hand"),
        ("perform an extend gesture", "an arm extension"),
    ),
    "FLEX": (
        ("flex my arm", "flexing my arm"),
        ("flex", "a flex gesture"),
        ("do the flex gesture", "the flex gesture"),
        ("activate flex", "activating flex"),
        ("flex my muscles", "flexing my muscles"),
        ("perform a flex gesture", "an arm flex"),
    ),
}

DEVICE_WORDS = {
    "COMPUTER": (
        "MyoPC", "my laptop", "my computer", "my PC", "my keyboard",
        "the laptop",
    ),
    "LIGHT": (
        "MyoLight", "my lamp", "my room light", "my smart light", "the lamp",
        "MyoLight mode",
    ),
}


def key_word(key, variant):
    if key == "SPACE":
        return "spacebar" if variant % 2 else "Space"
    if key == "ENTER":
        return "Enter"
    if key == "TAB":
        return "Tab"
    return key


def action_words(action, key, variant):
    """Imperative verb phrase following 'to' or 'should'."""
    if action == "KEY_TAP":
        name = key_word(key, variant)
        return (
            f"press {name}", f"tap the {name} key", f"hit {name} once",
            f"press the {name} key", f"send one {name} keypress",
            f"tap {name}",
        )[variant]
    if action == "KEY_TOGGLE":
        name = key_word(key, variant)
        return (
            f"toggle holding {name}", f"start or stop holding {name}",
            f"switch the {name} key between held and released",
            f"toggle the hold on {name}", f"hold or release {name}",
            f"toggle whether {name} is held down",
        )[variant]
    clauses = {
        "LIGHT_ON": (
            "turn the light on", "switch the lamp on", "turn on the lights",
            "power on the lamp", "switch the light on", "turn on the lamp",
        ),
        "LIGHT_OFF": (
            "turn the light off", "switch the lamp off", "turn off the lights",
            "power off the lamp", "switch the light off", "turn off the lamp",
        ),
        "LIGHT_TOGGLE": (
            "toggle the light", "switch the lamp on or off",
            "toggle the lamp's power", "flip the light between on and off",
            "switch the light between on and off", "toggle the lamp",
        ),
        "NONE": (
            "do nothing", "have no action", "stop triggering anything",
            "disable that gesture", "clear its assigned action",
            "leave the gesture unmapped",
        ),
    }
    return clauses[action][variant]


def render_prompt(target, gesture, action, key, variant, heldout_alternate=False):
    event, nominal = GESTURE_WORDS[gesture][variant]
    device = DEVICE_WORDS[target][variant]
    phrase = action_words(action, key, variant)
    # Five natural training constructions. Held-out constructions are not
    # used in training and employ new gesture wording (variant 5).
    if variant == 0:
        return f"With {device}, when I {event}, {phrase}."
    if variant == 1:
        return f"For {device}, make {nominal} {phrase}."
    if variant == 2:
        return f"I'd like {nominal} to {phrase} using {device}."
    if variant == 3:
        return f"Can {nominal} {phrase} for {device}?"
    if variant == 4:
        return f"Please have {nominal} {phrase} with {device}."
    if heldout_alternate:
        return f"Using {device}, I'd like it to {phrase} whenever I {event}."
    return f"If I {event}, {device} should {phrase}."


def build_dataset(seed=42):
    # Keep the gesture/action counts exactly balanced. Vary keyboard keys
    # without giving any one gesture a fixed letter. Fixed seed -> repeatable.
    rng = random.Random(seed)
    training, heldout = [], []
    combinations = [(target, gesture, action)
                    for target, actions in ACTIONS.items()
                    for gesture in GESTURES for action in actions]
    for combo_index, (target, gesture, action) in enumerate(combinations):
        for variant in range(6):
            key = None
            if action in ("KEY_TAP", "KEY_TOGGLE"):
                key = KEYS[(combo_index * 3 + variant * 7) % len(KEYS)]
            prompt = render_prompt(target, gesture, action, key, variant,
                                   heldout_alternate=bool(combo_index % 2))
            call = expected_call(target, gesture, action, key)
            signature(call)  # Real packet validation, before files are written.
            example = {"prompt": prompt, "call": call}
            (training if variant < 5 else heldout).append(example)
    rng.shuffle(training)
    rng.shuffle(heldout)
    assert len(training) == 245 and len(heldout) == 49
    train_prompts = {e["prompt"].casefold() for e in training}
    heldout_prompts = {e["prompt"].casefold() for e in heldout}
    assert len(train_prompts) == len(training)
    assert len(heldout_prompts) == len(heldout)
    assert train_prompts.isdisjoint(heldout_prompts)
    for entries, count in ((training, 5), (heldout, 1)):
        counts = Counter((e["call"]["arguments"]["target"],
                          e["call"]["arguments"]["gesture"],
                          e["call"]["arguments"]["action"]) for e in entries)
        assert set(counts.values()) == {count}, counts
        assert len(counts) == len(combinations)
    return training, heldout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=HERE / "natural_data")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--force", action="store_true",
                        help="Explicitly overwrite an existing generated dataset")
    args = parser.parse_args()
    train, heldout = build_dataset(args.seed)
    args.output.mkdir(parents=True, exist_ok=True)
    for filename, rows in (("train.json", train), ("heldout.json", heldout)):
        path = args.output / filename
        if path.exists() and not args.force:
            parser.error(f"{path} exists; use --force to replace it")
        path.write_text(json.dumps(rows, indent=2, ensure_ascii=False) + "\n")
    print(f"Validated {len(train)} training examples and {len(heldout)} held-out prompts")
    print(f"Files saved in {args.output}")


if __name__ == "__main__":
    main()
