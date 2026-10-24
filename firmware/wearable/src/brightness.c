#include "brightness.h"

#define FILTER_MS 500.0f
#define CONFIRM_MS 250U
#define HYSTERESIS 0.075f

static float clamp(float value, float lo, float hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

void brightness_init(struct brightness_estimator *b,
                     float baseline_mv,
                     float maximum_mv)
{
    if (!b) return;

    if (maximum_mv <= baseline_mv + 1.0f) {
        maximum_mv = baseline_mv + 1.0f;
    }

    *b = (struct brightness_estimator){0};

    b->baseline_mv = baseline_mv;
    b->maximum_mv = maximum_mv;
    b->filtered_mv = baseline_mv;
    b->initialized = true;
}

uint8_t brightness_get(const struct brightness_estimator *b)
{
    return b ? b->level * 25U : 0U;
}

uint8_t brightness_update(struct brightness_estimator *b,
                          int32_t emg0_mv,
                          uint32_t timestamp_ms,
                          bool orientation_valid)
{
    if (!b || !b->initialized) return 0;

    // Invalid orientation freezes brightness.
    if (!orientation_valid) {
        b->last_ms = timestamp_ms;
        b->candidate_since_ms = 0;
        b->candidate = b->level;
        return brightness_get(b);
    }

    uint32_t dt = timestamp_ms - b->last_ms;

    if (b->last_ms == 0 || dt > 1000U) {
        dt = 10U;
    }

    b->last_ms = timestamp_ms;

    // Time-based low-pass filter, compatible with
    // either 20 Hz testing or 100 Hz integration.
    float alpha = (float)dt / (FILTER_MS + (float)dt);

    b->filtered_mv += alpha *
        ((float)emg0_mv - b->filtered_mv);

    float intensity =
        (b->filtered_mv - b->baseline_mv) /
        (b->maximum_mv - b->baseline_mv);

    intensity = clamp(intensity, 0.0f, 1.0f);

    uint8_t proposed = b->level;

    // Adjacent-level thresholds with hysteresis.
    // Moving only one step at a time reduces jumps.
    if (b->level < 4) {
        float up_threshold =
            ((float)b->level + 0.5f) / 4.0f;

        if (intensity > up_threshold + HYSTERESIS) {
            proposed = b->level + 1;
        }
    }

    if (b->level > 0) {
        float down_threshold =
            ((float)b->level - 0.5f) / 4.0f;

        if (intensity < down_threshold - HYSTERESIS) {
            proposed = b->level - 1;
        }
    }

    if (proposed == b->level) {
        b->candidate = b->level;
        b->candidate_since_ms = 0;
        return brightness_get(b);
    }

    if (proposed != b->candidate ||
        b->candidate_since_ms == 0) {
        b->candidate = proposed;
        b->candidate_since_ms = timestamp_ms;
        return brightness_get(b);
    }

    if ((uint32_t)(timestamp_ms - b->candidate_since_ms)
        >= CONFIRM_MS) {
        b->level = proposed;
        b->candidate_since_ms = 0;
    }

    return brightness_get(b);
}
