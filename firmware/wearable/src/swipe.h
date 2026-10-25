#ifndef SWIPE_H
#define SWIPE_H

#include <stdbool.h>
#include <stdint.h>
#include "imu.h"

enum swipe_event {
    SWIPE_NONE = 0,
    SWIPE_UP,
    SWIPE_DOWN,
    SWIPE_LEFT,
    SWIPE_RIGHT
};

struct swipe_stats {
    uint32_t attempts;
    uint32_t accepted;
    uint32_t grace_starts;
    uint32_t gate_blocked;
    uint32_t below_threshold;
    uint32_t weak_capture;
    uint32_t direction_rejected;
    uint32_t ambiguous_rejected;
    uint32_t rearms;
    uint32_t gaps;
};

struct swipe_detector {
    float smooth[3];
    float vector_sum[3];

    uint32_t last_timestamp_ms;
    uint32_t last_gate_ms;
    uint32_t onset_ms;
    uint32_t last_attempt_ms;
    uint32_t neutral_since_ms;

    uint16_t vector_count;

    bool initialized;
    bool gate_seen;
    bool armed;
    bool collecting;
    bool neutral_pending;
    bool ever_attempted;
    bool weak_candidate;
    bool prev_above_onset;

    struct swipe_stats stats;
};

void swipe_init(struct swipe_detector *detector);

// Reset a partial swipe after a sensor gap, preserving stats.
void swipe_reset_motion(struct swipe_detector *detector);

enum swipe_event swipe_update(
    struct swipe_detector *detector,
    const struct imu_motion_sample *motion,
    uint32_t timestamp_ms,
    bool orientation_valid
);

#endif
