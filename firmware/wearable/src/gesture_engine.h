#ifndef GESTURE_ENGINE_H
#define GESTURE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "emg_model.h"
#include "swipe.h"
#include "myo_ble.h"

// Gesture interpretation runs in the main thread.
// Sensor acquisition remains in sensor_stream.c.

struct gesture_engine {
    bool select_armed;
    bool rest_tracking;
    bool flex_tracking;
    bool have_valid_orientation;
    bool mode_switch_pending;

    uint32_t rest_since_ms;
    uint32_t flex_since_ms;
    uint32_t flex_last_seen_ms;
    uint32_t last_valid_ms;

    uint32_t motion_block_until_ms;
    uint32_t cooldown_until_ms;
};

void gesture_engine_init(struct gesture_engine *engine);

enum myo_gesture gesture_engine_update(
    struct gesture_engine *engine,
    uint32_t now_ms,
    bool orientation_valid,
    bool emg_ready,
    emg_class_t emg_class,
    bool significant_motion,
    enum swipe_event swipe
);

// Consume a local receiver-mode change request.
bool gesture_engine_take_mode_switch(
    struct gesture_engine *engine);

#endif
