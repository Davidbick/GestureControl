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
    // Demo detector: begin ready, no startup gate/neutral prerequisite.
    d->armed = true;
}

void swipe_reset_motion(struct swipe_detector *d)
{
    if (d == NULL) return;
    struct swipe_stats old_stats = d->stats;
    swipe_init(d);
    d->stats = old_stats;
}

static enum swipe_event recognize(
    struct swipe_detector *d, const float mean[3])
{
    float energy = magnitude(mean);
    if (energy < SWIPE_MIN_CAPTURE_MDPS) {
        d->stats.weak_capture++;
        return SWIPE_NONE;
    }

    float best = -2.0f, second = -2.0f;
    int best_index = -1;

    for (int i = 0; i < 4; ++i) {
        float score = (mean[0] * swipe_centers[i][0] +
                       mean[1] * swipe_centers[i][1] +
                       mean[2] * swipe_centers[i][2]) / energy;
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
    (void)orientation_valid; // Diagnostic only for this demo.

    if (d->initialized) {
        uint32_t dt = timestamp_ms - d->last_timestamp_ms;
        if (dt == 0U) return SWIPE_NONE;
        if (dt > SWIPE_SAMPLE_GAP_MS) {
            swipe_reset_motion(d);
            d->stats.gaps++;
        }
    }
    d->initialized = true;
    d->last_timestamp_ms = timestamp_ms;

    const float raw[3] = {
        (float)motion->gyro.x_mdps,
        (float)motion->gyro.y_mdps,
        (float)motion->gyro.z_mdps
    };
    for (int i = 0; i < 3; ++i) {
        d->smooth[i] += (raw[i] - d->smooth[i]) / 3.0f;
    }
    float energy = magnitude(d->smooth);

    // Capture 80 ms of smoothed gyro data after a 60k mdps onset.
    if (d->collecting) {
        for (int i = 0; i < 3; ++i) {
            d->vector_sum[i] += d->smooth[i];
        }
        d->vector_count++;
        if ((uint32_t)(timestamp_ms - d->onset_ms) <
            SWIPE_CAPTURE_MS) {
            return SWIPE_NONE;
        }

        float mean[3];
        for (int i = 0; i < 3; ++i) {
            mean[i] = d->vector_sum[i] / d->vector_count;
        }
        d->collecting = false;
        d->armed = false;
        d->ever_attempted = true;
        d->last_attempt_ms = timestamp_ms;
        d->neutral_pending = false;
        return recognize(d, mean);
    }

    // Prevent return-stroke duplicates: cool down, then rearm after
    // 150 ms of low angular velocity (<20k mdps).
    if (!d->armed) {
        bool cooled_down =
            (uint32_t)(timestamp_ms - d->last_attempt_ms) >=
            SWIPE_LOCKOUT_MS;
        if (cooled_down && energy < SWIPE_NEUTRAL_MDPS) {
            if (!d->neutral_pending) {
                d->neutral_pending = true;
                d->neutral_since_ms = timestamp_ms;
            } else if ((uint32_t)(timestamp_ms -
                                  d->neutral_since_ms) >=
                       SWIPE_NEUTRAL_HOLD_MS) {
                d->armed = true;
                d->neutral_pending = false;
                d->stats.rearms++;
            }
        } else {
            d->neutral_pending = false;
        }
    }

    if (d->armed && energy >= SWIPE_ONSET_MDPS) {
        d->collecting = true;
        d->onset_ms = timestamp_ms;
        d->vector_count = 1;
        d->stats.attempts++;
        for (int i = 0; i < 3; ++i) {
            d->vector_sum[i] = d->smooth[i];
        }
    }
    return SWIPE_NONE;
}
