#include "gesture_engine.h"

#include <stddef.h>
#include <string.h>
#include <math.h>

#define REST_CONFIRM_MS       350U
#define FLEX_HOLD_MS         1100U
#define MODE_HOLD_MS         5000U
#define SWIPE_GATE_HOLD_MS   3000U
#define CENTER_STABLE_MS      250U
#define CENTER_COS_THRESHOLD  0.9659258f
#define FLEX_RELEASE_GRACE_MS 200U
#define MOTION_BLOCK_MS       650U
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

// Acceleration gives a gravity-relative hand orientation.
// Yaw around the gravity axis cannot be measured from this vector.
static bool near_swipe_center(
    const struct gesture_engine *engine,
    const struct imu_accel_sample *accel)
{
    if (!engine->swipe_center_valid || accel == NULL) {
        return false;
    }

    float x = (float)accel->x_mg;
    float y = (float)accel->y_mg;
    float z = (float)accel->z_mg;
    float cx = (float)engine->swipe_center.x_mg;
    float cy = (float)engine->swipe_center.y_mg;
    float cz = (float)engine->swipe_center.z_mg;

    float magnitude_squared = x * x + y * y + z * z;
    float center_squared = cx * cx + cy * cy + cz * cz;

    // Ignore samples dominated by acceleration from movement.
    if (magnitude_squared < 800.0f * 800.0f ||
        magnitude_squared > 1200.0f * 1200.0f ||
        center_squared < 1.0f) {
        return false;
    }

    float dot = x * cx + y * cy + z * cz;

    // Within 15 degrees of the orientation saved at activation.
    return dot >= CENTER_COS_THRESHOLD *
        sqrtf(magnitude_squared * center_squared);
}

