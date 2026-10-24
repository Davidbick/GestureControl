#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include "myo_ble_client.h"
#include "myo_ble.h"

LOG_MODULE_REGISTER(wio_controller, LOG_LEVEL_INF);

#define TRACKBALL_NODE DT_PATH(trackball)

// Suppress extra events from one trackball movement.
#define EVENT_COOLDOWN_MS 200

enum key_index {
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PRESS,
    KEY_COUNT
};

struct key_event {
    uint16_t code;
    bool pressed;
};

// The input callback queues events instead of doing
// Bluetooth operations inside the callback.
K_MSGQ_DEFINE(key_queue, sizeof(struct key_event), 16, 4);

static const struct device *const trackball =
    DEVICE_DT_GET(TRACKBALL_NODE);

static const enum myo_gesture gesture_map[KEY_COUNT] = {
    [KEY_UP]    = MYO_GESTURE_SWIPE_UP,
    [KEY_DOWN]  = MYO_GESTURE_SWIPE_DOWN,
    [KEY_LEFT]  = MYO_GESTURE_SWIPE_LEFT,
    [KEY_RIGHT] = MYO_GESTURE_SWIPE_RIGHT,
    [KEY_PRESS] = MYO_GESTURE_TENSE,
};

static const char *const gesture_names[KEY_COUNT] = {
    [KEY_UP]    = "SWIPE_UP",
    [KEY_DOWN]  = "SWIPE_DOWN",
    [KEY_LEFT]  = "SWIPE_LEFT",
    [KEY_RIGHT] = "SWIPE_RIGHT",
    [KEY_PRESS] = "TENSE",
};

// Convert Zephyr key codes to our internal indexes.
static int key_to_index(uint16_t code)
{
    switch (code) {
    case INPUT_KEY_UP:
        return KEY_UP;

    case INPUT_KEY_DOWN:
        return KEY_DOWN;

    case INPUT_KEY_LEFT:
        return KEY_LEFT;

    case INPUT_KEY_RIGHT:
        return KEY_RIGHT;

    case INPUT_KEY_ENTER:
        return KEY_PRESS;

    default:
        return -1;
    }
}

// Called by Zephyr's input subsystem.
static void trackball_callback(
    struct input_event *evt,
    void *user_data)
{
    ARG_UNUSED(user_data);

    if (evt->type != INPUT_EV_KEY ||
        key_to_index(evt->code) < 0) {
        return;
    }

    struct key_event event = {
        .code = evt->code,
        .pressed = evt->value != 0
    };

    if (k_msgq_put(&key_queue, &event, K_NO_WAIT) != 0) {
        LOG_WRN("Input queue full");
    }
}

// Listen only to the Wio trackball.
INPUT_CALLBACK_DEFINE(
    DEVICE_DT_GET(TRACKBALL_NODE),
    trackball_callback,
    NULL
);

// Gesture event handling
static bool key_held[KEY_COUNT];
static bool armed = true;
static int64_t last_event_ms = -EVENT_COOLDOWN_MS;

static bool all_keys_released(void)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        if (key_held[i]) {
            return false;
        }
    }

    return true;
}

static void handle_key_event(const struct key_event *event)
{
    int index = key_to_index(event->code);

    if (index < 0) {
        return;
    }

    // Ignore duplicate key state reports.
    if (key_held[index] == event->pressed) {
        return;
    }

    key_held[index] = event->pressed;

    // A release rearms the controller only when
    // every direction/button has been released.
    if (!event->pressed) {
        if (all_keys_released()) {
            armed = true;
        }

        return;
    }

    // One event per press/release cycle.
    if (!armed) {
        return;
    }

    armed = false;

    int64_t now = k_uptime_get();

    // Prevent several pulses from one rolling motion
    // from becoming several BLE gesture commands.
    if (now - last_event_ms < EVENT_COOLDOWN_MS) {
        LOG_DBG("Trackball event suppressed");
        return;
    }

    last_event_ms = now;

    enum myo_gesture gesture = gesture_map[index];

    if (!myo_ble_client_gesture_ready()) {
        LOG_WRN("%s ignored: BLE not ready",
                gesture_names[index]);
        return;
    }

    int err = myo_ble_client_send_gesture(gesture);

    if (err) {
        LOG_WRN("%s send failed: %d",
                gesture_names[index], err);
        return;
    }

    LOG_INF("Sent: %s", gesture_names[index]);
}

int main(void)
{
    LOG_INF("GestureControl Wio test controller");

    if (!device_is_ready(trackball)) {
        LOG_ERR("Trackball input device not ready");
        return 0;
    }

    int err = myo_ble_client_init();

    if (err) {
        LOG_ERR("BLE initialization failed: %d", err);
        return 0;
    }

    LOG_INF("Trackball controller ready");

    while (1) {
        struct key_event event;

        if (k_msgq_get(&key_queue, &event, K_FOREVER) == 0) {
            handle_key_event(&event);
        }
    }

    return 0;
}