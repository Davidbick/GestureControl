
# GestureControl TV Navigation

Chrome extension for TV-style navigation using the
GestureControl wearable or Wio test controller.

## Architecture

Wio / Wearable
    → BLE
    → nRF52840 Receiver
    → USB HID Keyboard
    → Chrome Extension

## Main Components

- content.js — Loads browser navigation components
- hid-input.js — Converts HID keys into navigation actions
- core.js — Spatial navigation and element selection
- virtual-keyboard.js — On-screen text entry
- sites/ — Website-specific navigation configurations
- popup.js — Extension settings
- background.js — Browser-level actions

## Controls

- Arrows — Navigate
- Enter — Select or activate

Back, Home, and fallback pointer controls will be
added in later development stages.

## Installation

1. Open chrome://extensions
2. Enable Developer mode
3. Select Load unpacked
4. Choose laptop/chrome_extension

## Attribution

The navigation engine is adapted from Couch Browser.

See THIRD_PARTY_NOTICES.md in the repository root
for original source information and licensing status.
