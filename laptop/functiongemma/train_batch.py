"""Local SFTTrainer adaptation of Google's FunctionGemma fine-tuning notebook.

Synthetic data, strict held-out evaluation, no BLE and no Hub publishing.
"""
import argparse
import hashlib
import json
import random
import sys
from pathlib import Path

from transformers_backend import ACTIVATION, MODEL_ID, FunctionGemma, parse_call

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT/'laptop'/'configuration'))
from ble_config import build_batch

DATA = HERE/'batch_data'
BASE_REVISION = '39eccb091651513a5dfb56892d3714c1b5b8276c'


def tools():
    return [t for t in json.loads((ROOT/'protocol/functiongemma_tools.json').read_text())
            if t['function']['name'] == 'configure_gesture_batch']


def signature(call):
    if set(call) != {'name','arguments'} or call['name'] != 'configure_gesture_batch':
        raise ValueError('Only configure_gesture_batch is allowed')
    target, entries = build_batch(call['arguments'])
    return target, sorted((e['gesture'], e['set_packet'][2:].hex()) for e in entries)


def row(prompt, target, mappings):
    call = {'name':'configure_gesture_batch','arguments':{'target':target,'mappings':mappings}}
    signature(call)
    return {'prompt':prompt, 'call':call}


def describe(mapping):
    action = mapping['action']
    if action in ('KEY_TAP','KEY_TOGGLE'):
        text = ('press ' if action == 'KEY_TAP' else 'toggle holding ') + mapping['key']
        if mapping.get('modifiers'):
            text += ' with ' + ' and '.join(mapping['modifiers'])
    else:
        text = action.lower().replace('_',' ')
    if 'amount' in mapping:
        text += ' amount ' + str(mapping['amount'])
    if 'duration_ms' in mapping:
        text += ' for ' + str(mapping['duration_ms']) + ' milliseconds'
    return mapping['gesture'].lower().replace('_',' ') + ' to ' + text


