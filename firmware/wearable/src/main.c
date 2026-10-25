#include <errno.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "emg.h"
#include "imu.h"
#include "orientation.h"
#include "ble.h"
#include "myo_ble.h"
#include "sensor_stream.h"
#include "emg_inference.h"
#include "gesture_engine.h"
#include "myo_ble_client.h"

LOG_MODULE_REGISTER(wearable, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
#define CALIBRATION_SAMPLES 100
#define CALIBRATION_INTERVAL_MS 20
#define CALIBRATION_MAX_DRIFT_COS 0.9781476f
#define SENSOR_WARNING_INTERVAL_MS 1000
#define SAMPLE_INTERVAL_MS 50
#define BLE_HEARTBEAT_MS 500

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED0_NODE, gpios);

// Orientation gate owned by acquisition thread.
static struct gesture_engine gesture_control;

#define EMG_BASELINE_SAMPLES 40
#define EMG_BASELINE_INTERVAL_MS 50
#define EMG_BASELINE_TIMEOUT_MS 5000

static void blink_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(blink_work, blink_work_handler);

static void blink_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    gpio_pin_toggle_dt(&led);

    //LOG_INF("Uptime: %lld ms",
    //        (long long)k_uptime_get());

    k_work_reschedule(&blink_work, K_MSEC(500));
}


static int capture_emg_baseline(int32_t baseline_mv[2])
{
    if (baseline_mv == NULL) return -EINVAL;

    int64_t sum[2] = {0, 0};
    uint32_t count = 0;
    uint32_t last_sequence = UINT32_MAX;
    int64_t deadline = k_uptime_get() + EMG_BASELINE_TIMEOUT_MS;

    while (count < EMG_BASELINE_SAMPLES &&
           k_uptime_get() < deadline) {
        struct sensor_frame frame;

        if (sensor_stream_latest(&frame) == 0 &&
            frame.sequence != last_sequence) {
            last_sequence = frame.sequence;
            int32_t emg0 = frame.emg.mv[0];
            int32_t emg1 = frame.emg.mv[1];

            if (emg0 < 0 || emg0 > 3300 ||
                emg1 < 0 || emg1 > 3300) {
                return -EINVAL;
            }
            sum[0] += emg0;
            sum[1] += emg1;
            count++;
        }

        k_msleep(EMG_BASELINE_INTERVAL_MS);
    }

    if (count != EMG_BASELINE_SAMPLES) return -EAGAIN;

    baseline_mv[0] = (int32_t)(sum[0] / EMG_BASELINE_SAMPLES);
    baseline_mv[1] = (int32_t)(sum[1] / EMG_BASELINE_SAMPLES);
    return 0;
}

static int capture_startup_reference(
    struct imu_accel_sample *reference)
{
    if (reference == NULL) return -EINVAL;

    int64_t sum_x = 0, sum_y = 0, sum_z = 0;
    struct imu_accel_sample first = {0};
    float first_magnitude = 0.0f;

    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
        struct imu_accel_sample sample;
        int err = imu_read_accel(&sample);
        if (err < 0) return err;

        float x = (float)sample.x_mg;
        float y = (float)sample.y_mg;
        float z = (float)sample.z_mg;
        float magnitude = sqrtf(x*x + y*y + z*z);

        if (magnitude < 800.0f || magnitude > 1200.0f) {
            return -EAGAIN;
        }

        if (i == 0) {
            first = sample;
            first_magnitude = magnitude;
        } else {
            float dot =
                x * first.x_mg +
                y * first.y_mg +
                z * first.z_mg;

            // Reject calibration if the arm drifts by over 12 degrees.
            if (dot < CALIBRATION_MAX_DRIFT_COS *
                      magnitude * first_magnitude) {
                return -EAGAIN;
            }
        }

        sum_x += sample.x_mg;
        sum_y += sample.y_mg;
        sum_z += sample.z_mg;
        k_msleep(CALIBRATION_INTERVAL_MS);
    }

    reference->x_mg = (int32_t)(sum_x / CALIBRATION_SAMPLES);
    reference->y_mg = (int32_t)(sum_y / CALIBRATION_SAMPLES);
    reference->z_mg = (int32_t)(sum_z / CALIBRATION_SAMPLES);
    return 0;
}

