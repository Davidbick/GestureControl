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

## Batch configuration (one BLE connection)

Create JSON with `"target": "COMPUTER"` or `"LIGHT"` and a `"mappings"` array of 1 to 5 unique gestures. Each entry has the same parameters as the single SET operation, except `target` is supplied at the top level. See `test_batch.json`.

```sh
python laptop/configuration/ble_config.py laptop/configuration/test_batch.json --dry-run
python laptop/configuration/ble_config.py laptop/configuration/test_batch.json
```

The script validates every entry before connecting, sends SET and GET for each gesture over one BLE link, and verifies the full mapping bytes. It stops on the first unverified operation and reports successes and uncertainty; operations are **not atomic**, and no automatic rollback occurs. The legacy single-command format is unchanged.

With firmware from `added dual-connection receivers`, the target should remain discoverable with the wearable/test controller connected; there is no need to switch the controller to Gemma mode to free MyoPC.

The three function definitions live in `protocol/functiongemma_tools.json`. This client supports the existing single function-call format and the new batch JSON format; FunctionGemma integration can select either route after validating its output. Receiver configurations live only in RAM, so they reset after a power cycle.
