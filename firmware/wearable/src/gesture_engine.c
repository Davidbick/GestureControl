#include "gesture_engine.h"

#include <stddef.h>
#include <string.h>

#define REST_CONFIRM_MS          350U
#define TENSE_SHORT_MIN_MS       350U
#define TENSE_MODE_HOLD_MS      5000U
#define TENSE_RELEASE_GRACE_MS   250U
#define TENSE_MISSING_ABORT_MS   600U
#define MOTION_SETTLE_MS         200U
#define ACTION_COOLDOWN_MS       350U

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
    // An intentional 350 ms REST is required before an EMG event.
    engine->emg_armed = false;
}

void gesture_engine_abort(struct gesture_engine *engine)
{
    if (engine == NULL) {
        return;
    }

    // Never complete a partially observed hold after sensor loss.
    engine->emg_armed = false;
    engine->rest_tracking = false;
    engine->tense_tracking = false;
    engine->tense_started_in_gate = false;
    engine->tense_mode_allowed = false;
    engine->mode_switch_pending = false;
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

    // Motion temporarily prevents NEW EMG holds, not IMU swipes.
    if (significant_motion) {
        engine->motion_block_until_ms = now_ms + MOTION_SETTLE_MS;
    }

    // Phase 1 swipe acquisition already validates onset in the
    // shared orientation gate. Swipes work in either receiver mode.
    // Phase 4 will refine onset/grace, direction and rearming.
    enum myo_gesture swipe_action = swipe_to_gesture(swipe);

    if (swipe_action != MYO_GESTURE_NONE &&
        !engine->tense_tracking &&
        !before_deadline(now_ms, engine->cooldown_until_ms)) {
        engine->emg_armed = false;
        engine->rest_tracking = false;
        engine->cooldown_until_ms = now_ms + ACTION_COOLDOWN_MS;
        return swipe_action;
    }

    // Unknown / unavailable predictions are not interpreted as
    // releases. Allow short inference flicker during a TENSE.
    if (!emg_ready || emg_class == EMG_UNKNOWN) {
        if (engine->tense_tracking &&
            (uint32_t)(now_ms - engine->tense_last_seen_ms)
                < TENSE_MISSING_ABORT_MS) {
            return MYO_GESTURE_NONE;
        }

        gesture_engine_abort(engine);
        return MYO_GESTURE_NONE;
    }

    // A TENSE is latched to its starting orientation and
    // cannot switch between action and mode-change semantics.
    if (engine->tense_tracking) {
        if (emg_class == EMG_TENSE) {
            engine->tense_last_seen_ms = now_ms;

            // A mode-switch TENSE must remain OUTSIDE the gate.
            if (orientation_valid) {
                engine->tense_mode_allowed = false;
            }

            if (engine->tense_mode_allowed &&
                (uint32_t)(now_ms - engine->tense_since_ms)
                    >= TENSE_MODE_HOLD_MS) {
                engine->mode_switch_pending = true;
                engine->tense_tracking = false;
                engine->cooldown_until_ms = now_ms + ACTION_COOLDOWN_MS;
            }
            return MYO_GESTURE_NONE;
        }

        if ((uint32_t)(now_ms - engine->tense_last_seen_ms)
            < TENSE_RELEASE_GRACE_MS) {
            return MYO_GESTURE_NONE;
        }

        uint32_t held_ms = engine->tense_last_seen_ms -
                           engine->tense_since_ms;

        // Short TENSE: release to REST after beginning inside gate.
        // Once started, it may finish outside the orientation gate.
        bool short_tense =
            engine->tense_started_in_gate &&
            emg_class == EMG_REST &&
            held_ms >= TENSE_SHORT_MIN_MS &&
            !before_deadline(now_ms, engine->cooldown_until_ms);

        engine->tense_tracking = false;
        engine->tense_mode_allowed = false;
        engine->tense_started_in_gate = false;

        if (short_tense) {
            engine->cooldown_until_ms = now_ms + ACTION_COOLDOWN_MS;
            return MYO_GESTURE_TENSE;
        }

        // Interrupted TENSE cannot immediately produce FLEX/EXTEND.
        return MYO_GESTURE_NONE;
    }

    // Stable REST is the single EMG rearm action in both modes.
    if (emg_class == EMG_REST) {
        if (!engine->rest_tracking) {
            engine->rest_tracking = true;
            engine->rest_since_ms = now_ms;
        } else if ((uint32_t)(now_ms - engine->rest_since_ms)
                   >= REST_CONFIRM_MS) {
            engine->emg_armed = true;
        }
        return MYO_GESTURE_NONE;
    }

    engine->rest_tracking = false;

    if (!engine->emg_armed ||
        before_deadline(now_ms, engine->cooldown_until_ms) ||
        before_deadline(now_ms, engine->motion_block_until_ms)) {
        return MYO_GESTURE_NONE;
    }

    if (emg_class == EMG_TENSE) {
        engine->emg_armed = false;
        engine->tense_tracking = true;
        engine->tense_since_ms = now_ms;
        engine->tense_last_seen_ms = now_ms;

        // Choose the behavior once, based on the starting gate.
        engine->tense_started_in_gate = orientation_valid;
        engine->tense_mode_allowed = !orientation_valid;
        return MYO_GESTURE_NONE;
    }

    if (emg_class == EMG_FLEX ||
        emg_class == EMG_EXTEND) {
        // Consume even invalid-gate activations; no deferred event
        // is allowed when the wearer moves back inside the gate.
        engine->emg_armed = false;

        if (!orientation_valid) {
            return MYO_GESTURE_NONE;
        }

        engine->cooldown_until_ms = now_ms + ACTION_COOLDOWN_MS;

        return emg_class == EMG_FLEX
            ? MYO_GESTURE_FLEX
            : MYO_GESTURE_EXTEND;
    }

    return MYO_GESTURE_NONE;
}

bool gesture_engine_take_mode_switch(struct gesture_engine *engine)
{
    if (engine == NULL || !engine->mode_switch_pending) {
        return false;
    }

    engine->mode_switch_pending = false;
    return true;
}