int main(void)
{
    if (!gpio_is_ready_dt(&led)) {
        return 0;
    }

    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    LOG_INF("Hand Controlled Smart Light starting");

    k_work_schedule(&blink_work, K_NO_WAIT);

    k_msleep(3000);

    int imu_status = imu_init();

    LOG_INF("imu_init() returned %d", imu_status);

    if (imu_status < 0) {
        LOG_ERR("IMU initialization failed");
        return 0;
    }

    if (emg_init() < 0) {
        LOG_ERR("EMG ADC initialization failed");
        return 0;
    }

    // Capture one shared orientation reference at startup.
    struct imu_accel_sample reference = {0};

    while (1) {
        LOG_INF("STARTUP ORIENTATION CALIBRATION");
        LOG_INF("Hold wearable in your control position.");
        LOG_INF("Hold still during the 2-second capture.");
        LOG_INF("Starting calibration in 2 seconds...");
        k_msleep(2000);

        int err = capture_startup_reference(&reference);
        if (err == 0) {
            LOG_INF("Orientation calibration successful.");
            break;
        }
        LOG_WRN("Calibration failed (%d); retrying...", err);
        k_msleep(500);
    }

    LOG_INF("Gate reference: X=%d Y=%d Z=%d",
            reference.x_mg, reference.y_mg, reference.z_mg);
    LOG_INF("Reference applies to both Light and Laptop modes.");

    // Start BLE after sensor calibration
    if (ble_init() < 0) {
        LOG_ERR("BLE initialization failed");
        return 0;
    }
    // Start continuous EMG + IMU acquisition.
    int stream_err = sensor_stream_start(&reference);
    if (stream_err < 0) {
        LOG_ERR("Sensor stream failed: %d", stream_err);
        return 0;
    }
        
    // Rest-only calibration supports four discrete EMG classes.
    int32_t rest_mv[2] = {0, 0};
    bool emg_calibrated = false;

    for (int attempt = 1; attempt <= 3; attempt++) {
        LOG_INF("EMG REST CALIBRATION");
        LOG_INF("Relax your forearm completely.");
        LOG_INF("Starting in 2 seconds.");
        k_msleep(2000);

        int err = capture_emg_baseline(rest_mv);
        if (err == 0) {
            emg_calibrated = emg_inference_set_baseline(
                rest_mv[0], rest_mv[1]);
            if (emg_calibrated) {
                LOG_INF("EMG calibrated: EMG0=%d mV, EMG1=%d mV",
                        rest_mv[0], rest_mv[1]);
                break;
            }
            err = -EINVAL;
        }
        LOG_WRN("EMG calibration attempt %d failed: %d", attempt, err);
    }

    if (!emg_calibrated) {
        LOG_ERR("EMG calibration failed; reboot to retry.");
        LOG_ERR("IMU acquisition and BLE remain active.");
    }

    sensor_stream_clear_swipes();

    // Start command recognition in the disarmed state.
    gesture_engine_init(&gesture_control);

    uint32_t last_gesture_sequence = UINT32_MAX;


    uint8_t last_sent = 0xff;
    int64_t last_tx_ms = 0;
    int64_t last_log_ms = 0;
    int64_t last_sensor_warning_ms = 0;
    int last_reported_class = -2;

    while (1) {
        struct sensor_frame frame = {0};

        int ret = sensor_stream_latest(&frame);        

        bool valid = false;

        if (ret == 0) {

            valid = frame.orientation_valid;

            // Read the latest EMG ML prediction.
            emg_class_t predicted_class = EMG_REST;

            bool prediction_ready =
                emg_inference_latest(&predicted_class);

            int angle = frame.orientation_angle_deg;

            // Run command recognition only for fresh sensor data.
            uint32_t gesture_now = k_uptime_get_32();

            bool fresh_gesture_frame =
                frame.sequence != last_gesture_sequence &&
                (uint32_t)(gesture_now - frame.timestamp_ms) <= 150U;

            if (fresh_gesture_frame) {

                last_gesture_sequence = frame.sequence;

                // Read all pending IMU swipe events.
                // Ignore events older than 350 ms.
                struct swipe_notification notification;
                enum swipe_event pending_swipe = SWIPE_NONE;

                while (sensor_stream_pop_swipe(&notification)) {
                    if ((uint32_t)(
                            gesture_now - notification.timestamp_ms
                        ) <= 350U) {

                        pending_swipe = notification.event;
                    }
                }

                // Suppress FLEX candidates during substantial
                // wrist movement. Raw gyro units are mdps.
                // 35000 mdps = 35 degrees/second.
                const int32_t motion_threshold = 35000;

                bool significant_motion =
                    frame.motion.gyro.x_mdps > motion_threshold ||
                    frame.motion.gyro.x_mdps < -motion_threshold ||
                    frame.motion.gyro.y_mdps > motion_threshold ||
                    frame.motion.gyro.y_mdps < -motion_threshold ||
                    frame.motion.gyro.z_mdps > motion_threshold ||
                    frame.motion.gyro.z_mdps < -motion_threshold;

                // Interpret all seven discrete gestures. Swipes are active
                // in either receiver mode; the sensor thread gated onset.
                enum myo_gesture action = gesture_engine_update(
                    &gesture_control,
                    gesture_now,
                    valid,
                    prediction_ready,
                    prediction_ready ? predicted_class : EMG_UNKNOWN,
                    significant_motion,
                    pending_swipe
                );

                // An outside-gate, five-second TENSE changes receivers.
                // It is never forwarded as a normal TENSE event.
                if (gesture_engine_take_mode_switch(
                        &gesture_control)) {
                    LOG_INF("Long TENSE: changing receiver mode");
                    sensor_stream_clear_swipes();

                    int mode_err = myo_ble_client_toggle_mode();
                    if (mode_err) {
                        LOG_WRN("Mode switch error: %d", mode_err);
                    } else {
                        LOG_INF("Receiver mode: %s",
                            myo_ble_client_get_mode() == MYO_MODE_LIGHT
                                ? "LIGHT" : "LAPTOP");
                    }
                }

                if (action != MYO_GESTURE_NONE) {

                    // Existing BLE central already supports
                    // the two-byte gesture packet.
                    int send_err =
                        myo_ble_client_send_gesture(action);

                    if (send_err == 0) {
                        LOG_INF(
                            "Gesture command sent: %d",
                            (int)action
                        );
                    } else {
                        LOG_WRN(
                            "Gesture command %d not sent: %d",
                            (int)action,
                            send_err
                        );
                    }
                }
            }

            const char *gesture = "WARMUP";
            int class_code = -1;

            if (prediction_ready) {
                class_code = (int)predicted_class;

                switch (predicted_class) {
                case EMG_REST:
                    gesture = "REST";
                    break;

                case EMG_FLEX:
                    gesture = "FLEX";
                    break;

                case EMG_EXTEND:
                    gesture = "EXTEND";
                    break;

                case EMG_TENSE:
                    gesture = "TENSE";
                    break;

                default:
                    gesture = "UNKNOWN";
                    break;
                }
            }

            int64_t log_now = k_uptime_get();

            // Log periodically and when the classification changes.
            // Throttle changes to avoid flooding the console.
            if ((log_now - last_log_ms >= 250) ||
                (class_code != last_reported_class &&
                log_now - last_log_ms >= 100)) {

                LOG_INF(
                    "EMG0=%d mV | EMG1=%d mV | Gesture=%s | "
                    "Orientation=%s | Angle=%d",
                    frame.emg.mv[0], frame.emg.mv[1], gesture,
                    valid ? "VALID" : "INVALID", angle
                );

                last_reported_class = class_code;
                last_log_ms = log_now;
            }
        } else {
            // Never complete a hold based on missing/stale samples.
            gesture_engine_abort(&gesture_control);
            sensor_stream_clear_swipes();

            int64_t warning_now = k_uptime_get();
            if (warning_now - last_sensor_warning_ms >=
                SENSOR_WARNING_INTERVAL_MS) {
                LOG_WRN("Sensor frame unavailable or stale: %d", ret);
                last_sensor_warning_ms = warning_now;
            }

        }
        
        uint8_t state = valid ? MYO_VALID : MYO_INVALID;

        int64_t now = k_uptime_get();

        if (state != last_sent ||
            now - last_tx_ms >= BLE_HEARTBEAT_MS) {

            if (ble_send_state(state) == 0) {
                last_sent = state;
                last_tx_ms = now;
            }
        }
                
        // Light dimming is now a receiver-side mapped gesture action.
        k_msleep(SAMPLE_INTERVAL_MS);
    }

    return 0;
}
