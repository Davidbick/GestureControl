#ifndef BLE_H
#define BLE_H

#include <stdint.h>

int ble_init(void);
int ble_send_state(uint8_t state);

#endif
