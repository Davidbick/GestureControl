#ifndef ORIENTATION_H
#define ORIENTATION_H

#include <stdbool.h>
#include "imu.h"

struct orientation_gate {
    float ref_x;
    float ref_y;
    float ref_z;

    bool calibrated;
    bool valid;

    int64_t candidate_since_ms;
};

void orientation_set_reference(
    struct orientation_gate *gate,
    const struct imu_accel_sample *reference);

bool orientation_update(
    struct orientation_gate *gate,
    const struct imu_accel_sample *sample);

int orientation_angle_deg(
    const struct orientation_gate *gate,
    const struct imu_accel_sample *sample);

#endif
