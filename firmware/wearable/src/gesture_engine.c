#include "gesture_engine.h"

#include <stddef.h>
#include <string.h>

#define REST_CONFIRM_MS       350U
#define FLEX_HOLD_MS         1100U
#define MODE_HOLD_MS         5000U
#define FLEX_RELEASE_GRACE_MS 200U
#define MOTION_BLOCK_MS       650U
#define SWIPE_GRACE_MS        450U
#define ACTION_COOLDOWN_MS    400U

static bool before_deadline(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) < 0;
}

static enum myo_gesture swipe_to_gesture(enum swipe_event swipe)
{
    switch (swipe) {
    case SWIPE_UP:
        return MYO_GESTURE_SWIPE_UP;
    case SWIPE_DOWN:
        return MYO_GESTURE_SWIPE_DOWN;
    case SWIPE_LEFT:
        return MYO_GESTURE_SWIPE_LEFT;
    case SWIPE_RIGHT:
        return MYO_GESTURE_SWIPE_RIGHT;
    default:
        return MYO_GESTURE_NONE;
    }
}

void gesture_engine_init(struct gesture_engine *engine)
{
    if (engine == NULL) {
        return;
    }

    memset(engine, 0, sizeof(*engine));

    // Start disarmed. A stable REST is required
    // before SELECT can be emitted.
    engine->select_armed = false;
}

enum myo_gesture gesture_engine_update(
    struct gesture_engine *engine,
    uint32_t now_ms,
    bool orientation_valid,
    bool emg_ready,
    emg_class_t emg_class,
    bool significant_motion,
    enum swipe_event swipe)
{
    if (engine == NULL) {
        return MYO_GESTURE_NONE;
    }

    // Remember the last good control orientation.
    if (orientation_valid) {
        engine->last_valid_ms = now_ms;
        engine->have_valid_orientation = true;
    }

    bool recently_valid =
        engine->have_valid_orientation &&
        (uint32_t)(now_ms - engine->last_valid_ms)
            <= SWIPE_GRACE_MS;

    // Any significant angular motion interrupts
    // an ongoing FLEX candidate.
    if (significant_motion) {
        engine->motion_block_until_ms =
            now_ms + MOTION_BLOCK_MS;

        engine->flex_tracking = false;
    }

    // Swipes always have priority over muscle
    // classification. The IMU detector itself
    // handles return-to-neutral and re-arming.
    if (swipe != SWIPE_NONE) {

        engine->motion_block_until_ms =
            now_ms + MOTION_BLOCK_MS;

        engine->flex_tracking = false;
        engine->rest_tracking = false;

        // Force a new REST before another SELECT.
        engine->select_armed = false;

        if (!recently_valid ||
            before_deadline(
                now_ms, engine->cooldown_until_ms)) {
            return MYO_GESTURE_NONE;
        }

        engine->cooldown_until_ms =
            now_ms + ACTION_COOLDOWN_MS;

        return swipe_to_gesture(swipe);
    }

    // New EMG commands require an actively valid
    // orientation and a valid ML prediction.
    if (!orientation_valid || !emg_ready) {

        engine->select_armed = false;
        engine->rest_tracking = false;
        engine->flex_tracking = false;

        return MYO_GESTURE_NONE;
    }

    // Wait for wrist movement to settle.
    if (before_deadline(
            now_ms, engine->motion_block_until_ms)) {

        engine->rest_tracking = false;
        engine->flex_tracking = false;

        return MYO_GESTURE_NONE;
    }


    // Short FLEX performs SELECT on release; five seconds
    // changes receiver without sending a SELECT first.
    if (emg_class == EMG_FLEX) {
        engine->rest_tracking = false;

        if (!engine->select_armed) {
            engine->flex_tracking = false;
            return MYO_GESTURE_NONE;
        }

        // A long prediction gap breaks the hold.
        if (engine->flex_tracking &&
            (uint32_t)(now_ms - engine->flex_last_seen_ms)
                >= FLEX_RELEASE_GRACE_MS) {
            engine->flex_tracking = false;
        }

        if (!engine->flex_tracking) {
            engine->flex_tracking = true;
            engine->flex_since_ms = now_ms;
            engine->flex_last_seen_ms = now_ms;
            return MYO_GESTURE_NONE;
        }

        engine->flex_last_seen_ms = now_ms;

        if ((uint32_t)(now_ms - engine->flex_since_ms)
            >= MODE_HOLD_MS) {
            engine->mode_switch_pending = true;
            engine->flex_tracking = false;
            engine->select_armed = false;
            engine->rest_tracking = false;
            engine->cooldown_until_ms =
                now_ms + ACTION_COOLDOWN_MS;
        }

        return MYO_GESTURE_NONE;
    }

    // A brief ML flicker should not end an ongoing FLEX.
    if (engine->flex_tracking) {
        if ((uint32_t)(now_ms - engine->flex_last_seen_ms)
            < FLEX_RELEASE_GRACE_MS) {
            return MYO_GESTURE_NONE;
        }

        uint32_t held_ms =
            engine->flex_last_seen_ms - engine->flex_since_ms;
        engine->flex_tracking = false;

        if (held_ms >= FLEX_HOLD_MS &&
            !before_deadline(now_ms, engine->cooldown_until_ms)) {
            engine->select_armed = false;
            engine->cooldown_until_ms =
                now_ms + ACTION_COOLDOWN_MS;
            return MYO_GESTURE_TENSE;
        }
    }

    // Stable REST re-arms SELECT and MODE SWITCH.
    if (emg_class == EMG_REST) {
        if (!engine->rest_tracking) {
            engine->rest_tracking = true;
            engine->rest_since_ms = now_ms;
        }

        if ((uint32_t)(now_ms - engine->rest_since_ms)
            >= REST_CONFIRM_MS) {
            engine->select_armed = true;
        }
    } else {
        engine->rest_tracking = false;
    }

    return MYO_GESTURE_NONE;
}

bool gesture_engine_take_mode_switch(
    struct gesture_engine *engine)
{
    if (engine == NULL || !engine->mode_switch_pending) {
        return false;
    }

    engine->mode_switch_pending = false;
    return true;
}
