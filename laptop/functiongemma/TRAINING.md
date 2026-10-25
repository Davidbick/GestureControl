# Batch-only FunctionGemma fine-tuning

This is a local adaptation of [Google's fine-tuning notebook](https://github.com/google-gemma/cookbook/blob/main/docs/functiongemma/finetuning-with-functiongemma.ipynb).
It uses the official base model, its chat template, the developer activation
sentence, TRL `SFTConfig` / `SFTTrainer`, and a saved local checkpoint. It never
publishes to the Hub or executes BLE. Firmware, protocol, and validators are
unchanged and checked against `protected_files.sha256.json`.

The simplified interface is **one tool**, `configure_gesture_batch(target,
mappings)`, including one-entry batches. Its fields, enum values, and constraints
are copied unchanged from the existing protocol schema. The base weather model
and its test are separate from the fine-tuned checkpoint.

## Reproduce

Use the existing `.venv-transformers` environment, authenticated for the model:

```powershell
.\.venv-transformers\Scripts\python.exe -m pip install -r laptop/functiongemma/requirements-training.txt
$env:FUNCTIONGEMMA_LIVE_TESTS='1'
.\.venv-transformers\Scripts\python.exe -m unittest discover -s laptop/functiongemma -p test_weather_regression.py -v
.\.venv-transformers\Scripts\python.exe laptop/functiongemma/train_batch.py --prepare
.\.venv-transformers\Scripts\python.exe -m unittest discover -s laptop/functiongemma -p test_batch_training.py -v
.\.venv-transformers\Scripts\python.exe -u laptop/functiongemma/train_batch.py --steps 64 --output laptop/functiongemma/training_runs/batch-v1
```

Choose a new output directory for another run: existing runs are never overwritten.
Training artifacts are Git-ignored. Dataset files and code are reviewable text.
The opt-in weather regression uses cached official weights, has a 180-second
outer deadline, and asserts the passing Tokyo call and mock-result summary.

## Data and evaluation

The 96 training commands are synthetic, generated with seed 1729. They cover
COMPUTER and LIGHT, all seven gestures, all action types, and examples with key,
modifiers, amount, and duration. Batch lengths in this pilot are 1, 2, 3, 4, and 7;
sizes 5 and 6 are not directly trained. This is a small template-generated pilot,
not a representative user-command dataset.

Eight manually authored held-out prompts use different wording and include the
original failing single/multi-mapping commands. Expected packet signatures and
prompt strings are disjoint from training. No held-out samples enter optimization
or checkpoint selection. This small test is used once before and once after the
fixed training run; it cannot establish production accuracy or robust refusal of
ambiguous/out-of-domain requests.

Every label is validated using the existing `build_batch`. Evaluation requires a
complete single call, strict parsing, successful validation, and exact packet
semantics (ignoring transaction IDs and mapping order). Invalid output is rejected,
never normalized, repaired, or executed. Generation is greedy and bounded to 256
tokens and 30 seconds per prompt. Long seven-entry outputs can exceed this token
budget and will be rejected; the pilot's held-out set has at most three entries.

## Notebook adaptations

- Pinned base revision `39eccb091651513a5dfb56892d3714c1b5b8276c`.
- CPU float32, eight CPU threads, batch size one, gradient checkpointing, and
  standard AdamW for the available 16 GB Windows machine.
- 64 optimizer steps, learning rate 5e-5, constant schedule, seed 42. This is a
  bounded initial run (less than one epoch), not the notebook's eight epochs.
- Chat-template-rendered prompt/completion strings preserve nested optional
  arguments without Arrow inserting null keys into tool calls. Completion-only
  loss trains the assistant response; the notebook uses conversational records.
- Sequence lengths are checked before training, with no permitted truncation of
  labels. The maximum accepted sequence is 2048 tokens, packing is disabled.
- Full-parameter fine-tuning; no adapters, quantization, or firmware changes.
- No automatic Hub push, external logging, or runtime installation.
- Final checkpoint is saved separately; original generation stop IDs are restored
  after trainer setup and before evaluation/export.

`training_runs/<run>/run.json` records configuration, source, and dataset hashes.
`before.json` and `after.json` record each held-out response and failure.
`summary.json` records completed training metrics and the before/after score.
`model/` holds the checkpoint and tokenizer. Completion of training alone is not
an acceptance gate for device execution.
