#include "swipe.h"
#include "swipe_config.h"
#include "swipe_tuning.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static float magnitude(const float value[3])
{
    return sqrtf(value[0] * value[0] +
                 value[1] * value[1] +
                 value[2] * value[2]);
}

void swipe_init(struct swipe_detector *d)
{
    if (d == NULL) return;
    memset(d, 0, sizeof(*d));

    // Require a stable in-gate neutral before first gesture.
    d->armed = false;
}

void swipe_reset_motion(struct swipe_detector *d)
{
    if (d == NULL) return;

    // Preserve counters across discontinuities, not partial motion.
    struct swipe_stats old_stats = d->stats;
    memset(d, 0, sizeof(*d));
    d->stats = old_stats;
}

static enum swipe_event recognize(
    struct swipe_detector *d,
    const float mean[3])
{
    float energy = magnitude(mean);
    if (energy < SWIPE_MIN_CAPTURE_MDPS) {
        d->stats.weak_capture++;
        return SWIPE_NONE;
    }

    float best = -2.0f;
    float second = -2.0f;
    int best_index = -1;

    // Preserve learned UP/DOWN/LEFT/RIGHT gyro signatures.
    for (int i = 0; i < 4; i++) {
        float dot = mean[0] * swipe_centers[i][0] +
                    mean[1] * swipe_centers[i][1] +
                    mean[2] * swipe_centers[i][2];
        float score = dot / energy;

        if (score > best) {
            second = best;
            best = score;
            best_index = i;
        } else if (score > second) {
            second = score;
        }
    }

    if (best_index < 0 || best < SWIPE_MIN_SIMILARITY) {
        d->stats.direction_rejected++;
        return SWIPE_NONE;
    }
    if (best - second < SWIPE_MIN_SCORE_MARGIN) {
        d->stats.ambiguous_rejected++;
        return SWIPE_NONE;
    }

    d->stats.accepted++;
    return (enum swipe_event)(best_index + 1);
}

enum swipe_event swipe_update(
    struct swipe_detector *d,
    const struct imu_motion_sample *motion,
    uint32_t timestamp_ms,
    bool orientation_valid)
{
    if (d == NULL || motion == NULL) return SWIPE_NONE;

    // 1. Sensor timing continuity.
    if (d->initialized) {
        uint32_t delta = timestamp_ms - d->last_timestamp_ms;
        if (delta == 0U) return SWIPE_NONE;
        if (delta > SWIPE_SAMPLE_GAP_MS) {
            swipe_reset_motion(d);
            d->stats.gaps++;
        }
    }
    d->initialized = true;
    d->last_timestamp_ms = timestamp_ms;

    // 2. Remember recent time inside the calibrated orientation.
    if (orientation_valid) {
        d->last_gate_ms = timestamp_ms;
        d->gate_seen = true;
    }

    // 3. Same EWMA used to learn swipe_config.h templates.
    const float raw[3] = {
        (float)motion->gyro.x_mdps,
        (float)motion->gyro.y_mdps,
        (float)motion->gyro.z_mdps
    };
    for (int axis = 0; axis < 3; axis++) {
        d->smooth[axis] += (raw[axis] - d->smooth[axis]) / 3.0f;
    }
    float energy = magnitude(d->smooth);

    // 4. Once onset is accepted, complete capture outside the gate.
    if (d->collecting) {
        for (int axis = 0; axis < 3; axis++) {
            d->vector_sum[axis] += d->smooth[axis];
        }
        d->vector_count++;

        if ((uint32_t)(timestamp_ms - d->onset_ms) <
            SWIPE_CAPTURE_MS) {
            return SWIPE_NONE;
        }

        float mean[3];
        for (int axis = 0; axis < 3; axis++) {
            mean[axis] = d->vector_sum[axis] / d->vector_count;
        }

        d->collecting = false;
        d->armed = false;
        d->ever_attempted = true;
        d->last_attempt_ms = timestamp_ms;
        d->neutral_pending = false;

        // Accepted and rejected attempts both require rearming.
        return recognize(d, mean);
    }

    // 5. Re-arm only after returning to the gate and staying still.
    if (!d->armed) {
        bool cooled_down = !d->ever_attempted ||
            (uint32_t)(timestamp_ms - d->last_attempt_ms) >=
                SWIPE_LOCKOUT_MS;

        if (orientation_valid && energy < SWIPE_NEUTRAL_MDPS) {
            if (!d->neutral_pending) {
                d->neutral_pending = true;
                d->neutral_since_ms = timestamp_ms;
            } else if (cooled_down &&
                       (uint32_t)(timestamp_ms -
                           d->neutral_since_ms) >=
                       SWIPE_NEUTRAL_HOLD_MS) {
                d->armed = true;
                d->neutral_pending = false;
                d->weak_candidate = false;
                d->stats.rearms++;
            }
        } else {
            d->neutral_pending = false;
        }

        d->prev_above_onset = energy >= SWIPE_ONSET_MDPS;
        return SWIPE_NONE;
    }

    // 6. A legitimate swipe starts within the gate or shortly after
    // leaving it, to avoid missing fast onset transitions.
    bool gate_fresh = orientation_valid ||
        (d->gate_seen &&
         (uint32_t)(timestamp_ms - d->last_gate_ms) <=
             SWIPE_GATE_GRACE_MS);
    bool above_onset = energy >= SWIPE_ONSET_MDPS;

    // 7. Diagnostics for movements too slow to cross onset.
    if (gate_fresh && energy >= SWIPE_PREMOTION_MDPS) {
        d->weak_candidate = true;
    }
    if (!above_onset && energy < SWIPE_NEUTRAL_MDPS &&
        d->weak_candidate) {
        d->stats.below_threshold++;
        d->weak_candidate = false;
    }

    // 8. Start capture, or disarm after any fully out-of-gate
    // movement so its return motion cannot create a false swipe.
    if (above_onset && gate_fresh) {
        d->collecting = true;
        d->onset_ms = timestamp_ms;
        d->vector_count = 1;
        d->weak_candidate = false;
        d->stats.attempts++;

        if (!orientation_valid) {
            d->stats.grace_starts++;
        }
        for (int axis = 0; axis < 3; axis++) {
            d->vector_sum[axis] = d->smooth[axis];
        }
    } else if (above_onset && !gate_fresh) {
        if (!d->prev_above_onset) {
            d->stats.gate_blocked++;
        }
        d->armed = false;
        d->neutral_pending = false;
        d->weak_candidate = false;
    }
    d->prev_above_onset = above_onset;
    return SWIPE_NONE;
}
