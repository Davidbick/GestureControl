#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include "myo_ble_client.h"
#include "myo_ble.h"
#include "rgb_led.h"

LOG_MODULE_REGISTER(wio_controller, LOG_LEVEL_INF);

#define TRACKBALL_NODE DT_PATH(trackball)
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

K_MSGQ_DEFINE(key_queue, sizeof(struct key_event), 16, 4);

static const struct device *const trackball = DEVICE_DT_GET(TRACKBALL_NODE);

static const enum myo_gesture gesture_map[KEY_COUNT] = {
    [KEY_UP] = MYO_GESTURE_SWIPE_UP,
    [KEY_DOWN] = MYO_GESTURE_SWIPE_DOWN,
    [KEY_LEFT] = MYO_GESTURE_SWIPE_LEFT,
    [KEY_RIGHT] = MYO_GESTURE_SWIPE_RIGHT,
    [KEY_PRESS] = MYO_GESTURE_TENSE,
};

static const char *const gesture_names[KEY_COUNT] = {
    [KEY_UP] = "SWIPE_UP",
    [KEY_DOWN] = "SWIPE_DOWN",
    [KEY_LEFT] = "SWIPE_LEFT",
    [KEY_RIGHT] = "SWIPE_RIGHT",
    [KEY_PRESS] = "TENSE",
};

static bool key_held[KEY_COUNT];
static bool direction_armed = true;
static int64_t last_direction_ms = -EVENT_COOLDOWN_MS;

static int key_to_index(uint16_t code)
{
    switch (code) {
    case INPUT_KEY_UP: return KEY_UP;
    case INPUT_KEY_DOWN: return KEY_DOWN;
    case INPUT_KEY_LEFT: return KEY_LEFT;
    case INPUT_KEY_RIGHT: return KEY_RIGHT;
    case INPUT_KEY_ENTER: return KEY_PRESS;
    default: return -1;
    }
}

static void trackball_callback(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    if (evt->type != INPUT_EV_KEY || key_to_index(evt->code) < 0) {
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

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TRACKBALL_NODE), trackball_callback, NULL);

static bool all_directions_released(void)
{
    for (int i = KEY_UP; i <= KEY_RIGHT; i++) {
        if (key_held[i]) {
            return false;
        }
    }
    return true;
}

static void send_gesture(int index)
{
    if (!myo_ble_client_gesture_ready()) {
        LOG_WRN("%s ignored: BLE not ready", gesture_names[index]);
        return;
    }

    int err = myo_ble_client_send_gesture(gesture_map[index]);
    if (err) {
        LOG_WRN("%s send failed: %d", gesture_names[index], err);
        return;
    }
    LOG_INF("Sent: %s", gesture_names[index]);
}

static void handle_key_event(const struct key_event *event)
{
    int index = key_to_index(event->code);
    if (index < 0 || key_held[index] == event->pressed) {
        return;
    }

    key_held[index] = event->pressed;
    int64_t now = k_uptime_get();

    if (index == KEY_PRESS) {
        // Center sends TENSE once per press/release, regardless of hold time.
        if (!event->pressed) {
            send_gesture(KEY_PRESS);
        }
        return;
    }

    if (!event->pressed) {
        if (all_directions_released()) {
            direction_armed = true;
        }
        return;
    }

    if (!direction_armed) {
        return;
    }
    direction_armed = false;

    if (key_held[KEY_PRESS] || now - last_direction_ms < EVENT_COOLDOWN_MS) {
        return;
    }

    last_direction_ms = now;
    send_gesture(index);
}

int main(void)
{
    LOG_INF("GestureControl Wio test controller starting");

    if (!device_is_ready(trackball)) {
        LOG_ERR("Trackball not ready");
        return 0;
    }

    int rgb_err = wio_rgb_init();
    if (rgb_err) {
        LOG_WRN("RGB not available: %d", rgb_err);
    }

    // Normal test-controller operation: stay connected to MyoPC.
    int err = myo_ble_client_init_mode(MYO_MODE_COMPUTER);
    if (err) {
        LOG_ERR("BLE initialization failed: %d", err);
        return 0;
    }

    LOG_INF("Trackball sends computer gestures. Voice is laptop-only.");
    while (1) {
        struct key_event event;
        if (k_msgq_get(&key_queue, &event, K_FOREVER) == 0) {
            handle_key_event(&event);
        }
    }
    return 0;
}