static void update_swipe_rearm(
    struct gesture_engine *engine,
    uint32_t now_ms,
    const struct imu_accel_sample *accel,
    bool significant_motion)
{
    if (!engine->swipe_enabled || engine->swipe_ready) {
        return;
    }

    if (significant_motion || !near_swipe_center(engine, accel)) {
        engine->center_tracking = false;
        return;
    }

    if (!engine->center_tracking) {
        engine->center_tracking = true;
        engine->center_since_ms = now_ms;
        return;
    }

    if ((uint32_t)(now_ms - engine->center_since_ms) >=
        CENTER_STABLE_MS) {
        engine->swipe_ready = true;
        engine->center_tracking = false;
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
    bool computer_mode,
    bool emg_ready,
    emg_class_t emg_class,
    const struct imu_accel_sample *accel,
    bool significant_motion,
    enum swipe_event swipe)
{
    if (engine == NULL) {
        return MYO_GESTURE_NONE;
    }

    // Any significant angular motion interrupts
    // an ongoing FLEX candidate.
    if (significant_motion) {
        engine->motion_block_until_ms =
            now_ms + MOTION_BLOCK_MS;

        engine->flex_tracking = false;
        engine->select_orientation_ok = false;
    }

    // Only enable laptop arrows after a deliberate 3-second FLEX.
    // After each accepted swipe, returning near the saved center
    // for 250 ms is required before another arrow key can be sent.
    update_swipe_rearm(engine, now_ms, accel, significant_motion);

    if (swipe != SWIPE_NONE) {
        if (!computer_mode ||
            !engine->swipe_enabled ||
            !engine->swipe_ready) {
            return MYO_GESTURE_NONE;
        }

        engine->swipe_ready = false;
        engine->center_tracking = false;
        engine->motion_block_until_ms = now_ms + MOTION_BLOCK_MS;
        engine->flex_tracking = false;
        engine->select_orientation_ok = false;
        engine->rest_tracking = false;
        engine->select_armed = false;

        if (before_deadline(now_ms, engine->cooldown_until_ms)) {
            return MYO_GESTURE_NONE;
        }

        engine->cooldown_until_ms = now_ms + ACTION_COOLDOWN_MS;
        return swipe_to_gesture(swipe);
    }

    // Timed FLEX holds work in any arm orientation.
    // The EMG prediction is still required.
    if (!emg_ready) {
        engine->select_armed = false;
        engine->rest_tracking = false;
        engine->flex_tracking = false;
        engine->select_orientation_ok = false;
        return MYO_GESTURE_NONE;
    }

    // Only SELECT depends on calibrated orientation.
    // A lost orientation invalidates the entire short hold,
    // but does not interrupt a 3- or 5-second FLEX.
    if (!orientation_valid) {
        engine->select_armed = false;
        engine->rest_tracking = false;
        engine->select_orientation_ok = false;
    }

    // Wait for wrist movement to settle.
    if (before_deadline(
            now_ms, engine->motion_block_until_ms)) {

        engine->rest_tracking = false;
        engine->flex_tracking = false;
        engine->select_orientation_ok = false;

        return MYO_GESTURE_NONE;
    }


    // Short FLEX performs SELECT on release; five seconds
    // changes receiver without sending a SELECT first.
    if (emg_class == EMG_FLEX) {
        engine->rest_tracking = false;

        // A mode switch fires once per continuous FLEX.
        // Wait for release before starting another hold.
        if (engine->flex_consumed) {
            engine->flex_last_seen_ms = now_ms;
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
            engine->select_orientation_ok =
                orientation_valid && engine->select_armed;
            return MYO_GESTURE_NONE;
        }

        engine->flex_last_seen_ms = now_ms;

        if ((uint32_t)(now_ms - engine->flex_since_ms)
            >= MODE_HOLD_MS) {
            engine->mode_switch_pending = true;
            engine->flex_consumed = true;
            engine->select_orientation_ok = false;
            // A receiver change always exits laptop swipe control.
            engine->swipe_enabled = false;
            engine->swipe_ready = false;
            engine->swipe_center_valid = false;
            engine->center_tracking = false;
            engine->flex_tracking = false;
            engine->select_armed = false;
            engine->rest_tracking = false;
            engine->cooldown_until_ms =
                now_ms + ACTION_COOLDOWN_MS;
        }

        return MYO_GESTURE_NONE;
    }

    // After a mode switch, require FLEX to be released
    // for at least the normal classifier grace interval.
    if (engine->flex_consumed) {
        if ((uint32_t)(now_ms - engine->flex_last_seen_ms)
            < FLEX_RELEASE_GRACE_MS) {
            return MYO_GESTURE_NONE;
        }
        engine->flex_consumed = false;
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

        bool select_allowed =
            engine->select_orientation_ok && orientation_valid;
        engine->select_orientation_ok = false;

        // Between 3 and 5 seconds: toggle laptop swipe control
        // on release so that a 5-second hold changes mode only.
        if (computer_mode &&
            held_ms >= SWIPE_GATE_HOLD_MS &&
            held_ms < MODE_HOLD_MS) {
            engine->swipe_enabled = !engine->swipe_enabled;
            engine->swipe_ready = false;
            engine->center_tracking = false;
            engine->swipe_center_valid =
                engine->swipe_enabled && accel != NULL;

            if (engine->swipe_center_valid) {
                engine->swipe_center = *accel;
            }

            engine->select_armed = false;
            engine->cooldown_until_ms =
                now_ms + ACTION_COOLDOWN_MS;
            return MYO_GESTURE_NONE;
        }

        if (held_ms >= FLEX_HOLD_MS &&
            held_ms < SWIPE_GATE_HOLD_MS &&
            select_allowed &&
            !before_deadline(now_ms, engine->cooldown_until_ms)) {
            engine->select_armed = false;
            engine->cooldown_until_ms =
                now_ms + ACTION_COOLDOWN_MS;
            return MYO_GESTURE_TENSE;
        }
    }

    // Stable REST re-arms SELECT and MODE SWITCH.
    if (emg_class == EMG_REST && orientation_valid) {
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
