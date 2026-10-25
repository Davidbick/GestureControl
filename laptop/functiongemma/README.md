# FunctionGemma investigation — 2026-10-25

Status update: Google's Transformers weather sequence now passes after local
Hugging Face authentication. Transformers GestureControl single/batch smoke tests
still fail safely (0/2). See [TRANSFORMERS.md](TRANSFORMERS.md) for the implementation,
commands, and results. The Ollama investigation below records the earlier run.

Ollama status: offline safeguards pass; live FunctionGemma inference fails. Do not enable
BLE execution or treat this as an end-to-end passing implementation. Nothing was
pushed. All changes are confined to this previously untracked inference directory;
the protocol schema, encoder, firmware, and BLE executor were not changed.

## Findings established before editing

- Ollama client and server: **0.40.11**. Ollama Python SDK: **0.6.3**, Python
  **3.13**, global `C:\Python313\python.exe`. The repository `.venv` did not contain
  Ollama. `rich` was installed into the current user's Python packages to run the
  exact official example.
- Model: `functiongemma:latest`, digest
  `7c19b650567acfb1bee50d12bb286370e8a54d8877f6a0ecfd01f8c8fdc223bb`, Q8_0,
  268.10M parameters, CPU execution, 4096 runtime context.
- `ollama show functiongemma --modelfile` reports `TEMPLATE {{ .Prompt }}`,
  `RENDERER functiongemma`, and `PARSER functiongemma`. This is a native renderer
  model; the short template alone is not evidence of a missing chat template.
- The model blob SHA256 matches its filename:
  `415f8f959d807bd4d4da891f01225d7b330416947fb011a8473080ae4fd07885`.
  This establishes local blob integrity, not inference correctness.
- Stop-token metadata is present: IDs 1 (`<eos>`), 50
  (`<start_function_response>`), and 106 (`<end_of_turn>`).
  `<end_function_call>` is ID 49 and is not an end-of-generation token. Do not
  add it as an API stop string: removing that delimiter can prevent parsing.
- Ran the exact Python example copied from Ollama's FunctionGemma model page
  before changing application code. It exceeded a 60-second outer subprocess
  deadline. The example itself has no generation bound. This was a timeout,
  not a successful weather call.
- The original local weather script reproduced the reported empty result:
  `tool_calls=None`, empty content, 12 generated tokens, about 0.26 seconds.
- Bounded probes with the official weather tool description: no explicit
  developer message produced seven empty-name calls and hit 128 tokens;
  Google's exact activation message with `role=developer` produced empty text
  and no calls after 61 tokens; `role=system` hit 128 tokens with no calls.
- A raw `/api/generate` probe using Google's declaration/control-token format,
  bypassing native chat rendering and tool-call extraction, repeated
  `call:get_weather{city: Paris}` until the 128-token bound. The raw endpoint can
  suppress special tokens in displayed text, so absence of those tokens alone
  is not proof of tokenizer corruption. Repeated generation is independent of
  the application's tool-call loop.
- The original mapping harness failed all five scenarios. It exposed only
  `TOOLS[0]` (set), making batch/get/reset requests impossible to satisfy through
  the advertised tool interface. Its detailed `SYSTEM` instructions were unused.
  For custom prompts it could accept multiple individually valid calls and did
  not reject generation ending at the token limit.

## Root-cause boundary

There are two separate defects: the application advertises an incomplete API and
does not strictly gate complete responses; independently, this installed
FunctionGemma/Ollama stack fails even the official weather example. Changing
the application prompt cannot explain or fix that independent failure. Merely
reading `message.content` is also insufficient: successful tool responses can
have empty content, but the reproduced failures had no usable `tool_calls`.

The exact runtime/model-conversion defect remains **unproven**. It would be
incorrect to claim an old Ollama version, missing developer role, broken model
download, or a parser-only defect as the sole cause. Current upstream Ollama
renderer source supplies a developer activation message automatically; current
parser source buffers unfinished calls and can emit empty-name calls for malformed
bodies. These mechanisms explain how generated tokens can produce an empty or
invalid API result, but upstream `main` is not a verified source match for the
installed binary. The attempted version-tag source fetch was unavailable.

## Changes

