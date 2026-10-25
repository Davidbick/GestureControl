# FunctionGemma through Transformers

This path follows [Google's full function-calling example](https://ai.google.dev/gemma/docs/functiongemma/full-function-calling-sequence-with-functiongemma)
using `google/functiongemma-270m-it`, `AutoProcessor`, `AutoModelForCausalLM`,
the official developer activation sentence, and `apply_chat_template`.
It has no Ollama dependency and never imports or executes BLE code.

From the GestureControl repository root in PowerShell:

```powershell
python -m venv .venv-transformers
.\.venv-transformers\Scripts\python.exe -m pip install -r laptop/functiongemma/requirements-transformers.txt
```

Accept Google's model license/access request at
[the official model repository](https://huggingface.co/google/functiongemma-270m-it),
then authenticate locally (do not put credentials in source or chat):

```powershell
.\.venv-transformers\Scripts\hf.exe auth login
.\.venv-transformers\Scripts\python.exe -u laptop/functiongemma/transformers_weather.py
```

The weather script uses Google's Tokyo prompt and mock `get_current_weather`
function (15 degrees Celsius, sunny). It validates the emitted call before
running that mock, then supplies the tool result using Google's structured
tool-message format and checks the final answer. It does not fetch real weather.
Model files download into the standard Hugging Face cache on the first run.

Differences from the illustrative notebook are deliberate: CPU float32, greedy
generation, a 128-token budget per weather turn, a 30-second generation time
limit, and strict parsing instead of regex extraction plus `globals()` dispatch.
`max_time` is checked between generation steps; it does not bound model download,
loading, or a single computation step. Incomplete generations fail rather than
being accepted as a valid prefix. Function and escape tokens are preserved in
decoding, and only the final generation-stop token is removed.

`transformers_backend.FunctionGemma.call` also accepts protocol JSON tool
definitions and returns one parsed call. Its result still requires the existing
`build_packet` / `build_batch` validators before any application use. It does not
execute calls or establish semantic correctness for arbitrary prompts.

Offline tests:

```powershell
.\.venv-transformers\Scripts\python.exe -m unittest discover -s laptop/functiongemma -p test_transformers.py -v
```

Results on 2026-10-25:

- Installed isolated environment: PyTorch 2.14.1, Transformers 5.20.2,
  Accelerate 1.15.0; `pip check` passed.
- Seven offline tests passed: official weather parsing, nested batch arguments,
  punctuation preservation, malformed/repeated calls, weather argument checks,
  generation bounds, and preservation of special delimiters.
- Initial model access was blocked by HTTP 401. After the user accepted access
  and authenticated locally, the official model downloaded and loaded successfully.
- **Live official weather sequence: PASS (exit 0).** Generated call:
  `get_current_weather(location="Tokyo, Japan")`. The mock returned
  `{"temperature":15,"weather":"sunny"}`. Final generated response:
  "The current weather in Tokyo is sunny with a temperature of 15 degrees Celsius."
- **Live GestureControl smoke tests: 0/2 (exit 1), safely rejected.** Single
  mapping produced `gesture="W"`, `action="SWIPE_UP"`, and unsolicited modifiers;
  the existing validator rejected `Unknown gesture: W`. Batch output failed
  strict parsing with `Trailing data or non-object arguments`. No output was
  repaired or executed. These results do not establish usable domain accuracy.
- The weather success isolates the previous empty/repeated weather failure to
  the tested Ollama/model-artifact/runtime path, rather than an inability of the
  official model to perform that task. It does not identify the precise Ollama
  defect, and the two stacks use different weight formats and precision.
- No BLE operations or push were performed.

Run the single/batch smoke tests (unchanged protocol schema and validators):

```powershell
.\.venv-transformers\Scripts\python.exe -u laptop/functiongemma/transformers_mappings.py
```

The default FunctionGemma model remains unsuitable for unattended GestureControl
execution on the evidence above. Task-specific fine-tuning or a separately
validated stronger model is required before enabling execution.
