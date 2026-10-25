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
#include "rgb_led.h"
#include "gemma_ble.h"

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

// -----------------------------------------------------
// GestureControl mode switching
// -----------------------------------------------------

#define MODE_HOLD_MS 3000
#define MODE_RETRY_MS 500

enum wio_mode {
    WIO_MODE_LAPTOP,
    WIO_MODE_GEMMA
};

enum mode_phase {
    MODE_READY,
    MODE_STOP_OLD,
    MODE_WAIT_DISCONNECT,
    MODE_START_NEW
};

static enum wio_mode current_mode = WIO_MODE_LAPTOP;
static enum wio_mode requested_mode = WIO_MODE_LAPTOP;

static enum mode_phase mode_phase = MODE_READY;

static int64_t next_mode_retry_ms;

static bool rgb_ready;

// -----------------------------------------------------
// Trackball button and directional state
// -----------------------------------------------------

static bool key_held[KEY_COUNT];

static bool direction_armed = true;
static int64_t last_direction_ms = -EVENT_COOLDOWN_MS;

static int64_t press_started_ms;
static bool long_press_consumed;

static bool all_directions_released(void)
{
    for (int i = KEY_UP; i <= KEY_RIGHT; i++) {
        if (key_held[i]) {
            return false;
        }
    }

    return true;
}

// Send gesture only in active Laptop mode
static void send_gesture(int index)
{
    if (current_mode != WIO_MODE_LAPTOP ||
        mode_phase != MODE_READY) {
        return;
    }

    if (!myo_ble_client_gesture_ready()) {
        LOG_WRN("%s ignored: BLE not ready",
                gesture_names[index]);
        return;
    }

    enum myo_gesture gesture = gesture_map[index];

    int err = myo_ble_client_send_gesture(gesture);

    if (err) {
        LOG_WRN("%s send failed: %d",
                gesture_names[index], err);
        return;
    }

    LOG_INF("Sent: %s", gesture_names[index]);
}

// Request mode toggle
static void request_mode_toggle(void)
{
    if (mode_phase != MODE_READY) {
        LOG_WRN("Mode switch already in progress");
        return;
    }

    requested_mode =
        current_mode == WIO_MODE_LAPTOP
            ? WIO_MODE_GEMMA
            : WIO_MODE_LAPTOP;

    mode_phase = MODE_STOP_OLD;
    next_mode_retry_ms = 0;

    LOG_INF("Requested mode: %s",
        requested_mode == WIO_MODE_GEMMA
            ? "GEMMA" : "LAPTOP");
}

// Trackball events
static void handle_key_event(const struct key_event *event)
{
    int index = key_to_index(event->code);

    if (index < 0) {
        return;
    }

    // Ignore duplicate press/release reports.
    if (key_held[index] == event->pressed) {
        return;
    }

    key_held[index] = event->pressed;

    int64_t now = k_uptime_get();

    // Center trackball button
    if (index == KEY_PRESS) {

        if (event->pressed) {
            press_started_ms = now;
            long_press_consumed = false;
            return;
        }

        // Released after a long hold.
        if (long_press_consumed) {
            return;
        }

        int64_t held_ms = now - press_started_ms;

        if (held_ms >= MODE_HOLD_MS) {
            long_press_consumed = true;
            request_mode_toggle();
            return;
        }

        // Short press = TENSE in Laptop mode.
        send_gesture(KEY_PRESS);
        return;
    }

    // Directional trackball input
    if (!event->pressed) {
        if (all_directions_released()) {
            direction_armed = true;
        }

        return;
    }

    // Only one action per direction press cycle.
    if (!direction_armed) {
        return;
    }

    direction_armed = false;

    // Ignore directional control while center is held.
    if (key_held[KEY_PRESS]) {
        return;
    }

    // Ignore directions outside Laptop mode.
    if (current_mode != WIO_MODE_LAPTOP ||
        mode_phase != MODE_READY) {
        return;
    }

    if (now - last_direction_ms < EVENT_COOLDOWN_MS) {
        return;
    }

    last_direction_ms = now;

    send_gesture(index);
}

