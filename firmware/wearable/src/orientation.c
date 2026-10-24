#include "orientation.h"
#include <zephyr/kernel.h>

#include <math.h>

#define ENTER_ANGLE_DEG 25
#define EXIT_ANGLE_DEG 35

#define VALID_HOLD_TIME_MS 200

#define MIN_GRAVITY_MG 800.0f
#define MAX_GRAVITY_MG 1200.0f


static float vector_magnitude(float x, float y, float z)
{
    return sqrtf((x * x) + (y * y) + (z * z));
}

static float orientation_alignment(
    const struct orientation_gate *gate,
    const struct imu_accel_sample *sample)
{
    float sx = sample->x_mg;
    float sy = sample->y_mg;
    float sz = sample->z_mg;

    float sample_mag = vector_magnitude(sx, sy, sz);

    float ref_mag = vector_magnitude(
        gate->ref_x,
        gate->ref_y,
        gate->ref_z);

    if (sample_mag == 0.0f || ref_mag == 0.0f) {
        return -1.0f;
    }

    float dot =
        (sx * gate->ref_x) +
        (sy * gate->ref_y) +
        (sz * gate->ref_z);

    return dot / (sample_mag * ref_mag);
}

void orientation_set_reference(
    struct orientation_gate *gate,
    const struct imu_accel_sample *reference)
{
    gate->ref_x = reference->x_mg;
    gate->ref_y = reference->y_mg;
    gate->ref_z = reference->z_mg;

    gate->calibrated = true;
    gate->valid = false;
    gate->candidate_since_ms = -1;
}

bool orientation_update(
    struct orientation_gate *gate,
    const struct imu_accel_sample *sample)
{
    if (!gate->calibrated) {
        return false;
    }

    float magnitude = vector_magnitude(
        sample->x_mg,
        sample->y_mg,
        sample->z_mg);

    if (magnitude < MIN_GRAVITY_MG ||
        magnitude > MAX_GRAVITY_MG) {

        gate->valid = false;
        gate->candidate_since_ms = -1;

        return false;
    }

    int angle = orientation_angle_deg(gate, sample);

    if (gate->valid) {

        if (angle > EXIT_ANGLE_DEG) {
            gate->valid = false;
            gate->candidate_since_ms = -1;
        }

        return gate->valid;
    }

    if (angle <= ENTER_ANGLE_DEG) {

        int64_t now = k_uptime_get();

        if (gate->candidate_since_ms < 0) {
            gate->candidate_since_ms = now;
        }

        if ((now - gate->candidate_since_ms) >=
            VALID_HOLD_TIME_MS) {

            gate->valid = true;
            gate->candidate_since_ms = -1;
        }

    } else {
        gate->candidate_since_ms = -1;
    }

    return gate->valid;
}

int orientation_angle_deg(
    const struct orientation_gate *gate,
    const struct imu_accel_sample *sample)
{
    float alignment =
        orientation_alignment(gate, sample);

    if (alignment > 1.0f) {
        alignment = 1.0f;
    }

    if (alignment < -1.0f) {
        alignment = -1.0f;
    }

    float radians = acosf(alignment);

    return (int)(radians * 57.2958f);
}
