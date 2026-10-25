
"""FunctionGemma single-mapping memorization test on HiPerGator.

Uses one model-facing tool and eight natural-language examples.
Does not connect to BLE.
"""
import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "laptop" / "configuration"))

from ble_config import build_batch
from transformers_backend import ACTIVATION, MODEL_ID, parse_call

REVISION = "39eccb091651513a5dfb56892d3714c1b5b8276c"

# Simplified tool for FunctionGemma, not a change to the BLE API.
TOOL = {
    "type": "function",
    "function": {
        "name": "map_gesture",
        "description": "Set one gesture action on MyoPC or MyoLight.",
        "parameters": {
            "type": "object",
            "properties": {
                "target": {
                    "type": "string",
                    "enum": ["COMPUTER", "LIGHT"],
                },
                "gesture": {
                    "type": "string",
                    "enum": [
                        "SWIPE_UP", "SWIPE_DOWN",
                        "SWIPE_LEFT", "SWIPE_RIGHT",
                        "TENSE", "FLEX", "EXTEND",
                    ],
                },
                "action": {
                    "type": "string",
                    "enum": [
                        "KEY_TAP", "KEY_TOGGLE",
                        "LIGHT_TOGGLE", "LIGHT_ON",
                        "LIGHT_OFF", "NONE",
                    ],
                },
                "key": {
                    "type": "string",
                    "description": (
                        "Required for keyboard actions, "
                        "such as W, A, S, D, or SPACE."
                    ),
                },
            },
            "required": ["target", "gesture", "action"],
            "additionalProperties": False,
        },
    },
}

TOOLS = [TOOL]

# Prompt, target, gesture, action, key
EXAMPLES = [
    ("Make swiping up press W on MyoPC.",
     "COMPUTER", "SWIPE_UP", "KEY_TAP", "W"),

    ("When I swipe down, hold S on my laptop.",
     "COMPUTER", "SWIPE_DOWN", "KEY_TOGGLE", "S"),

    ("Tensing my arm should toggle MyoLight.",
     "LIGHT", "TENSE", "LIGHT_TOGGLE", None),

    ("Flex to switch my lamp off.",
     "LIGHT", "FLEX", "LIGHT_OFF", None),

    ("Make extending my arm turn the light on.",
     "LIGHT", "EXTEND", "LIGHT_ON", None),

    ("Flick left to hit A on my keyboard.",
     "COMPUTER", "SWIPE_LEFT", "KEY_TAP", "A"),

    ("Swipe right should hold D on MyoPC.",
     "COMPUTER", "SWIPE_RIGHT", "KEY_TOGGLE", "D"),

    ("Disable the tense action on my laptop.",
     "COMPUTER", "TENSE", "NONE", None),
]


def expected_call(target, gesture, action, key):
    args = {
        "target": target,
        "gesture": gesture,
        "action": action,
    }
    if key is not None:
        args["key"] = key

    return {
        "name": "map_gesture",
        "arguments": args,
    }


def signature(call):
    """Validate with the existing GestureControl encoder."""
    if not isinstance(call, dict):
        raise ValueError("Expected a function call")

    if set(call) != {"name", "arguments"}:
        raise ValueError("Invalid function call structure")

    if call["name"] != "map_gesture":
        raise ValueError("Unexpected function")

    args = call["arguments"]

    if not isinstance(args, dict):
        raise ValueError("Arguments must be an object")

    allowed = {"target", "gesture", "action", "key"}
    required = {"target", "gesture", "action"}

    if set(args) - allowed:
        raise ValueError("Unexpected mapping arguments")

    if not required <= set(args):
        raise ValueError("Missing required arguments")

    mapping = {
        k: v for k, v in args.items()
        if k != "target"
    }

    target, entries = build_batch({
        "target": args["target"],
        "mappings": [mapping],
    })

    return target, entries[0]["set_packet"][2:].hex()


def messages_for(prompt):
    return [
        {"role": "developer", "content": ACTIVATION},
        {"role": "user", "content": prompt},
    ]


def generate(model, tokenizer, prompt):
    """Generate and preserve even incomplete output for debugging."""
    import torch

    max_tokens = 160

    inputs = tokenizer.apply_chat_template(
        messages_for(prompt),
        tools=TOOLS,
        add_generation_prompt=True,
        return_dict=True,
        return_tensors="pt",
    ).to(model.device)

    with torch.inference_mode():
        output = model.generate(
            **inputs,
            max_new_tokens=max_tokens,
            max_time=30,
            do_sample=False,
            pad_token_id=tokenizer.eos_token_id,
        )

    tokens = output[0][
        inputs["input_ids"].shape[-1]:
    ].tolist()

    eos = model.generation_config.eos_token_id
    end_ids = (eos,) if isinstance(eos, int) else tuple(eos or ())

    complete = bool(
        tokens
        and tokens[-1] in end_ids
        and len(tokens) < max_tokens
    )

    raw = tokenizer.decode(
        tokens[:-1] if complete else tokens,
        skip_special_tokens=False,
    )

    return raw, complete, len(tokens)


