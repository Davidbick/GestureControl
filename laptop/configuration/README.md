# Direct BLE configuration test

Install Bleak on Windows or Raspberry Pi:

```sh
python -m pip install -r laptop/configuration/requirements.txt
```

Check a tool call without connecting to BLE:

```sh
python laptop/configuration/ble_config.py laptop/configuration/test_set.json --dry-run
python -m unittest discover -s laptop/configuration -p "test_*.py"
```

To configure a receiver directly:

```sh
python laptop/configuration/ble_config.py laptop/configuration/test_set.json
python laptop/configuration/ble_config.py laptop/configuration/test_get.json
python laptop/configuration/ble_config.py laptop/configuration/test_reset.json
```

Use `test_light.json` to configure `MyoLight`.

## Batch configuration

The host API exposes four named calls: `set_gesture_mapping`,
`get_gesture_mapping`, `reset_gesture_mapping`, and
`configure_gesture_batch`. The batch call takes one `target` (`COMPUTER`
or `LIGHT`) and an array of 1–7 distinct gesture mappings. The
legacy top-level `{"target": ..., "mappings": [...]}` format is also supported.

Examples (dry-run first, then use the BLE receiver):

```sh
python laptop/configuration/ble_config.py laptop/configuration/test_batch_seven.json --dry-run
python laptop/configuration/ble_config.py laptop/configuration/test_tool_batch.json --dry-run
python laptop/configuration/ble_config.py laptop/configuration/test_tool_batch.json
```

All mappings are validated before connecting. One BLE connection is used for
the batch; every SET is checked and followed by a GET readback. On an error,
processing stops and reports verified vs. unverified gestures. A batch is
**not atomic** and does **not roll back** earlier changes. A single SET now
uses the same readback-verified path. A receiver running older five-gesture
firmware rejects requests requiring gesture IDs 6 or 7 before any writes.

`test_tool_batch.json` demonstrates the named function-call format. No
language-model inference is necessary to exercise the configuration API.

Receivers support two concurrent BLE connections. A laptop configuration client can
connect while either the wearable or Wio test controller owns one connection. If
both slots are already occupied, disconnect one controller before configuring.
The Wio's obsolete Gemma mode is no longer needed.

The four function definitions live in `protocol/functiongemma_tools.json`.
The host can dispatch validated named calls through `execute_tool_call()`.
Model inference and voice integration are separate future steps.
Configurations live in RAM and reset after a power cycle.