def prepare():
    rng = random.Random(1729)
    gesture_names = ['SWIPE_UP','SWIPE_DOWN','SWIPE_LEFT','SWIPE_RIGHT','TENSE','EXTEND','FLEX']
    samples = []
    computer = ['KEY_TAP','KEY_TOGGLE','MOUSE_LEFT_CLICK','MOUSE_RIGHT_CLICK',
                'MOUSE_MIDDLE_CLICK','MOUSE_DOUBLE_CLICK','MOUSE_MOVE_UP','MOUSE_MOVE_DOWN',
                'MOUSE_MOVE_LEFT','MOUSE_MOVE_RIGHT','MOUSE_SCROLL_UP','MOUSE_SCROLL_DOWN','NONE']
    light = ['LIGHT_TOGGLE','LIGHT_ON','LIGHT_OFF','LIGHT_SET_BRIGHTNESS',
             'LIGHT_BRIGHTNESS_UP','LIGHT_BRIGHTNESS_DOWN','NONE']
    for index in range(96):
        target = 'LIGHT' if index % 3 == 0 else 'COMPUTER'
        count = [1,2,3,1,4,2,7,3][index % 8]
        mappings = []
        for j, gesture in enumerate(rng.sample(gesture_names,count)):
            actions = light if target == 'LIGHT' else computer
            action = actions[(index+j) % len(actions)]
            m = {'gesture':gesture,'action':action}
            if action in ('KEY_TAP','KEY_TOGGLE'):
                m['key'] = rng.choice(['W','A','S','D','SPACE','ENTER','E','Q','F1','TAB'])
                if index % 4 == 0:
                    m['modifiers'] = ['CTRL','SHIFT']
                if action == 'KEY_TAP' and index % 5 == 0:
                    m['duration_ms'] = 100
            elif action.startswith('MOUSE_MOVE'):
                m['amount'] = rng.choice([10,25,40,80])
            elif action.startswith('MOUSE_SCROLL'):
                m['amount'] = rng.choice([1,2,5])
            elif action in ('LIGHT_SET_BRIGHTNESS','LIGHT_BRIGHTNESS_UP','LIGHT_BRIGHTNESS_DOWN'):
                m['amount'] = rng.choice([10,25,50,75,100])
            elif 'CLICK' in action and index % 4 == 0:
                m['duration_ms'] = 100
            mappings.append(m)
        lead = ['Configure','Set','Map','Please assign'][index % 4]
        prompt = f'{lead} on the {target.lower()}: ' + '; '.join(map(describe,mappings)) + '.'
        samples.append(row(prompt,target,mappings))
    # Manually authored prompts use different phrasing; never used for training.
    heldout = [
        row('Set swipe up to press W on the computer.','COMPUTER',
            [{'gesture':'SWIPE_UP','action':'KEY_TAP','key':'W'}]),
        row("I'm playing Minecraft. Make swipe up toggle W, swipe left press A, and tense press Space on the computer.",'COMPUTER',
            [{'gesture':'SWIPE_UP','action':'KEY_TOGGLE','key':'W'},
             {'gesture':'SWIPE_LEFT','action':'KEY_TAP','key':'A'},
             {'gesture':'TENSE','action':'KEY_TAP','key':'SPACE'}]),
        row('Make tense toggle the light.','LIGHT',[{'gesture':'TENSE','action':'LIGHT_TOGGLE'}]),
        row('On my light, flex should switch it off and extend should switch it on.','LIGHT',
            [{'gesture':'FLEX','action':'LIGHT_OFF'},{'gesture':'EXTEND','action':'LIGHT_ON'}]),
        row('For the computer, swipe right should tap Q with CTRL and SHIFT for 100 milliseconds.','COMPUTER',
            [{'gesture':'SWIPE_RIGHT','action':'KEY_TAP','key':'Q','modifiers':['CTRL','SHIFT'],'duration_ms':100}]),
        row('Set light brightness to 75 percent when I swipe down.','LIGHT',
            [{'gesture':'SWIPE_DOWN','action':'LIGHT_SET_BRIGHTNESS','amount':75}]),
        row('On the computer, swipe left moves the mouse left by 25 and swipe up scrolls up by 2.','COMPUTER',
            [{'gesture':'SWIPE_LEFT','action':'MOUSE_MOVE_LEFT','amount':25},
             {'gesture':'SWIPE_UP','action':'MOUSE_SCROLL_UP','amount':2}]),
        row('Disable the flex action on the computer.','COMPUTER',[{'gesture':'FLEX','action':'NONE'}]),
    ]
    heldout_calls = {json.dumps(signature(r['call'])) for r in heldout}
    samples = [r for r in samples if json.dumps(signature(r['call'])) not in heldout_calls]
    rng.shuffle(samples)
    DATA.mkdir(exist_ok=True)
    for name, rows in [('train',samples),('heldout',heldout)]:
        (DATA/f'{name}.json').write_text(json.dumps(rows,indent=2)+'\n')
    (DATA/'tools.json').write_text(json.dumps(tools(),indent=2)+'\n')
    print(f'DATA: {len(samples)} train, {len(heldout)} heldout; all validated',flush=True)


def evaluate(backend, rows, destination):
    results = []
    for index, sample in enumerate(rows):
        result = {'prompt':sample['prompt'],'expected':sample['call'],'passed':False}
        try:
            generated = backend.generate([
                {'role':'developer','content':ACTIVATION},
                {'role':'user','content':sample['prompt']}], tools(), max_new_tokens=256)
            result['raw'] = generated.text
            call = parse_call(generated.text)
            result['call'] = call
            result['passed'] = signature(call) == signature(sample['call'])
        except (ValueError, TypeError, KeyError) as exc:
            result['error'] = str(exc)
        results.append(result)
        print(f'EVAL {index+1}/{len(rows)}: {result["passed"]}',flush=True)
        destination.write_text(json.dumps(results,indent=2)+'\n')
    return sum(r['passed'] for r in results)


