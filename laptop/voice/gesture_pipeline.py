
"""Local FunctionGemma -> validated batch configuration for the live demo.

No BLE writes occur in this module. Special-case game phrases are deliberately
normalized for the demo because the trained single-map tool has no mouse actions.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'laptop' / 'functiongemma'))
sys.path.insert(0, str(ROOT / 'laptop' / 'configuration'))

from ble_config import build_batch
from train_single import TOOLS, expected_call, generate, signature
from transformers_backend import parse_call

DEFAULT_MODEL = ROOT / 'laptop/functiongemma/training_runs/natural-v3/model'

# Match gestures, not action phrases such as "look left".
GESTURE_RE = re.compile(
    r"\b(?:"
    r"(?:(?:swip(?:e|ing)|flick(?:ing)?|flap(?:ping)?|flip(?:ping)?)"
    r"\s+(?:my\s+hand\s+)?"
    r"(?P<after>up(?:wards?)?|down(?:wards?)?|left|right))"
    r"|(?P<before>up(?:wards?)?|down(?:wards?)?|left|right)"
    r"\s+(?:swipe|flick)"
    r"|(?P<muscle>tense|tensing|flex|flexing|extend|extending)"
    r")\b",
    re.I,
)

DIRECTIONS = {
    'up': 'SWIPE_UP',
    'upward': 'SWIPE_UP',
    'upwards': 'SWIPE_UP',
    'down': 'SWIPE_DOWN',
    'downward': 'SWIPE_DOWN',
    'downwards': 'SWIPE_DOWN',
    'left': 'SWIPE_LEFT',
    'right': 'SWIPE_RIGHT',
}

MUSCLES = {
    'tense': 'TENSE',
    'tensing': 'TENSE',
    'flex': 'FLEX',
    'flexing': 'FLEX',
    'extend': 'EXTEND',
    'extending': 'EXTEND',
}


def split_gestures(command):
    matches = list(GESTURE_RE.finditer(command))
    result = []

    for i, match in enumerate(matches):
        direction = match.group('after') or match.group('before')

        if direction:
            gesture = DIRECTIONS[direction.lower()]
        else:
            gesture = MUSCLES[match.group('muscle').lower()]

        end = (
            matches[i + 1].start()
            if i + 1 < len(matches)
            else len(command)
        )

        clause = command[match.start():end].strip(' ,.;:!?\n')
        result.append((gesture, clause))

    return result



def _key(value):
    return (
        'SPACE'
        if value.upper() in ('SPACE', 'SPACEBAR')
        else value.upper()
    )


def intent_for_clause(gesture, clause):
    """Recognize familiar demo actions. Return (action, key) or None."""
    s = clause.lower()

    if re.search(r'\bscroll\s+up\b', s):
        return 'MOUSE_SCROLL_UP', None

    if re.search(r'\bscroll\s+down\b', s):
        return 'MOUSE_SCROLL_DOWN', None

    # Prevent accidentally combining two actions when speech
    # recognition misses a gesture boundary.
    forward = re.search(
        r'\b(?:move|walk|go|run)\s+forward\b', s
    )
    look = re.search(
        r'\b(?:look|turn|rotate|aim)\s+'
        r'(?:the\s+camera\s+)?(?:left|right)\b', s
    )

    if forward and look:
        raise ValueError(
            'Conflicting actions in one gesture clause: '
            + clause
        )

    # Keyboard arrow keys are different from mouse movement.
    arrow = re.search(
        r'\b(?:press|tap|hit|to)\s+(?:the\s+)?'
        r'(up|down|left|right)\s+arrow(?:\s+key)?\b',
        s,
    )

    if arrow:
        return 'KEY_TAP', arrow.group(1).upper() + '_ARROW'

    if re.search(
        r'\b(?:look|turn|rotate|aim)\s+'
        r'(?:the\s+camera\s+)?left\b'
        r'|\b(?:camera|mouse)\s+left\b',
        s,
    ):
        return 'MOUSE_MOVE_LEFT', None

    if re.search(
        r'\b(?:look|turn|rotate|aim)\s+'
        r'(?:the\s+camera\s+)?right\b'
        r'|\b(?:camera|mouse)\s+right\b',
        s,
    ):
        return 'MOUSE_MOVE_RIGHT', None

    if re.search(r'\b(?:move|walk|go|run)\s+forward\b', s):
        return 'KEY_TOGGLE', 'W'

    if re.search(r'\b(?:move|walk|go|run)\s+back(?:wards)?\b', s):
        return 'KEY_TOGGLE', 'S'

    if re.search(r'\bjump\b', s):
        return 'KEY_TAP', 'SPACE'

    hold = re.search(
        r'\b(?:hold|toggle(?:\s+holding)?|keep\s+holding)'
        r'\s+(?:down\s+)?(?:the\s+)?'
        r'([a-z0-9]+|space(?:bar)?)\b',
        s,
    )
    if hold:
        return 'KEY_TOGGLE', _key(hold.group(1))

    tap = re.search(
        r'\b(?:press|tap|hit)\s+(?:the\s+)?'
        r'([a-z0-9]+|space(?:bar)?)\b',
        s,
    )
    if tap:
        return 'KEY_TAP', _key(tap.group(1))

    if re.search(
        r'\b(?:turn|switch)\s+(?:the\s+)?'
        r'(?:lamp|light)?\s*on\b',
        s,
    ):
        return 'LIGHT_ON', None

    if re.search(
        r'\b(?:turn|switch)\s+(?:the\s+)?'
        r'(?:lamp|light)?\s*off\b',
        s,
    ):
        return 'LIGHT_OFF', None

    if re.search(
        r'\btoggle\s+(?:the\s+)?(?:lamp|light)\b',
        s,
    ):
        return 'LIGHT_TOGGLE', None

    return None


def split_requests(command):
    """Split likely separate instructions without requiring known actions.

    Never split 'swipe up and scroll up' at 'and scroll':
    that is one gesture/action pair.
    """
    pieces = re.split(
        r'\b(?:and|then)\s+(?='
        r'(?:swipe|swiping|flick|flap|swap|striped|skirt|'
        r'wave|tense|flex|extend|move\s+(?:my\s+)?hand)\b)',
        command,
        flags=re.I,
    )

    requests = []

    for piece in pieces:
        piece = piece.strip(" ,.;:\n")
        if not piece:
            continue

        recognized = split_gestures(piece)

        if recognized:
            requests.extend(recognized)
        else:
            # No familiar gesture pattern? Let Gemma interpret it.
            requests.append((None, piece))

    return requests

def _target(command):
    if re.search(r'\b(?:myolight|lamp|light)\b', command, re.I):
        return 'LIGHT'
    return 'COMPUTER'


def _model_phrase(target, gesture, action, key):
    names = {
        'SWIPE_UP': 'swipe up',
        'SWIPE_DOWN': 'swipe down',
        'SWIPE_LEFT': 'swipe left',
        'SWIPE_RIGHT': 'swipe right',
        'TENSE': 'tense',
        'FLEX': 'flex',
        'EXTEND': 'extend',
    }

    g = names[gesture]
    device = 'MyoLight' if target == 'LIGHT' else 'MyoPC'

    if action == 'KEY_TOGGLE':
        return f'On {device}, make {g} toggle holding {key}.'

    if action == 'KEY_TAP':
        return f'On {device}, make {g} press {key}.'

    return (
        f'On {device}, make {g} '
        f'{action.lower().replace("_", " ")}.'
    )


class GesturePipeline:
    def __init__(self, model_path=DEFAULT_MODEL):
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer

        model_path = Path(model_path)

        if not (model_path / 'config.json').is_file():
            raise FileNotFoundError(
                f'Model checkpoint missing: {model_path}'
            )

        print(
            '[Gemma] Loading local checkpoint:',
            model_path,
            flush=True,
        )

        self.tokenizer = AutoTokenizer.from_pretrained(
            str(model_path),
            local_files_only=True,
        )

        self.model = AutoModelForCausalLM.from_pretrained(
            str(model_path),
            local_files_only=True,
            dtype=torch.float32,
            attn_implementation='eager',
        ).eval()

        print('[Gemma] Ready (CPU, offline)', flush=True)

    def _infer(self, prompt):
        raw, complete, _ = generate(
            self.model,
            self.tokenizer,
            prompt,
        )

        if not complete:
            raise ValueError(
                'FunctionGemma generation did not finish'
            )

        return parse_call(raw)

    
    def prepare(self, command):
        """Interpret spoken requests; validate before any BLE operation."""
        target = _target(command)
        requests = split_requests(command)

        if not requests:
            raise ValueError("No instructions received")

        if len(requests) > 7:
            raise ValueError("Maximum 7 mappings per request")

        mappings = []

        for recognized_gesture, clause in requests:
            clause = re.sub(
                r'\s+(?:and|then)\s*$',
                '',
                clause.strip(),
                flags=re.I,
            )

            if not clause:
                continue

            # First try the existing simple interpreter.
            intent = None

            if recognized_gesture is not None:
                try:
                    intent = intent_for_clause(
                        recognized_gesture, clause
                    )
                except ValueError:
                    # Even a phrase Python finds confusing gets
                    # a chance to be interpreted by FunctionGemma.
                    intent = None

            mapping = None

            if intent is not None:
                action, key = intent

                candidate = {
                    "gesture": recognized_gesture,
                    "action": action,
                }

                if key is not None:
                    candidate["key"] = key

                try:
                    # Confirm rule-based result is executable.
                    build_batch({
                        "target": target,
                        "mappings": [candidate],
                    })
                    mapping = candidate

                    print(
                        f"[Parser] {recognized_gesture} -> "
                        f"{action} {key or ''}",
                        flush=True,
                    )
                except (ValueError, TypeError, KeyError):
                    # Invalid rule result? Try the model instead.
                    mapping = None

            if mapping is None:
                # FunctionGemma receives the actual instruction,
                # not a mapping Python has already decided.
                device = (
                    "MyoLight"
                    if target == "LIGHT"
                    else "MyoPC"
                )

                prompt = f"On {device}, {clause}"

                print(
                    f"[Gemma] Interpreting: {prompt}",
                    flush=True,
                )

                call = self._infer(prompt)

                # Strictly validate the model-generated tool call.
                signature(call)

                args = call["arguments"]

                if args["target"] != target:
                    raise ValueError(
                        "FunctionGemma selected the wrong device: "
                        + str(args["target"])
                    )

                if (
                    recognized_gesture is not None
                    and args["gesture"] != recognized_gesture
                ):
                    raise ValueError(
                        "FunctionGemma gesture mismatch: "
                        f"expected {recognized_gesture}, "
                        f"received {args['gesture']}"
                    )

                mapping = {
                    k: v
                    for k, v in args.items()
                    if k != "target"
                }

                print(
                    f"[Gemma] Interpreted: {mapping}",
                    flush=True,
                )

            mappings.append(mapping)

        batch = {
            "target": target,
            "mappings": mappings,
        }

        # Validate the entire set, including duplicates and
        # all existing receiver restrictions.
        build_batch(batch)

        return batch