// Detect 3-second hold without waiting for release
static void poll_button(void)
{
    if (!key_held[KEY_PRESS] ||
        long_press_consumed) {
        return;
    }

    int64_t held_ms =
        k_uptime_get() - press_started_ms;

    if (held_ms >= MODE_HOLD_MS) {
        long_press_consumed = true;

        LOG_INF("Long press detected");

        request_mode_toggle();
    }
}

// Execute safe Bluetooth handoffs
static void poll_mode_switch(void)
{
    if (mode_phase == MODE_READY) {

        // If the laptop disconnects during Gemma mode,
        // advertise again so it can reconnect.
        if (current_mode == WIO_MODE_GEMMA) {
            gemma_ble_poll();
        }

        return;
    }

    int64_t now = k_uptime_get();

    if (now < next_mode_retry_ms) {
        return;
    }

    int err = 0;

    // Stop the previous Bluetooth role first.
    if (mode_phase == MODE_STOP_OLD) {

        if (current_mode == WIO_MODE_LAPTOP) {
            err = myo_ble_client_pause();
        } else {
            err = gemma_ble_stop();
        }

        if (err) {
            LOG_ERR("BLE stop failed: %d", err);
            next_mode_retry_ms = now + MODE_RETRY_MS;
            return;
        }

        mode_phase = MODE_WAIT_DISCONNECT;
    }

    // Wait until the previous connection is gone.
    if (mode_phase == MODE_WAIT_DISCONNECT) {

        bool idle =
            current_mode == WIO_MODE_LAPTOP
                ? myo_ble_client_idle()
                : gemma_ble_idle();

        if (!idle) {
            return;
        }

        mode_phase = MODE_START_NEW;
    }

    // Start the requested Bluetooth role.
    if (mode_phase == MODE_START_NEW) {

        if (requested_mode == WIO_MODE_GEMMA) {
            err = gemma_ble_start();
        } else {
            err = myo_ble_client_resume_computer();
        }

        if (err) {
            LOG_ERR("BLE mode start failed: %d", err);
            next_mode_retry_ms = now + MODE_RETRY_MS;
            return;
        }

        current_mode = requested_mode;
        mode_phase = MODE_READY;

        // OFF = Laptop, ON = Gemma.
        if (rgb_ready) {

            uint8_t brightness =
                current_mode == WIO_MODE_GEMMA
                    ? 100U : 0U;

            int led_err = wio_rgb_set_channel(
                WIO_RGB_PIN_29,
                brightness);

            if (led_err) {
                LOG_ERR("Mode LED failed: %d", led_err);
            }
        }

        LOG_INF("MODE ACTIVE: %s",
            current_mode == WIO_MODE_GEMMA
                ? "GEMMA" : "LAPTOP");
    }
}

int main(void)
{
    LOG_INF("GestureControl Wio controller starting");

    if (!device_is_ready(trackball)) {
        LOG_ERR("Trackball not ready");
        return 0;
    }

    // Configure RGB outputs.
    int rgb_err = wio_rgb_init();

    if (rgb_err) {
        LOG_ERR("RGB init failed: %d", rgb_err);
    } else {
        rgb_ready = true;

        // Laptop is the default mode.
        wio_rgb_set_channel(WIO_RGB_PIN_29, 0);
    }

    // Start in Laptop mode, scanning for MyoPC.
    int err = myo_ble_client_init_mode(
        MYO_MODE_COMPUTER);

    if (err) {
        LOG_ERR("BLE initialization failed: %d", err);
        return 0;
    }

    LOG_INF("Default mode: LAPTOP");
    LOG_INF("Hold center button for 3 seconds to switch");

    while (1) {

        struct key_event event;

        // Check for input, but wake periodically
        // to process a held button and BLE transitions.
        if (k_msgq_get(
                &key_queue,
                &event,
                K_MSEC(25)) == 0) {

            handle_key_event(&event);
        }

        poll_button();
        poll_mode_switch();
    }

    return 0;
}