def train(args):
    import torch
    from datasets import Dataset
    from transformers import AutoTokenizer, AutoModelForCausalLM, set_seed
    from trl import SFTConfig, SFTTrainer
    torch.set_num_threads(8)
    set_seed(42)
    if args.output.exists():
        raise ValueError('Choose a fresh output directory; existing runs are preserved')
    args.output.mkdir(parents=True)
    train_rows = json.loads((DATA/'train.json').read_text())
    heldout = json.loads((DATA/'heldout.json').read_text())
    tokenizer = AutoTokenizer.from_pretrained(MODEL_ID, revision=BASE_REVISION)
    model = AutoModelForCausalLM.from_pretrained(MODEL_ID, revision=BASE_REVISION,
        dtype=torch.float32, attn_implementation='eager').eval()
    original_generation_config = model.generation_config.to_dict()
    backend = FunctionGemma.__new__(FunctionGemma)
    backend.model, backend.processor = model, tokenizer
    before = evaluate(backend, heldout, args.output/'before.json')
    rendered = []
    lengths = []
    for sample in train_rows:
        signature(sample['call'])
        messages = [{'role':'developer','content':ACTIVATION},
                    {'role':'user','content':sample['prompt']}]
        prompt = tokenizer.apply_chat_template(messages,tools=tools(),add_generation_prompt=True,tokenize=False)
        messages.append({'role':'assistant','tool_calls':[{'type':'function','function':sample['call']}]})
        full = tokenizer.apply_chat_template(messages,tools=tools(),add_generation_prompt=False,tokenize=False)
        if not full.startswith(prompt):
            raise ValueError('Chat template prompt/completion boundary mismatch')
        lengths.append(len(tokenizer(full,add_special_tokens=False)['input_ids']))
        rendered.append({'prompt':prompt,'completion':full[len(prompt):]})
    if max(lengths) > 2048:
        raise ValueError('Dataset exceeds sequence bound; refusing silent truncation')
    config = SFTConfig(output_dir=str(args.output),max_length=max(lengths)+8,
        packing=False,max_steps=args.steps,per_device_train_batch_size=1,
        gradient_checkpointing=True,optim='adamw_torch',learning_rate=5e-5,
        lr_scheduler_type='constant',logging_steps=4,save_strategy='no',
        eval_strategy='no',fp16=False,bf16=False,use_cpu=True,
        report_to='none',push_to_hub=False,dataloader_pin_memory=False,
        completion_only_loss=True,seed=42,disable_tqdm=True)
    trainer = SFTTrainer(model=model,args=config,train_dataset=Dataset.from_list(rendered),processing_class=tokenizer)
    metadata = {'base_model':MODEL_ID,'revision':BASE_REVISION,'steps':args.steps,
        'train_count':len(train_rows),'heldout_count':len(heldout),'max_sequence_tokens':max(lengths),
        'dataset_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in DATA.glob('*.json')},
        'source':'https://github.com/google-gemma/cookbook/blob/main/docs/functiongemma/finetuning-with-functiongemma.ipynb'}
    (args.output/'run.json').write_text(json.dumps(metadata,indent=2))
    metrics = trainer.train().metrics
    model.gradient_checkpointing_disable()
    model.config.use_cache = True
    from transformers import GenerationConfig
    model.generation_config = GenerationConfig.from_dict(original_generation_config)
    trainer.save_model(str(args.output/'model'))
    tokenizer.save_pretrained(args.output/'model')
    model.eval()
    after = evaluate(backend,heldout,args.output/'after.json')
    (args.output/'summary.json').write_text(json.dumps({'before':before,'after':after,'total':len(heldout),'training':metrics},indent=2))
    print(f'COMPLETE: before={before}/8 after={after}/8 saved={args.output}',flush=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--prepare',action='store_true')
    parser.add_argument('--steps',type=int,default=64)
    parser.add_argument('--output',type=Path,default=HERE/'training_runs'/'batch-v1')
    args=parser.parse_args()
    if args.prepare:
        prepare()
    else:
        if not 1 <= args.steps <= 1000:
            parser.error('steps must be between 1 and 1000')
        train(args)
