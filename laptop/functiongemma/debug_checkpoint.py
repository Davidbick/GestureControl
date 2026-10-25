import json
import time
from pathlib import Path

import torch
from transformers import AutoTokenizer, AutoModelForCausalLM

from transformers_backend import ACTIVATION
from train_batch import DATA, tools

ROOT = Path(__file__).resolve().parents[2]
CHECKPOINT = ROOT / "laptop/functiongemma/training_runs/batch-v2/model"

torch.set_num_threads(8)

tokenizer = AutoTokenizer.from_pretrained(CHECKPOINT)
model = AutoModelForCausalLM.from_pretrained(
    CHECKPOINT,
    dtype=torch.float32,
    attn_implementation="eager",
).eval()

samples = json.loads((DATA / "heldout.json").read_text())

# Start with only the simplest failed command.
sample = samples[0]
print("PROMPT:", sample["prompt"], flush=True)

messages = [
    {"role": "developer", "content": ACTIVATION},
    {"role": "user", "content": sample["prompt"]},
]

inputs = tokenizer.apply_chat_template(
    messages,
    tools=tools(),
    add_generation_prompt=True,
    return_dict=True,
    return_tensors="pt",
)

start = time.perf_counter()

with torch.inference_mode():
    output = model.generate(
        **inputs,
        max_new_tokens=384,
        max_time=90,
        do_sample=False,
        pad_token_id=tokenizer.eos_token_id,
    )

elapsed = time.perf_counter() - start
tokens = output[0][inputs["input_ids"].shape[-1]:].tolist()

print("TIME:", round(elapsed, 2), "seconds")
print("TOKENS:", len(tokens))
print("LAST TOKEN:", tokens[-1] if tokens else None)
print("CONFIGURED END TOKENS:", model.generation_config.eos_token_id)
print("RAW OUTPUT:")
print(tokenizer.decode(tokens, skip_special_tokens=False))
