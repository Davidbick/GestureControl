#ifndef GESTURE_ENGINE_H
#define GESTURE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "emg_model.h"
#include "swipe.h"
#include "myo_ble.h"

// Gesture interpretation runs on the wearable main thread.
// Swipe detection and EMG inference run in sensor_stream.c.
struct gesture_engine {
    bool emg_armed;
    bool rest_tracking;
    bool tense_tracking;
    bool tense_started_in_gate;
    bool tense_mode_allowed;
    bool mode_switch_pending;

    uint32_t rest_since_ms;
    uint32_t tense_since_ms;
    uint32_t tense_last_seen_ms;
    uint32_t motion_block_until_ms;
    uint32_t cooldown_until_ms;
};

void gesture_engine_init(struct gesture_engine *engine);

// Abort unfinished EMG holds if sensor data is missing/stale.
void gesture_engine_abort(struct gesture_engine *engine);

enum myo_gesture gesture_engine_update(
    struct gesture_engine *engine,
    uint32_t now_ms,
    bool orientation_valid,
    bool emg_ready,
    emg_class_t emg_class,
    bool significant_motion,
    enum swipe_event swipe
);

// A five-second TENSE outside the orientation gate requests
// a local receiver change; no TENSE action is sent over BLE.
bool gesture_engine_take_mode_switch(
    struct gesture_engine *engine);

#endif
