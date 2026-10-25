#ifndef ORIENTATION_H
#define ORIENTATION_H

#include <stdbool.h>
#include <stdint.h>
#include "imu.h"

struct orientation_gate {
    float ref_x;
    float ref_y;
    float ref_z;

    bool calibrated;
    bool valid;
    bool candidate_active;
    uint32_t candidate_since_ms;
};

void orientation_set_reference(
    struct orientation_gate *gate,
    const struct imu_accel_sample *reference);

void orientation_invalidate(struct orientation_gate *gate);

bool orientation_update(
    struct orientation_gate *gate,
    const struct imu_accel_sample *sample,
    uint32_t timestamp_ms);

int orientation_angle_deg(
    const struct orientation_gate *gate,
    const struct imu_accel_sample *sample);

#endif
