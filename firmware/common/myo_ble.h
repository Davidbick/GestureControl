#ifndef MYO_BLE_H
#define MYO_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/bluetooth/uuid.h>

// Shared service identifying GestureControl receivers
#define MYO_SERVICE_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abcde)

// Writable characteristic for classifications
#define MYO_STATE_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abcdf)

#define MYO_GESTURE_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abce0)

#define MYO_BRIGHTNESS_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abce1)

#define GC_CONFIG_COMMAND_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abce4)

#define GC_CONFIG_RESPONSE_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abce5)

#define GC_CONFIG_INFO_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abce6)

// Temporary classification values
#define MYO_INVALID 0
#define MYO_VALID   1

// Shared gesture classifications
enum myo_gesture {
    MYO_GESTURE_NONE = 0,
    MYO_GESTURE_SWIPE_UP = 1,
    MYO_GESTURE_SWIPE_DOWN = 2,
    MYO_GESTURE_SWIPE_LEFT = 3,
    MYO_GESTURE_SWIPE_RIGHT = 4,
    MYO_GESTURE_TENSE = 5,
    MYO_GESTURE_EXTEND = 6, /* EMG0-dominant flex */
    MYO_GESTURE_FLEX = 7    /* EMG1-dominant flex */
};

// Two-byte BLE packet.
struct myo_gesture_packet {
    uint8_t gesture;
    uint8_t sequence;
};

// Two-byte brightness packet.
struct myo_brightness_packet {
    uint8_t percent;
    uint8_t sequence;
};

static inline bool myo_brightness_valid(uint8_t percent)
{
    return percent <= 100U && (percent % 25U) == 0U;
}


static inline bool myo_gesture_valid(uint8_t gesture)
{
    return gesture >= MYO_GESTURE_SWIPE_UP &&
           gesture <= MYO_GESTURE_FLEX;
}

#endif
