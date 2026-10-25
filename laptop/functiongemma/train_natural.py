"""Fine-tune FunctionGemma with natural speech, evaluate on unseen prompts.

Build data first: python laptop/functiongemma/generate_natural_data.py
Train on one assigned GPU: CUDA_VISIBLE_DEVICES=0 python -u laptop/functiongemma/train_natural.py \\
    --steps 490 --output laptop/functiongemma/training_runs/natural-v1

Does not write to BLE. Keeps the working 8-example trainer and checkpoints intact.
"""
import argparse
import hashlib
import json
import os
from collections import Counter
from pathlib import Path

from train_single import (
    REVISION, TOOLS, expected_call, sample_messages, signature,
)
from transformers_backend import MODEL_ID, parse_call

HERE = Path(__file__).resolve().parent


def read_split(path):
    rows = json.loads(path.read_text())
    if not isinstance(rows, list) or not rows:
        raise ValueError(f"Expected a nonempty list in {path}")
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"prompt", "call"}:
            raise ValueError(f"Bad dataset entry in {path}")
        if not isinstance(row["prompt"], str) or not row["prompt"].strip():
            raise ValueError(f"Empty or invalid prompt in {path}")
        signature(row["call"])  # Validate the actual BLE packet semantics.
    normalized = [row["prompt"].strip().casefold() for row in rows]
    if len(normalized) != len(set(normalized)):
        raise ValueError(f"Duplicate prompts in {path}")
    return rows


def check_splits(train, heldout):
    seen = {row["prompt"].strip().casefold() for row in train}
    overlap = seen & {row["prompt"].strip().casefold() for row in heldout}
    if overlap:
        raise ValueError(f"Training/test prompt leakage: {sorted(overlap)[:3]}")
    # Report coverage; don't silently mix the held-out data into training.
    for name, rows in (("train", train), ("heldout", heldout)):
        counts = Counter((row["call"]["arguments"]["target"],
                          row["call"]["arguments"]["gesture"],
                          row["call"]["arguments"]["action"]) for row in rows)
        print(f"{name}: {len(rows)} unique utterances / "
              f"{len(counts)} target-gesture-action combinations", flush=True)


def evaluate(model, tokenizer, examples, path, max_new_tokens=160):
    """Evaluate exact device/action/key semantics, not similarity of wording."""
    import torch
    passed = 0
    results = []
    for index, sample in enumerate(examples, start=1):
        entry = {"prompt": sample["prompt"], "expected": sample["call"],
                 "passed": False}
        try:
            inputs = tokenizer.apply_chat_template(
                sample_messages(sample["prompt"]), tools=TOOLS,
                add_generation_prompt=True, return_dict=True,
                return_tensors="pt").to(model.device)
            with torch.inference_mode():
                result = model.generate(
                    **inputs, max_new_tokens=max_new_tokens,
                    max_time=30, do_sample=False,
                    pad_token_id=tokenizer.eos_token_id)
            tokens = result[0][inputs["input_ids"].shape[-1]:].tolist()
            ends = model.generation_config.eos_token_id
            eos = (ends,) if isinstance(ends, int) else tuple(ends or ())
            complete = bool(tokens and tokens[-1] in eos and
                            len(tokens) < max_new_tokens)
            entry["raw"] = tokenizer.decode(
                tokens[:-1] if complete else tokens,
                skip_special_tokens=False)
            entry["tokens"] = len(tokens)
            if not complete:
                raise ValueError("Generation unfinished")
            actual = parse_call(entry["raw"])
            entry["actual"] = actual
            entry["passed"] = signature(actual) == signature(sample["call"])
            if entry["passed"]:
                passed += 1
            else:
                entry["error"] = "Valid call but incorrect mapping"
        except (ValueError, TypeError, KeyError) as exc:
            entry["error"] = str(exc)
        results.append(entry)
        if index % 10 == 0 or index == len(examples):
            print(f"EVAL {index}/{len(examples)}: {passed} correct", flush=True)
        path.write_text(json.dumps(results, indent=2) + "\n")
    return passed