- Use Google's exact activation sentence in a developer message followed by the
  existing domain instructions, and advertise all four unchanged protocol tools.
- Set temperature 0, seed 42, a 256-token mapping limit, and a 30-second HTTP
  timeout. Weather uses 128 tokens. No retries or multi-step generation loop.
- Require a completed, normally stopped response with exactly one native call,
  no accompanying text, a known function, and object arguments. Reject exhausted
  token budgets even when a valid-looking prefix exists.
- Validate with the existing `build_packet` / `build_batch` functions. Never repair
  enum values, salvage malformed text with regex, silently deduplicate calls, or
  execute BLE. Batch validation includes every entry before accepting the call.
- Keep expected-behavior checks separate from schema/protocol validity. A valid
  call can still express the wrong intent. Custom `--prompt` inputs have no
  expected-intent oracle and remain dry-run diagnostics only.
- Add import-safe weather diagnostics and offline regression tests. The pristine
  official example is retained separately; run it only through the bounded wrapper.

## Results

| Check | Result |
| --- | --- |
| Exact official example | Timed out at 60 seconds |
| Original mapping harness | 0/5 |
| New offline guardrails | 11/11 pass |
| Existing protocol suite | 27/27 pass; BLE objects mocked |
| Corrected bounded weather | Fail: 128 tokens, `done_reason=length`, no calls |
| Corrected live single COMPUTER mapping | Rejected: 256 tokens, malformed calls |
| Corrected live batch mapping | Rejected: 256 tokens, malformed/repeated calls |
| Corrected live LIGHT mapping | Rejected: prose refusal, no call (32 tokens) |
| Corrected live GET / RESET | Both rejected at 256 tokens |
| Corrected live suite | **0/5**, exits 1 |

Single and batch expected fixtures pass the real Python encoders, including wire
payload comparisons. Those fixture tests do **not** imply successful model
inference. The live single and batch outputs fail before acceptance. No Bluetooth
connection, scan, or hardware action occurred.

Run from the repository root using the Python environment containing Ollama:

```powershell
python -m unittest discover -s laptop/functiongemma -p test_guardrails.py -v
python -m unittest discover -s laptop/configuration -p test_protocol.py -v
python laptop/functiongemma/test_basic.py
python laptop/functiongemma/test_inference.py
python laptop/functiongemma/reproduce_official.py
```

## Supported alternative

The fastest supported **path to isolate and bypass this Ollama failure** is
Google's documented Hugging Face Transformers route for
`google/functiongemma-270m-it`: `AutoProcessor.apply_chat_template` with the exact
developer activation, `AutoModelForCausalLM`, deterministic bounded generation,
and strict parsing into the same protocol calls and validators. It needs model
license acceptance and installation of Transformers/PyTorch; neither was installed
in the inspected Python environment. No gated model access or large download was
attempted. This recommendation is about the shortest supported integration path,
not a measured latency win or a guarantee of zero-shot mapping accuracy.

For an Ollama-native replacement, `qwen3:0.6b` is an officially listed small
tool-capable candidate; disable thinking and run these exact single/batch gates
before adopting it. Its speed and accuracy have not been measured here, so no
claim that it is the fastest reliable model is warranted. Google describes
FunctionGemma as a base for task-specific fine-tuning; reliable domain mappings
may require fine-tuning even after the runtime problem is resolved.

## Official sources

- [Ollama FunctionGemma example and minimum version](https://ollama.com/library/functiongemma)
- [Google full FunctionGemma calling sequence](https://ai.google.dev/gemma/docs/functiongemma/full-function-calling-sequence-with-functiongemma)
- [Google formatting and supported workflows](https://ai.google.dev/gemma/docs/functiongemma/formatting-and-best-practices)
- [Google FunctionGemma overview and fine-tuning guidance](https://ai.google.dev/gemma/docs/functiongemma)
- [Ollama renderer source (main)](https://github.com/ollama/ollama/blob/main/model/renderers/functiongemma.go)
- [Ollama parser source (main)](https://github.com/ollama/ollama/blob/main/model/parsers/functiongemma.go)
- [Ollama Qwen3 0.6B](https://ollama.com/library/qwen3:0.6b)
