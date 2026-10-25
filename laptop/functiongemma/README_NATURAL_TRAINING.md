# FunctionGemma natural-language training (v2)

These files do not replace `train_single.py`, modify the BLE API, or connect to hardware.

## Install

Copy these files into the existing GestureControl repository:

- `laptop/functiongemma/generate_natural_data.py`
- `laptop/functiongemma/train_natural.py`
- `laptop/functiongemma/natural_data/train.json`
- `laptop/functiongemma/natural_data/heldout.json`

## Verify data on HiPerGator

Run in the root of GestureControl, using your known working GPU virtual environment:

```bash
../gesturecontrol-gpu-env/bin/python laptop/functiongemma/generate_natural_data.py \
  --output laptop/functiongemma/natural_data_check
```

Expected: `Validated 245 training examples and 49 held-out prompts`. This does not use a GPU.

## Train on one GPU

```bash
CUDA_VISIBLE_DEVICES=0 ../gesturecontrol-gpu-env/bin/python -u \
  laptop/functiongemma/train_natural.py \
  --steps 490 \
  --output laptop/functiongemma/training_runs/natural-v1
```

The script defaults to evaluating **all 49 held-out prompts after training**. Set `--evaluate-before` to evaluate the base model too (slower). Use a fresh `--output` folder for every run. Do not overwrite your successful overfit checkpoints.

## Inspect performance

```bash
cat laptop/functiongemma/training_runs/natural-v1/summary.json
```

Inspect incorrect predictions:

```bash
../gesturecontrol-gpu-env/bin/python - <<'PY'
import json
p = 'laptop/functiongemma/training_runs/natural-v1/after.json'
for item in json.load(open(p)):
    if not item['passed']:
        print('\nINPUT:', item['prompt'])
        print('EXPECTED:', item['expected'])
        print('ACTUAL:', item.get('actual'))
        print('ERROR:', item.get('error'))
PY
```

Training data is **synthetically templated**, with different held-out phrasings; it is not real user utterance data. Success here tests generalization to different prompt wordings for familiar gesture/action combinations, **not** to unseen API actions, multiple instructions, real speech recognition errors, or unconstrained conversation.

No BLE writes are performed. This checkpoint is for evaluation, not deployment without more testing.
