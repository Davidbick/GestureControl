#include "swipe.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static enum swipe_event tick(
    struct swipe_detector *d,
    uint32_t *time_ms,
    bool gate,
    float x, float y, float z)
{
    struct imu_motion_sample motion = {0};
    motion.gyro.x_mdps = (int32_t)x;
    motion.gyro.y_mdps = (int32_t)y;
    motion.gyro.z_mdps = (int32_t)z;

    enum swipe_event result =
        swipe_update(d, &motion, *time_ms, gate);
    *time_ms += 10;
    return result;
}

static void neutral(
    struct swipe_detector *d,
    uint32_t *time_ms,
    bool gate,
    int count)
{
    while (count-- > 0) {
        assert(tick(d, time_ms, gate, 0, 0, 0) == SWIPE_NONE);
    }
}

static enum swipe_event move(
    struct swipe_detector *d,
    uint32_t *time_ms,
    bool gate,
    const float vector[3],
    int count)
{
    enum swipe_event seen = SWIPE_NONE;

    for (int i = 0; i < count; i++) {
        enum swipe_event event =
            tick(d, time_ms, gate,
                 vector[0], vector[1], vector[2]);
        if (event != SWIPE_NONE) {
            // Two events from a single stroke indicate rearm failure.
            assert(seen == SWIPE_NONE);
            seen = event;
        }
    }
    return seen;
}

int main(void)
{
    // Copies of deployed learned signatures, order UP/DOWN/LEFT/RIGHT.
    const float centers[4][3] = {
        { 0.48681577f, 0.20604222f, 0.84885630f},
        {-0.24785536f, 0.58038338f,-0.77570797f},
        { 0.81636970f, 0.26121223f,-0.51508124f},
        {-0.67543457f, 0.05956263f, 0.73501050f}
    };

    struct swipe_detector d;
    uint32_t now = 0;
    swipe_init(&d);

    neutral(&d, &now, false, 60);
    assert(!d.armed);
    neutral(&d, &now, true, 60);
    assert(d.armed);

    for (int direction = 0; direction < 4; direction++) {
        float v[3];
        for (int axis = 0; axis < 3; axis++) {
            v[axis] = centers[direction][axis] * 250000;
        }

        assert(move(&d, &now, true, v, 12) ==
               (enum swipe_event)(direction + 1));
        assert(!d.armed);

        assert(move(&d, &now, false, v, 15) == SWIPE_NONE);
        neutral(&d, &now, false, 60);
        assert(!d.armed);
        neutral(&d, &now, true, 60);
        assert(d.armed);
    }

    // Gate grace allows a swipe to leave the pose before high gyro.
    float up[3];
    for (int i = 0; i < 3; i++) up[i] = centers[0][i] * 250000;
    assert(move(&d, &now, false, up, 12) == SWIPE_UP);
    assert(d.stats.grace_starts > 0);
    neutral(&d, &now, true, 65);
    assert(d.armed);

    // A subthreshold attempt must not become an event.
    float slow[3];
    for (int i = 0; i < 3; i++) slow[i] = centers[1][i] * 45000;
    assert(move(&d, &now, true, slow, 30) == SWIPE_NONE);
    neutral(&d, &now, true, 35);
    assert(d.stats.below_threshold > 0);

    // A movement starting outside gate grace must be rejected.
    neutral(&d, &now, false, 50);
    float left[3];
    for (int i = 0; i < 3; i++) left[i] = centers[2][i] * 250000;
    assert(move(&d, &now, false, left, 12) == SWIPE_NONE);
    assert(d.stats.gate_blocked > 0);
    neutral(&d, &now, true, 60);
    assert(d.armed);

    // Ambiguous direction is rejected rather than guessed.
    float ambiguous[3];
    for (int i = 0; i < 3; i++) {
        ambiguous[i] = (centers[0][i] + centers[3][i]) * 250000;
    }
    assert(move(&d, &now, true, ambiguous, 12) == SWIPE_NONE);
    assert(d.stats.ambiguous_rejected > 0);

    // A missing sensor interval forces another neutral rearm.
    now += 100;
    (void)tick(&d, &now, true, 0, 0, 0);
    assert(!d.armed);
    assert(d.stats.gaps > 0);

    printf("Phase 4 swipe tests passed\n");
    return 0;
}
