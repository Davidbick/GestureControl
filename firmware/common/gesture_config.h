#ifndef GESTURE_CONFIG_H
#define GESTURE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/gatt.h>

#define GC_CONFIG_VERSION 1
#define GC_CONFIG_PACKET_SIZE 12
#define GC_GESTURE_COUNT 5

#define GC_DEFAULT_KEY_DURATION_MS 35
#define GC_DEFAULT_MOUSE_STEP 40
#define GC_DEFAULT_SCROLL_STEP 1
#define GC_DEFAULT_BRIGHTNESS_STEP 25

#define GC_MOD_CTRL  0x01
#define GC_MOD_SHIFT 0x02
#define GC_MOD_ALT   0x04
#define GC_MOD_GUI   0x08

enum gc_target {
    GC_TARGET_COMPUTER = 1,
    GC_TARGET_LIGHT = 2
};

enum gc_operation {
    GC_OP_GET = 1,
    GC_OP_SET = 2,
    GC_OP_RESET = 3,
    GC_OP_RESET_ALL = 4
};

enum gc_action {
    GC_ACTION_NONE = 0,
    GC_ACTION_KEY_TAP = 1,
    GC_ACTION_MOUSE_LEFT_CLICK = 2,
    GC_ACTION_MOUSE_RIGHT_CLICK = 3,
    GC_ACTION_MOUSE_MIDDLE_CLICK = 4,
    GC_ACTION_MOUSE_MOVE_UP = 5,
    GC_ACTION_MOUSE_MOVE_DOWN = 6,
    GC_ACTION_MOUSE_MOVE_LEFT = 7,
    GC_ACTION_MOUSE_MOVE_RIGHT = 8,
    GC_ACTION_MOUSE_SCROLL_UP = 9,
    GC_ACTION_MOUSE_SCROLL_DOWN = 10,
    GC_ACTION_LIGHT_TOGGLE = 11,
    GC_ACTION_LIGHT_ON = 12,
    GC_ACTION_LIGHT_OFF = 13,
    GC_ACTION_LIGHT_SET_BRIGHTNESS = 14,
    GC_ACTION_LIGHT_BRIGHTNESS_UP = 15,
    GC_ACTION_LIGHT_BRIGHTNESS_DOWN = 16,
    GC_ACTION_MOUSE_DOUBLE_CLICK = 17,
    GC_ACTION_KEY_TOGGLE = 18
};

enum gc_status {
    GC_STATUS_OK = 0,
    GC_STATUS_BAD_VERSION = 1,
    GC_STATUS_WRONG_TARGET = 2,
    GC_STATUS_INVALID_GESTURE = 3,
    GC_STATUS_UNSUPPORTED_ACTION = 4,
    GC_STATUS_INVALID_PARAMETER = 5,
    GC_STATUS_BUSY = 6,
    GC_STATUS_INVALID_OPERATION = 7
};

struct gc_mapping {
    uint8_t action;
    uint8_t keycode;
    uint8_t modifiers;
    uint8_t amount;
    uint16_t duration_ms;
};

struct gc_config_request {
    uint8_t version;
    uint8_t transaction_id;
    uint8_t operation;
    uint8_t target;
    uint8_t gesture;
    uint8_t action;
    uint8_t keycode;
    uint8_t modifiers;
    uint8_t amount;
    uint8_t duration_lo;
    uint8_t duration_hi;
    uint8_t flags;
};

struct gc_config_response {
    uint8_t version;
    uint8_t transaction_id;
    uint8_t operation;
    uint8_t target;
    uint8_t gesture;
    uint8_t status;
    uint8_t action;
    uint8_t keycode;
    uint8_t modifiers;
    uint8_t amount;
    uint8_t duration_lo;
    uint8_t duration_hi;
};

_Static_assert(sizeof(struct gc_config_request) == GC_CONFIG_PACKET_SIZE,
               "Invalid configuration request size");
_Static_assert(sizeof(struct gc_config_response) == GC_CONFIG_PACKET_SIZE,
               "Invalid configuration response size");

void gc_config_init(enum gc_target target);
struct gc_mapping gc_config_get(uint8_t gesture);
uint32_t gc_config_revision(void);

ssize_t gc_config_write(struct bt_conn *conn,
                        const struct bt_gatt_attr *attr,
                        const void *buf,
                        uint16_t len,
                        uint16_t offset,
                        uint8_t flags);

ssize_t gc_config_read_response(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                void *buf,
                                uint16_t len,
                                uint16_t offset);

ssize_t gc_config_read_info(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf,
                            uint16_t len,
                            uint16_t offset);

#endif
