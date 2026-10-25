# FunctionGemma (experimental)

Local FunctionGemma inference and fine-tuning using Google's Hugging Face Transformers implementation. No Ollama dependency. Nothing in this directory sends BLE commands.

## Current state

- The original `google/functiongemma-270m-it` passed the Tokyo weather function-calling test using Transformers.
- The unfine-tuned model failed GestureControl's eight held-out mapping requests (0/8). Training runs must be evaluated separately; a decreasing training loss is **not** proof of mapping accuracy.
- GestureControl training uses exactly one tool, `configure_gesture_batch(target, mappings)`. One mapping is represented as a one-item batch. The authoritative schema is `protocol/functiongemma_tools.json`; validation uses `laptop/configuration/ble_config.py`.
- Generated mappings are **not** connected to BLE. No model output should be executed without both protocol and intent validation.

## Setup (PowerShell, repository root)

```powershell
python -m venv .venv-transformers
.\.venv-transformers\Scripts\python.exe -m pip install -r laptop/functiongemma/requirements-training.txt
```

Accept the license for [google/functiongemma-270m-it](https://huggingface.co/google/functiongemma-270m-it), then authenticate locally using `.\.venv-transformers\Scripts\hf.exe auth login`. Model downloads are cached; inference is local.

For inference-only environments, install `requirements-transformers.txt` instead.

## Train and evaluate

The checked-in `batch_data/train.json` contains 96 synthetic training examples, and `batch_data/heldout.json` contains eight distinct evaluation prompts. The labels are validated with the existing batch encoder.

```powershell
.\.venv-transformers\Scripts\python.exe -m unittest discover -s laptop/functiongemma -p test_batch_training.py -v
.\.venv-transformers\Scripts\python.exe -u laptop/functiongemma/train_batch.py --steps 64 --output laptop/functiongemma/training_runs/batch-v3
```

Use a **new** output directory for each run; existing runs are never overwritten. Do not run `--prepare` unless you explicitly want to regenerate the checked-in synthetic dataset.

A completed run writes `summary.json`, `before.json`, `after.json`, and a `model/` checkpoint under its output directory. Training outputs are git-ignored. Inspect the held-out results before attempting any device integration; eight prompts alone are not a production reliability test.

## Regression tests

```powershell
.\.venv-transformers\Scripts\python.exe -m unittest discover -s laptop/functiongemma -p test_transformers.py -v
.\.venv-transformers\Scripts\python.exe -m unittest discover -s laptop/configuration -p test_protocol.py -v
```

Optional live reference-model weather regression, with downloaded weights cached:

```powershell
$env:FUNCTIONGEMMA_LIVE_TESTS='1'
.\.venv-transformers\Scripts\python.exe -m unittest discover -s laptop/functiongemma -p test_weather_regression.py -v
```

`transformers_backend.py` provides the bounded inference and strict function-call parser; `transformers_weather.py` is the known-good reference smoke test; `train_batch.py` owns training and held-out evaluation. There is no automatic upload to Hugging Face and no BLE execution.

Reference: [Google's FunctionGemma fine-tuning notebook](https://github.com/google-gemma/cookbook/blob/main/docs/functiongemma/finetuning-with-functiongemma.ipynb).
