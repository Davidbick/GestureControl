#ifndef SENSOR_STREAM_H
#define SENSOR_STREAM_H

#include <stdint.h>
#include "emg.h"
#include "imu.h"
#include <stdbool.h>
#include "swipe.h"

#define SENSOR_SAMPLE_INTERVAL_MS 10

struct sensor_frame {
    uint32_t sequence;
    uint32_t timestamp_ms;

    struct emg_sample emg;
    struct imu_motion_sample motion;

    uint32_t dropped_total;
    uint32_t errors_total;
};

// Start acquisition and USB output workers.
void sensor_stream_start(void);

// Copy the latest valid sample for other firmware.
int sensor_stream_latest(struct sensor_frame *sample);

#endif

// One recognized IMU event, including its detection time.
struct swipe_notification {
    enum swipe_event event;
    uint32_t timestamp_ms;
};

// Non-blocking retrieval of one detected swipe.
bool sensor_stream_pop_swipe(
    struct swipe_notification *notification);

// Discard old events, such as swipes detected
// during startup calibration.
void sensor_stream_clear_swipes(void);
