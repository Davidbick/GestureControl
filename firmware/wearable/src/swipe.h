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

struct swipe_detector {
    float smooth[3];
    float vector_sum[3];

    uint32_t last_timestamp_ms;
    uint32_t onset_ms;
    uint32_t emitted_ms;
    uint32_t neutral_since_ms;

    uint16_t vector_count;

    bool initialized;
    bool armed;
    bool collecting;
    bool neutral_pending;
};

void swipe_init(struct swipe_detector *detector);

enum swipe_event swipe_update(
    struct swipe_detector *detector,
    const struct imu_motion_sample *motion,
    uint32_t timestamp_ms
);

#endif
