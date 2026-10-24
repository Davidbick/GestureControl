#ifndef MYO_BLE_H
#define MYO_BLE_H

#include <zephyr/bluetooth/uuid.h>

// Shared service identifying GestureControl receivers
#define MYO_SERVICE_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abcde)

// Writable characteristic for classifications
#define MYO_STATE_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abcdf)

// Temporary classification values
#define MYO_INVALID 0
#define MYO_VALID   1

#endif
