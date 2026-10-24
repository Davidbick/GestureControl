#include "swipe.h"
#include "swipe_config.h"

#include <math.h>
#include <string.h>

#define CAPTURE_MS 80u
#define NEUTRAL_MS 300u
#define LOCKOUT_MS 1200u

#define NEUTRAL_THRESHOLD \
    (SWIPE_THRESHOLD_MDPS * 0.45f)

void swipe_init(struct swipe_detector *d)
{
    if (d == NULL) {
        return;
    }

    memset(d, 0, sizeof(*d));
    d->armed = true;
}

static float magnitude(const float v[3])
{
    return sqrtf(
        v[0] * v[0] +
        v[1] * v[1] +
        v[2] * v[2]
    );
}

static enum swipe_event recognize(const float vector[3])
{
    float mag = magnitude(vector);

    if (mag < 1.0f) {
        return SWIPE_NONE;
    }

    float best_score = -2.0f;
    int best_index = -1;

    for (int i = 0; i < 4; i++) {
        float score = (
            vector[0] * swipe_centers[i][0] +
            vector[1] * swipe_centers[i][1] +
            vector[2] * swipe_centers[i][2]
        ) / mag;

        if (score > best_score) {
            best_score = score;
            best_index = i;
        }
    }

    if (best_score < SWIPE_SIMILARITY) {
        return SWIPE_NONE;
    }

    return (enum swipe_event)(best_index + 1);
}

enum swipe_event swipe_update(
    struct swipe_detector *d,
    const struct imu_motion_sample *motion,
    uint32_t timestamp_ms
)
{
    if (d == NULL || motion == NULL) {
        return SWIPE_NONE;
    }

    if (d->initialized) {
        uint32_t dt = timestamp_ms - d->last_timestamp_ms;

        // Never process duplicate or excessively delayed samples.
        if (dt == 0u) {
            return SWIPE_NONE;
        }

        if (dt > 40u) {
            swipe_init(d);
        }
    }

    d->initialized = true;
    d->last_timestamp_ms = timestamp_ms;

    const float raw[3] = {
        (float)motion->gyro.x_mdps,
        (float)motion->gyro.y_mdps,
        (float)motion->gyro.z_mdps
    };

    // Causal EWMA, identical to Python analysis.
    for (int i = 0; i < 3; i++) {
        d->smooth[i] +=
            (raw[i] - d->smooth[i]) / 3.0f;
    }

    float energy = magnitude(d->smooth);

    // 1. Collect the first 80 ms of a swipe.
    if (d->collecting) {
        for (int i = 0; i < 3; i++) {
            d->vector_sum[i] += d->smooth[i];
        }

        d->vector_count++;

        if ((uint32_t)(timestamp_ms - d->onset_ms)
            < CAPTURE_MS) {
            return SWIPE_NONE;
        }

        float mean[3];

        for (int i = 0; i < 3; i++) {
            mean[i] =
                d->vector_sum[i] / d->vector_count;
        }

        d->collecting = false;
        d->armed = false;
        d->emitted_ms = timestamp_ms;
        d->neutral_pending = false;

        // Only one event per detected movement.
        return recognize(mean);
    }

    // 2. Detect the start of a new swipe.
    if (d->armed) {
        if (energy >= SWIPE_THRESHOLD_MDPS) {
            d->collecting = true;
            d->onset_ms = timestamp_ms;
            d->vector_count = 1;

            for (int i = 0; i < 3; i++) {
                d->vector_sum[i] = d->smooth[i];
            }
        }

        return SWIPE_NONE;
    }

    // 3. Suppress return motion and wait for neutral.
    if (energy < NEUTRAL_THRESHOLD) {
        if (!d->neutral_pending) {
            d->neutral_since_ms = timestamp_ms;
            d->neutral_pending = true;
        } else if (
            (uint32_t)(timestamp_ms - d->neutral_since_ms)
                >= NEUTRAL_MS &&
            (uint32_t)(timestamp_ms - d->emitted_ms)
                >= LOCKOUT_MS
        ) {
            d->armed = true;
            d->neutral_pending = false;
        }
    } else {
        d->neutral_pending = false;
    }

    return SWIPE_NONE;
}