def render_training(tokenizer, train):
    records, lengths = [], []
    for example in train:
        messages = sample_messages(example["prompt"])
        prefix = tokenizer.apply_chat_template(
            messages, tools=TOOLS, add_generation_prompt=True, tokenize=False)
        messages.append({"role": "assistant", "tool_calls": [
            {"type": "function", "function": example["call"]}]})
        full = tokenizer.apply_chat_template(
            messages, tools=TOOLS, add_generation_prompt=False, tokenize=False)
        if not full.startswith(prefix):
            raise ValueError("Training prompt/completion boundary mismatch")
        completion = full[len(prefix):]
        if "<start_function_call>" not in completion or "<end_function_call>" not in completion:
            raise ValueError("Training label missing FunctionGemma call delimiters")
        length = len(tokenizer(full, add_special_tokens=False)["input_ids"])
        if length > 2048:
            raise ValueError("Refusing to truncate a training example")
        lengths.append(length)
        records.append({"prompt": prefix, "completion": completion})
    return records, max(lengths)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=Path, default=HERE / "natural_data")
    parser.add_argument("--steps", type=int, default=490)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--evaluate-before", action="store_true",
                        help="Also run the untouched base model on all held-out prompts")
    parser.add_argument("--eval-limit", type=int, default=0,
                        help="Evaluate only first N held-out examples for a fast smoke test")
    args = parser.parse_args()
    if not 1 <= args.steps <= 10000:
        parser.error("--steps must be between 1 and 10000")
    if args.output.exists():
        parser.error("Output path exists; choose a new directory to preserve results")
    train_path, heldout_path = args.data / "train.json", args.data / "heldout.json"
    train, heldout = read_split(train_path), read_split(heldout_path)
    check_splits(train, heldout)
    if args.eval_limit:
        heldout = heldout[:args.eval_limit]
    if not heldout:
        parser.error("No evaluation examples")

    import torch
    from datasets import Dataset
    from transformers import (
        AutoModelForCausalLM, AutoTokenizer, GenerationConfig, set_seed,
    )
    from trl import SFTConfig, SFTTrainer

    if not torch.cuda.is_available() or torch.cuda.device_count() != 1:
        raise RuntimeError("Run on exactly one L4: CUDA_VISIBLE_DEVICES=0")
    torch.set_num_threads(int(os.environ.get("SLURM_CPUS_PER_TASK") or "4"))
    set_seed(42)
    print("TRAINING GPU:", torch.cuda.get_device_name(0), flush=True)
    # Only create an output folder once data and CUDA checks have passed.
    args.output.mkdir(parents=True)
    tokenizer = AutoTokenizer.from_pretrained(MODEL_ID, revision=REVISION)
    model = AutoModelForCausalLM.from_pretrained(
        MODEL_ID, revision=REVISION, dtype=torch.float32,
        attn_implementation="eager").to("cuda:0").eval()
    original_generation_config = model.generation_config.to_dict()

    if args.evaluate_before:
        before = evaluate(model, tokenizer, heldout, args.output / "before.json")
    else:
        before = None
    rendered, max_tokens = render_training(tokenizer, train)
    config = SFTConfig(
        output_dir=str(args.output), max_length=max_tokens + 8,
        packing=False, max_steps=args.steps, per_device_train_batch_size=1,
        gradient_checkpointing=False, optim="adamw_torch", learning_rate=5e-5,
        lr_scheduler_type="constant", logging_steps=25, save_strategy="no",
        eval_strategy="no", fp16=False, bf16=False, use_cpu=False,
        report_to="none", push_to_hub=False, dataloader_pin_memory=False,
        completion_only_loss=True, seed=42, disable_tqdm=True,
    )
    trainer = SFTTrainer(
        model=model, args=config, train_dataset=Dataset.from_list(rendered),
        processing_class=tokenizer)
    metadata = {
        "model": MODEL_ID, "revision": REVISION, "steps": args.steps,
        "train_count": len(train), "heldout_count": len(heldout),
        "dataset_sha256": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (train_path, heldout_path)
        },
        "before_evaluated": args.evaluate_before,
    }
    (args.output / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    metrics = trainer.train().metrics
    model.config.use_cache = True
    model.generation_config = GenerationConfig.from_dict(original_generation_config)
    trainer.save_model(str(args.output / "model"))
    tokenizer.save_pretrained(args.output / "model")
    model.eval()
    after = evaluate(model, tokenizer, heldout, args.output / "after.json")
    summary = {"before": before, "after": after, "total": len(heldout),
               "accuracy": round(after / len(heldout), 4),
               "steps": args.steps, "training": metrics}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"COMPLETE: {after}/{len(heldout)} ({after/len(heldout):.1%}) unseen prompts", flush=True)


if __name__ == "__main__":
    main()
