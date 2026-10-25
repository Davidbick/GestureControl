"""Single/batch Transformers smoke tests; encode packets without BLE."""
import json
import sys
from pathlib import Path
from transformers_backend import ACTIVATION, FunctionGemma

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'laptop' / 'configuration'))
from ble_config import build_packet, build_batch


def signature(call):
    if call['name'] == 'configure_gesture_batch':
        target, entries = build_batch(call['arguments'])
        return target, tuple(sorted((e['gesture'],e['set_packet'][2:].hex()) for e in entries))
    packet, target = build_packet(call)
    return call['name'], target, packet[2:].hex()


CASES = [
    ('Set swipe up to press W on the computer.',
     {'name':'set_gesture_mapping','arguments':{
         'target':'COMPUTER','gesture':'SWIPE_UP','action':'KEY_TAP','key':'W'}}),
    ("I'm playing Minecraft. Make swipe up toggle W, swipe left press A, and tense press Space on the computer.",
     {'name':'configure_gesture_batch','arguments':{
         'target':'COMPUTER','mappings':[
             {'gesture':'SWIPE_UP','action':'KEY_TOGGLE','key':'W'},
             {'gesture':'SWIPE_LEFT','action':'KEY_TAP','key':'A'},
             {'gesture':'TENSE','action':'KEY_TAP','key':'SPACE'},
         ]}}),
]


def main():
    model = FunctionGemma()
    tools = json.loads((ROOT/'protocol'/'functiongemma_tools.json').read_text())
    instructions = ACTIVATION + '\nFor one mapping use set_gesture_mapping. For multiple mappings on one target use one configure_gesture_batch call. Use uppercase enum values. KEY_TOGGLE toggles a held key; KEY_TAP briefly presses it.'
    passed = 0
    for prompt, expected in CASES:
        print('PROMPT:', prompt, flush=True)
        try:
            call = model.call(prompt, tools, instructions)
            print('CALL:', json.dumps(call), flush=True)
            actual = signature(call)
            if actual != signature(expected):
                raise ValueError('Valid protocol call does not match expected behavior')
            print('PASS: existing validators and expected wire payload; no BLE', flush=True)
            passed += 1
        except (ValueError, TypeError, KeyError) as exc:
            print('REJECTED:', str(exc), flush=True)
    print(f'RESULT: {passed}/{len(CASES)}', flush=True)
    return 0 if passed == len(CASES) else 1


if __name__ == '__main__':
    raise SystemExit(main())
