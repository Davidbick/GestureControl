"""Google's FunctionGemma Transformers path. No Ollama or BLE dependency."""
import re
from dataclasses import dataclass

MODEL_ID = "google/functiongemma-270m-it"
ACTIVATION = "You are a model that can do function calling with the following functions"


def parse_call(text):
    """Parse exactly one complete call, including nested batch arguments.

    Strings must use FunctionGemma's escape delimiters. Reject duplicate keys,
    trailing text/calls, and malformed values rather than salvaging a prefix.
    """
    match = re.fullmatch(r"\s*<start_function_call>call:([A-Za-z_]\w*)(.*)<end_function_call>\s*", text, re.S)
    if not match:
        raise ValueError("Expected exactly one complete FunctionGemma call")
    body = match[2]
    pos = 0

    def whitespace():
        nonlocal pos
        while pos < len(body) and body[pos].isspace():
            pos += 1

    def take(token):
        nonlocal pos
        whitespace()
        if not body.startswith(token, pos):
            raise ValueError(f"Expected {token!r} at offset {pos}")
        pos += len(token)

    def value(depth=0):
        nonlocal pos
        whitespace()
        if depth > 16:
            raise ValueError("Excessive nesting")
        if body.startswith('<escape>', pos):
            pos += 8
            end = body.find('<escape>', pos)
            if end < 0:
                raise ValueError("Unterminated string")
            result = body[pos:end]
            pos = end + 8
            return result
        if body.startswith('{', pos):
            pos += 1
            result = {}
            whitespace()
            if body.startswith('}', pos):
                pos += 1
                return result
            while True:
                whitespace()
                key = re.match(r'[A-Za-z_]\w*', body[pos:])
                if not key:
                    raise ValueError("Invalid argument key")
                key = key[0]
                pos += len(key)
                if key in result:
                    raise ValueError("Duplicate argument key")
                take(':')
                result[key] = value(depth + 1)
                whitespace()
                if body.startswith('}', pos):
                    pos += 1
                    return result
                take(',')
        if body.startswith('[', pos):
            pos += 1
            result = []
            whitespace()
            if body.startswith(']', pos):
                pos += 1
                return result
            while True:
                result.append(value(depth + 1))
                whitespace()
                if body.startswith(']', pos):
                    pos += 1
                    return result
                take(',')
        literal = re.match(r'(true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)', body[pos:])
        if not literal:
            raise ValueError("Invalid value; strings require <escape> delimiters")
        import json
        pos += len(literal[0])
        return json.loads(literal[0])

    arguments = value()
    whitespace()
    if pos != len(body) or not isinstance(arguments, dict):
        raise ValueError("Trailing data or non-object arguments")
    return {'name': match[1], 'arguments': arguments}


@dataclass
class Generation:
    text: str
    tokens: int


class FunctionGemma:
    def __init__(self, model_id=MODEL_ID):
        import torch
        from transformers import AutoProcessor, AutoModelForCausalLM
        self.processor = AutoProcessor.from_pretrained(model_id)
        # Explicit CPU float32 avoids CPU half-precision compatibility issues.
        self.model = AutoModelForCausalLM.from_pretrained(
            model_id, dtype=torch.float32, device_map='cpu',
        ).eval()

    def generate(self, messages, tools, max_new_tokens=128):
        import torch
        if not 1 <= max_new_tokens <= 256:
            raise ValueError("Token limit must be between 1 and 256")
        inputs = self.processor.apply_chat_template(
            messages, tools=tools, add_generation_prompt=True,
            return_dict=True, return_tensors='pt',
        ).to(self.model.device)
        with torch.inference_mode():
            output = self.model.generate(
                **inputs, pad_token_id=self.processor.eos_token_id,
                max_new_tokens=max_new_tokens, max_time=30, do_sample=False,
            )
        tokens = output[0][inputs['input_ids'].shape[-1]:].tolist()
        eos = self.model.generation_config.eos_token_id
        eos = [eos] if isinstance(eos, int) else (eos or [])
        if not tokens or tokens[-1] not in eos or len(tokens) >= max_new_tokens:
            raise ValueError("Generation did not complete within token/time budget")
        # Remove only the terminal EOS, preserving function/escape delimiters.
        text = self.processor.decode(tokens[:-1], skip_special_tokens=False)
        return Generation(text=text, tokens=len(tokens))

    def call(self, prompt, tools, instructions=ACTIVATION):
        messages = [{'role':'developer','content':instructions},
                    {'role':'user','content':prompt}]
        generated = self.generate(messages, tools, max_new_tokens=256)
        call = parse_call(generated.text)
        names = {tool['function']['name'] for tool in tools}
        if call['name'] not in names:
            raise ValueError("Unknown tool")
        return call
