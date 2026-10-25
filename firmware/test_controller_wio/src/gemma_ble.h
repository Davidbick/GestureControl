#ifndef GESTURE_GEMMA_BLE_H
#define GESTURE_GEMMA_BLE_H

#include <stdbool.h>

/* Start advertising for the Gemma laptop. */
int gemma_ble_start(void);

/* Stop advertising and disconnect the laptop. */
int gemma_ble_stop(void);

/* Restart advertising if the laptop disconnects. */
int gemma_ble_poll(void);

/* True after all Gemma BLE activity has stopped. */
bool gemma_ble_idle(void);

#endif