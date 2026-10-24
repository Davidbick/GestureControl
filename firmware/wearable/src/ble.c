#include "ble.h"
#include "myo_ble_client.h"

int ble_init(void)
{
    return myo_ble_client_init();
}

int ble_send_state(uint8_t state)
{
    return myo_ble_client_send_state(state);
}