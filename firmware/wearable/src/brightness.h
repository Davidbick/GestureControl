#ifndef BRIGHTNESS_H
#define BRIGHTNESS_H

#include <stdbool.h>
#include <stdint.h>

struct brightness_estimator {
    float baseline_mv;
    float maximum_mv;
    float filtered_mv;
    uint32_t last_ms;
    uint32_t candidate_since_ms;
    uint8_t level;
    uint8_t candidate;
    bool initialized;
};

void brightness_init(struct brightness_estimator *b,
                     float baseline_mv,
                     float maximum_mv);

uint8_t brightness_update(struct brightness_estimator *b,
                          int32_t emg0_mv,
                          uint32_t timestamp_ms,
                          bool orientation_valid);

uint8_t brightness_get(const struct brightness_estimator *b);

#endif
