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

The target must be advertising and free: put the Wio controller in Gemma mode to release `MyoPC`. If `MyoLight` is currently connected to the wearable, disconnect that connection first. After configuration, disconnect the laptop and return the Wio to Laptop mode to exercise the updated HID mapping.

The three function definitions live in `protocol/functiongemma_tools.json`. This client already accepts that function call format, so FunctionGemma can invoke `execute_tool_call()` after parsing and validating its output. Receiver configurations live only in RAM, so they reset after a power cycle.
