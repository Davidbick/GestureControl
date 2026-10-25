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

Receivers support two concurrent BLE connections. A laptop configuration client can
connect while either the wearable or Wio test controller owns one connection. If
both slots are already occupied, disconnect one controller before configuring.
The Wio's obsolete Gemma mode is no longer needed.

The three function definitions live in `protocol/functiongemma_tools.json`.
This client accepts that format: FunctionGemma can invoke `execute_tool_call()`
after parsing/validating output. Configurations live in RAM and reset after a
power cycle.
