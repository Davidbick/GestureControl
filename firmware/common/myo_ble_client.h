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

#endif