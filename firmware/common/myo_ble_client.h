#ifndef MYO_BLE_CLIENT_H
#define MYO_BLE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "myo_ble.h"

// Initialize BLE central and start scanning.
int myo_ble_client_init(void);

// Compatibility with existing wearable.
int myo_ble_client_send_state(uint8_t state);

// Send one classified gesture event.
int myo_ble_client_send_gesture(enum myo_gesture gesture);

// True once the new gesture characteristic is found.
bool myo_ble_client_gesture_ready(void);

// Send quantized brightness, 0 to 100 percent.
int myo_ble_client_send_brightness(uint8_t percent);

// True when the receiver's brightness
// characteristic has been discovered.
bool myo_ble_client_brightness_ready(void);

enum myo_receiver_mode {
    MYO_MODE_LIGHT = 0,
    MYO_MODE_COMPUTER = 1
};

// Disconnect and connect to the other receiver type.
int myo_ble_client_toggle_mode(void);

// Thread-safe selected receiver mode, independent of connection state.
enum myo_receiver_mode myo_ble_client_get_mode(void);

#endif