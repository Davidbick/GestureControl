
#include "sensor_stream.h"
#include "emg_inference.h"
#include "swipe.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(sensor_stream, LOG_LEVEL_INF);

#define SAMPLE_QUEUE_DEPTH 128

// Queue separates acquisition from slow serial output.
K_MSGQ_DEFINE(
    sample_queue,
    sizeof(struct sensor_frame),
    SAMPLE_QUEUE_DEPTH,
    4
);

K_MUTEX_DEFINE(latest_lock);

K_SEM_DEFINE(acquisition_start, 0, 1);
K_SEM_DEFINE(output_start, 0, 1);

static struct sensor_frame latest_frame;
static bool latest_valid;

static atomic_t dropped_samples;
static atomic_t read_errors;

static uint32_t next_sequence;
static struct swipe_detector swipe_detector;

static const struct device *const console_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static bool recording_connected(void)
{
    if (!device_is_ready(console_dev)) {
        return false;
    }

    uint32_t dtr = 0;

    int ret = uart_line_ctrl_get(
        console_dev,
        UART_LINE_CTRL_DTR,
        &dtr);

    return ret == 0 && dtr != 0;
}

static void acquisition_thread(
    void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_take(&acquisition_start, K_FOREVER);

    int64_t next_time = k_uptime_get();

    LOG_INF("Sensor acquisition started at 100 Hz");

    while (1) {
        struct sensor_frame frame = {0};

        frame.sequence = next_sequence++;
        frame.timestamp_ms = k_uptime_get_32();

        int emg_ret = emg_read(&frame.emg);
        int imu_ret = imu_read_motion(&frame.motion);

        if (emg_ret == 0 && imu_ret == 0) {
            enum swipe_event event = swipe_update(
                &swipe_detector,
                &frame.motion,
                frame.timestamp_ms
            );

            if (event != SWIPE_NONE) {
                const char *name = "UNKNOWN";

                switch (event) {
                case SWIPE_UP:
                    name = "UP";
                    break;
                case SWIPE_DOWN:
                    name = "DOWN";
                    break;
                case SWIPE_LEFT:
                    name = "LEFT";
                    break;
                case SWIPE_RIGHT:
                    name = "RIGHT";
                    break;
                default:
                    break;
                }

                LOG_INF("SWIPE DETECTED: %s", name);
            }
            // Feed every valid EMG0 sample to the classifier.
            // Runs at the acquisition rate of 100 Hz.
            emg_inference_push(
                frame.emg.mv[0],
                frame.sequence,
                frame.timestamp_ms
            );

            k_mutex_lock(&latest_lock, K_FOREVER);

            latest_frame = frame;
            latest_valid = true;

            k_mutex_unlock(&latest_lock);

            frame.errors_total =
                (uint32_t)atomic_get(&read_errors);

            frame.dropped_total =
                (uint32_t)atomic_get(&dropped_samples);

            if (k_msgq_put(
                    &sample_queue,
                    &frame,
                    K_NO_WAIT) != 0) {

                atomic_inc(&dropped_samples);
            }

        } else {
            atomic_inc(&read_errors);
        }

        // Maintain absolute sample deadlines.
        next_time += SENSOR_SAMPLE_INTERVAL_MS;

        int64_t now = k_uptime_get();
        int64_t wait_ms = next_time - now;

        if (wait_ms > 0) {
            k_msleep(wait_ms);
        } else {
            // Skip missed deadlines rather than
            // accumulating timing drift.
            do {
                next_time += SENSOR_SAMPLE_INTERVAL_MS;
            } while (next_time <= now);
        }
    }
}

static void output_thread(
    void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    k_sem_take(&output_start, K_FOREVER);

    while (1) {
        struct sensor_frame frame;

        if (k_msgq_get(
                &sample_queue,
                &frame,
                K_FOREVER) != 0) {
            continue;
        }

        // Discard output when no host is recording.
        // Acquisition continues independently.
        if (!recording_connected()) {
            continue;
        }

        printk(
            "MC1,%u,%u,%d,%d,%d,%d,"
            "%d,%d,%d,%d,%d,%d,%u,%u\n",
            frame.sequence,
            frame.timestamp_ms,
            frame.emg.raw[0],
            frame.emg.raw[1],
            frame.emg.mv[0],
            frame.emg.mv[1],
            frame.motion.accel.x_mg,
            frame.motion.accel.y_mg,
            frame.motion.accel.z_mg,
            frame.motion.gyro.x_mdps,
            frame.motion.gyro.y_mdps,
            frame.motion.gyro.z_mdps,
            frame.dropped_total,
            frame.errors_total);
    }
}

K_THREAD_DEFINE(
    acquisition_thread_id,
    2048,
    acquisition_thread,
    NULL, NULL, NULL,
    5, 0, 0
);

K_THREAD_DEFINE(
    output_thread_id,
    2048,
    output_thread,
    NULL, NULL, NULL,
    8, 0, 0
);

void sensor_stream_start(void)
{
    swipe_init(&swipe_detector);

    atomic_set(&dropped_samples, 0);
    atomic_set(&read_errors, 0);

    k_sem_give(&acquisition_start);
    k_sem_give(&output_start);
}

int sensor_stream_latest(struct sensor_frame *sample)
{
    if (sample == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&latest_lock, K_FOREVER);

    if (!latest_valid) {
        k_mutex_unlock(&latest_lock);
        return -EAGAIN;
    }

    *sample = latest_frame;

    k_mutex_unlock(&latest_lock);

    return 0;
}