def evaluate(model, tokenizer, path):
    results = []

    for i, example in enumerate(EXAMPLES, 1):
        prompt, target, gesture, action, key = example

        expected = expected_call(target, gesture, action, key)

        result = {
            "prompt": prompt,
            "expected": expected,
            "passed": False,
        }

        try:
            raw, complete, count = generate(
                model, tokenizer, prompt
            )

            result["raw"] = raw
            result["tokens"] = count

            if not complete:
                raise ValueError("Generation unfinished")

            actual = parse_call(raw)
            result["actual"] = actual

            result["passed"] = (
                signature(actual) == signature(expected)
            )

        except (ValueError, TypeError, KeyError) as exc:
            result["error"] = str(exc)

        results.append(result)

        print(
            f"EVAL {i}/8: {result['passed']}",
            flush=True,
        )

        path.write_text(
            json.dumps(results, indent=2) + "\n"
        )

    return sum(r["passed"] for r in results)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--steps", type=int, default=80)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not 1 <= args.steps <= 1000:
        parser.error("steps must be between 1 and 1000")

    if args.output.exists():
        parser.error("Output already exists; choose a new name")

    import torch
    from datasets import Dataset
    from transformers import (
        AutoTokenizer,
        AutoModelForCausalLM,
        GenerationConfig,
        set_seed,
    )
    from trl import SFTConfig, SFTTrainer

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")

    torch.set_num_threads(
        int(os.environ.get("SLURM_CPUS_PER_TASK") or "4")
    )
    set_seed(42)

    print(
        "TRAINING GPU:",
        torch.cuda.get_device_name(0),
        flush=True,
    )

    for example in EXAMPLES:
        signature(expected_call(*example[1:]))

    args.output.mkdir(parents=True)

    tokenizer = AutoTokenizer.from_pretrained(
        MODEL_ID, revision=REVISION
    )

    model = AutoModelForCausalLM.from_pretrained(
        MODEL_ID,
        revision=REVISION,
        dtype=torch.float32,
        attn_implementation="eager",
    ).to("cuda:0").eval()

    original_config = model.generation_config.to_dict()

    before = evaluate(
        model, tokenizer, args.output / "before.json"
    )

    records = []
    lengths = []

    for example in EXAMPLES:
        prompt, target, gesture, action, key = example
        call = expected_call(target, gesture, action, key)

        messages = messages_for(prompt)

        prefix = tokenizer.apply_chat_template(
            messages,
            tools=TOOLS,
            add_generation_prompt=True,
            tokenize=False,
        )

        messages.append({
            "role": "assistant",
            "tool_calls": [{
                "type": "function",
                "function": call,
            }],
        })

        full = tokenizer.apply_chat_template(
            messages,
            tools=TOOLS,
            add_generation_prompt=False,
            tokenize=False,
        )

        if not full.startswith(prefix):
            raise ValueError("Training format mismatch")

        lengths.append(len(
            tokenizer(
                full, add_special_tokens=False
            )["input_ids"]
        ))

        records.append({
            "prompt": prefix,
            "completion": full[len(prefix):],
        })

    config = SFTConfig(
        output_dir=str(args.output),
        max_length=max(lengths) + 8,
        packing=False,
        max_steps=args.steps,
        per_device_train_batch_size=1,
        gradient_checkpointing=False,
        optim="adamw_torch",
        learning_rate=5e-5,
        lr_scheduler_type="constant",
        logging_steps=10,
        save_strategy="no",
        eval_strategy="no",
        fp16=False,
        bf16=False,
        use_cpu=False,
        report_to="none",
        push_to_hub=False,
        dataloader_pin_memory=False,
        completion_only_loss=True,
        seed=42,
        disable_tqdm=True,
    )

    trainer = SFTTrainer(
        model=model,
        args=config,
        train_dataset=Dataset.from_list(records),
        processing_class=tokenizer,
    )

    metrics = trainer.train().metrics

    model.config.use_cache = True
    model.generation_config = GenerationConfig.from_dict(
        original_config
    )

    trainer.save_model(str(args.output / "model"))
    tokenizer.save_pretrained(args.output / "model")

    model.eval()

    after = evaluate(
        model, tokenizer, args.output / "after.json"
    )

    summary = {
        "before": before,
        "after": after,
        "total": len(EXAMPLES),
        "steps": args.steps,
        "training": metrics,
    }

    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n"
    )

    print(
        f"COMPLETE: {before}/8 -> {after}/8",
        flush=True,
    )


if __name__ == "__main__":
    main()
