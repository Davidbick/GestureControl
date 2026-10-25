"""Run a frozen, out-of-template FunctionGemma evaluation on a saved checkpoint.

Evaluation only: no training, BLE imports are used ONLY to encode/validate
labels through train_single.signature. No device scanning, no actuation.

HiPerGator, from GestureControl repo root:
  CUDA_VISIBLE_DEVICES=0 ../gesturecontrol-gpu-env/bin/python -u \\
    laptop/functiongemma/evaluate_challenge.py \\
    --model laptop/functiongemma/training_runs/natural-v3/model \\
    --output laptop/functiongemma/challenge_runs/challenge-v1

Also supports --validate-only, --interactive, or --prompts-file file.txt.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from collections import Counter
from pathlib import Path

from train_single import TOOLS, signature
from transformers_backend import ACTIVATION, parse_call


def messages_for(prompt):
    return [
        {"role": "developer", "content": ACTIVATION},
        {"role": "user", "content": prompt},
    ]

HERE = Path(__file__).resolve().parent
DEFAULT_MODEL = HERE / "training_runs" / "natural-v3" / "model"
DEFAULT_DATA = HERE / "challenge_v1.json"


def read_challenge(path: Path):
    if not path.is_file():
        raise FileNotFoundError(path)
    rows = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(rows, list) or not rows:
        raise ValueError("Challenge data must be a nonempty JSON list")
    ids, prompts, combinations = set(), set(), set()
    kind_counts = Counter()
    for row in rows:
        if not isinstance(row, dict) or row.get("kind") not in {"mapping", "unsupported"}:
            raise ValueError(f"Invalid row: {row!r}")
        prompt, uid = row.get("prompt"), row.get("id")
        if not isinstance(uid, str) or not uid or uid in ids:
            raise ValueError(f"Invalid or duplicate id: {uid!r}")
        if not isinstance(prompt, str) or not prompt.strip():
            raise ValueError(f"Blank prompt for {uid}")
        normalized = prompt.strip().casefold()
        if normalized in prompts:
            raise ValueError(f"Duplicate prompt: {prompt!r}")
        prompts.add(normalized)
        ids.add(uid)
        kind_counts[row["kind"]] += 1
        if row["kind"] == "mapping":
            if set(row) != {"id", "kind", "prompt", "expected"}:
                raise ValueError(f"Unexpected fields for {uid}")
            # Strict BLE encoder validation, but NO BLE connection.
            signature(row["expected"])
            args = row["expected"]["arguments"]
            combo = args["target"], args["gesture"], args["action"]
            if combo in combinations:
                raise ValueError(f"Duplicate target/gesture/action combination: {combo}")
            combinations.add(combo)
        elif set(row) != {"id", "kind", "prompt", "reason"}:
            raise ValueError(f"Unexpected fields for {uid}")
    return rows, kind_counts


def check_leakage(rows, reference_files):
    challenge_prompts = {r["prompt"].strip().casefold() for r in rows}
    for path in reference_files:
        if not path.is_file():
            print(f"REFERENCE NOT FOUND (skip overlap check): {path}", flush=True)
            continue
        examples = json.loads(path.read_text(encoding="utf-8"))
        existing = {r["prompt"].strip().casefold() for r in examples}
        overlap = existing & challenge_prompts
        if overlap:
            raise ValueError(f"Challenge prompt leaked into {path}: {sorted(overlap)[:3]}")
        print(f"No exact-prompt overlap with {path} ({len(examples)} rows)", flush=True)


def load_model(path):
    import torch
    from transformers import AutoTokenizer, AutoModelForCausalLM

    if not path.is_dir():
        raise FileNotFoundError(f"Saved checkpoint not found: {path}")
    if not torch.cuda.is_available() or torch.cuda.device_count() != 1:
        raise RuntimeError("Requires one GPU; set CUDA_VISIBLE_DEVICES=0 on HiPerGator")
    torch.set_num_threads(int(os.environ.get("SLURM_CPUS_PER_TASK") or "4"))
    tokenizer = AutoTokenizer.from_pretrained(path, local_files_only=True)
    model = AutoModelForCausalLM.from_pretrained(
        path, dtype=torch.float32, attn_implementation="eager",
        local_files_only=True,
    ).to("cuda:0").eval()
    print("CHECKPOINT:", path, flush=True)
    print("GPU:", torch.cuda.get_device_name(0), flush=True)
    return model, tokenizer


def generate(model, tokenizer, prompt, max_new_tokens=160):
    import torch
    inputs = tokenizer.apply_chat_template(
        messages_for(prompt), tools=TOOLS, add_generation_prompt=True,
        return_dict=True, return_tensors="pt",
    ).to(model.device)
    with torch.inference_mode():
        output = model.generate(
            **inputs, max_new_tokens=max_new_tokens, max_time=30,
            do_sample=False, pad_token_id=tokenizer.eos_token_id,
        )
    generated_ids = output[0][inputs["input_ids"].shape[-1]:].tolist()
    eos = model.generation_config.eos_token_id
    eos_ids = (eos,) if isinstance(eos, int) else tuple(eos or ())
    complete = bool(generated_ids and generated_ids[-1] in eos_ids
                    and len(generated_ids) < max_new_tokens)
    raw = tokenizer.decode(
        generated_ids[:-1] if complete else generated_ids,
        skip_special_tokens=False,
    )
    return {"raw": raw, "tokens": len(generated_ids), "complete": complete}


def interpret(model, tokenizer, prompt):
    """Parse/validate without performing any action. Retain failed raw outputs."""
    response = generate(model, tokenizer, prompt)
    response.update({"valid_mapping": False, "call": None})
    if not response["complete"]:
        response["error"] = "Generation unfinished"
        return response
    try:
        call = parse_call(response["raw"])
        signature(call)
        response["call"] = call
        response["valid_mapping"] = True
    except (ValueError, TypeError, KeyError) as exc:
        response["error"] = str(exc)
    return response


def explicit_abstention(raw):
    """Conservative text check. Malformed function calls are NOT abstentions."""
    if "<start_function_call>" in raw or "<end_function_call>" in raw:
        return False
    return bool(re.search(
        r"\b(?:cannot|can't|unable|unsupported|unclear|ambiguous|"
        r"clarif(?:y|ication)|specify|need (?:a|the|more)|which (?:device|gesture))\b",
        raw.casefold(),
    ))


def score(rows, predictions):
    positives = Counter()
    negatives = Counter()
    failures = Counter()
    results = []
    for sample, answer in zip(rows, predictions, strict=True):
        entry = {**sample, **answer}
        if sample["kind"] == "mapping":
            positives["total"] += 1
            if answer["valid_mapping"]:
                got = answer["call"]["arguments"]
                want = sample["expected"]["arguments"]
                if signature(answer["call"]) == signature(sample["expected"]):
                    entry["passed"] = True
                    positives["correct"] += 1
                else:
                    entry["passed"] = False
                    entry["mismatches"] = {
                        field: {"expected": want.get(field), "actual": got.get(field)}
                        for field in ("target", "gesture", "action", "key")
                        if want.get(field) != got.get(field)
                    }
                    for field in entry["mismatches"]:
                        failures[field] += 1
            else:
                entry["passed"] = False
                failures["invalid_or_incomplete_call"] += 1
        else:
            negatives["total"] += 1
            if answer["valid_mapping"]:
                # The model has no training to abstain. Every such valid call
                # is an unintended mapping for the deliberately invalid input.
                negatives["valid_unintended_mapping"] += 1
                entry["classification"] = "UNINTENDED_VALID_MAPPING"
            elif explicit_abstention(answer["raw"]):
                negatives["explicit_abstention"] += 1
                entry["classification"] = "EXPLICIT_ABSTENTION"
            else:
                negatives["nonexecuting_but_not_refusal"] += 1
                entry["classification"] = "NONEXECUTING_INVALID_OUTPUT"
            # There is no honest 'correct' score for negatives until a refusal
            # tool and explicit no-call training exist.
        results.append(entry)
    return results, {
        "mapping": {"correct": positives["correct"],
                    "total": positives["total"],
                    "accuracy": round(positives["correct"] / positives["total"], 4)},
        "unsupported": {
            "total": negatives["total"],
            "unintended_valid_mappings": negatives["valid_unintended_mapping"],
            "explicit_abstentions": negatives["explicit_abstention"],
            "invalid_nonexecuting_outputs": negatives["nonexecuting_but_not_refusal"],
        },
        "mapping_failure_breakdown": dict(failures),
    }


def run_benchmark(model, tokenizer, rows, output):
    if output.exists():
        raise FileExistsError(f"Output already exists; use a new --output path: {output}")
    output.mkdir(parents=True)
    predictions = []
    for index, sample in enumerate(rows, 1):
        answer = interpret(model, tokenizer, sample["prompt"])
        predictions.append(answer)
        category = "VALID" if answer["valid_mapping"] else "INVALID"
        print(f"TEST {index:02d}/{len(rows)} {sample['id']} [{sample['kind']}] {category}", flush=True)
        interim, summary = score(rows[:index], predictions)
        (output / "results.json").write_text(json.dumps(interim, indent=2) + "\n", encoding="utf-8")
    results, summary = score(rows, predictions)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"MAPPINGS: {summary['mapping']['correct']}/{summary['mapping']['total']} "
          f"({summary['mapping']['accuracy']:.1%})", flush=True)
    print(f"UNSUPPORTED: {summary['unsupported']['unintended_valid_mappings']}/"
          f"{summary['unsupported']['total']} yielded unintended valid mappings", flush=True)
    print("OUTPUT:", output, flush=True)


def run_manual(model, tokenizer, prompt):
    answer = interpret(model, tokenizer, prompt)
    print("RAW:", answer["raw"])
    if answer["valid_mapping"]:
        print("VALIDATED (NOT SENT TO DEVICES):", json.dumps(answer["call"], indent=2))
    else:
        print("INVALID / NO VALID MAPPING:", answer.get("error", "not a function call"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--output", type=Path, help="New directory for a benchmark run")
    parser.add_argument("--validate-only", action="store_true", help="Validate challenge labels and split overlap; no model load")
    parser.add_argument("--interactive", action="store_true", help="Try your own commands manually, with no device execution")
    parser.add_argument("--prompts-file", type=Path, help="Text file of real STT transcripts, one command per line (unscored)")
    args = parser.parse_args()

    if sum(bool(x) for x in (args.validate_only, args.interactive, args.prompts_file)) > 1:
        parser.error("Choose one of --validate-only, --interactive, or --prompts-file")
    rows, counts = read_challenge(args.data)
    print(f"Challenge: {counts['mapping']} mapping commands, {counts['unsupported']} unsupported requests", flush=True)
    check_leakage(rows, [HERE / "natural_data_v3" / "train.json",
                         HERE / "natural_data_v3" / "heldout.json"])
    if args.validate_only:
        print("Validation OK; no model loaded and no BLE access", flush=True)
        return
    if not args.interactive and args.output is None:
        parser.error("--output is required for benchmark or --prompts-file mode")
    if args.output is not None and args.output.exists():
        parser.error("Output path exists; use a fresh directory")
    model, tokenizer = load_model(args.model)
    if args.interactive:
        print("Enter commands, one at a time. Ctrl-D to stop; NOTHING is sent over BLE.")
        try:
            while True:
                prompt = input("GestureControl> ").strip()
                if prompt:
                    run_manual(model, tokenizer, prompt)
        except (KeyboardInterrupt, EOFError):
            print("\nDone")
        return
    if args.prompts_file:
        prompts = [p.strip() for p in args.prompts_file.read_text(encoding="utf-8").splitlines()
                   if p.strip() and not p.lstrip().startswith("#")]
        if not prompts:
            parser.error("Prompts file is empty")
        args.output.mkdir(parents=True)
        responses = []
        for index, prompt in enumerate(prompts, 1):
            answer = interpret(model, tokenizer, prompt)
            responses.append({"prompt": prompt, **answer})
            (args.output / "transcripts.json").write_text(json.dumps(responses, indent=2) + "\n")
            print(f"TRANSCRIPT {index}/{len(prompts)}: {prompt}\n  "
                  f"{'VALID '+str(answer['call']) if answer['valid_mapping'] else 'INVALID '+str(answer.get('error'))}",
                  flush=True)
        print("Unlabeled transcripts are for human review, not automatic accuracy scoring")
        return
    metadata = {"model": str(args.model), "challenge_data": str(args.data),
                "challenge_sha256": hashlib.sha256(args.data.read_bytes()).hexdigest()}
    run_benchmark(model, tokenizer, rows, args.output)
    (args.output / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
