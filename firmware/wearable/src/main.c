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
#include "brightness.h"
#include "emg_inference.h"
#include "gesture_engine.h"
#include "myo_ble_client.h"

LOG_MODULE_REGISTER(wearable, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
#define CALIBRATION_SAMPLES 100
#define SAMPLE_INTERVAL_MS 50
#define BLE_HEARTBEAT_MS 500

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static struct orientation_gate orientation;
static struct brightness_estimator brightness;
static struct gesture_engine gesture_control;

#define BRIGHTNESS_CAL_SAMPLES 40
#define BRIGHTNESS_CAL_INTERVAL_MS 50

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


static int capture_emg0_average_mv(int32_t *average_mv)
{
    int64_t sum = 0;
    int count = 0;

    uint32_t last_sequence = UINT32_MAX;
    int64_t deadline = k_uptime_get() + 5000;

    while (count < BRIGHTNESS_CAL_SAMPLES &&
           k_uptime_get() < deadline) {

        struct sensor_frame frame;

        if (sensor_stream_latest(&frame) == 0 &&
            frame.sequence != last_sequence) {

            last_sequence = frame.sequence;
            sum += frame.emg.mv[0];
            count++;
        }

        k_msleep(BRIGHTNESS_CAL_INTERVAL_MS);
    }

    if (count != BRIGHTNESS_CAL_SAMPLES) {
        LOG_ERR("EMG calibration failed: %d samples", count);
        return -1;
    }

    *average_mv = (int32_t)(sum / count);
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

    LOG_INF("Hold wearable in control orientation");
    LOG_INF("Calibrating in 2 seconds...");

    k_msleep(2000);

    int valid_samples = 0;
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int64_t sum_z = 0;

    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {

        struct imu_accel_sample sample;

        if (imu_read_accel(&sample) == 0) {

            sum_x += sample.x_mg;
            sum_y += sample.y_mg;
            sum_z += sample.z_mg;
            valid_samples++;
        }

        k_msleep(20);
    }
    
    if (valid_samples == 0) {
        LOG_ERR("No valid IMU calibration samples");
        return 0;
    }

    struct imu_accel_sample reference = {
        .x_mg = sum_x / valid_samples,
        .y_mg = sum_y / valid_samples,
        .z_mg = sum_z / valid_samples
    };

    orientation_set_reference(
        &orientation,
        &reference);

    LOG_INF("Reference: x=%d y=%d z=%d",
            reference.x_mg,
            reference.y_mg,
            reference.z_mg);
    
    
    // Start BLE after sensor calibration
    if (ble_init() < 0) {
        LOG_ERR("BLE initialization failed");
        return 0;
    }
    // Start continuous EMG + IMU acquisition.
    sensor_stream_start();
        
    // Brightness calibration uses EMG0.
    // Keep the arm in its calibrated control orientation.

    int32_t rest_mv = 0;
    int32_t extended_mv = 0;
    bool brightness_ready = false;

    LOG_INF("BRIGHTNESS CALIBRATION");
    LOG_INF("Relax your forearm. Starting in 2 seconds.");

    k_msleep(2000);

    int rest_status = capture_emg0_average_mv(&rest_mv);

    if (rest_status == 0) {
        LOG_INF("Rest baseline: %d mV", rest_mv);
    }

    LOG_INF("Extend your wrist comfortably.");
    LOG_INF("Hold steady. Starting in 2 seconds.");

    k_msleep(2000);

    int extend_status =
        capture_emg0_average_mv(&extended_mv);

    if (extend_status == 0) {
        LOG_INF("Extension level: %d mV", extended_mv);
    }

    if (rest_status == 0 &&
        extend_status == 0 &&
        extended_mv > rest_mv + 3) {

        brightness_init(
            &brightness,
            (float)rest_mv,
            (float)extended_mv
        );

        brightness_ready = true;

        LOG_INF("Brightness calibration complete");
        LOG_INF("Range: %d to %d mV",
                rest_mv, extended_mv);

    } else {
        LOG_WRN("Brightness calibration invalid");
        LOG_WRN("Brightness disabled until reboot");
        // Existing BLE behavior still continues.
    }

    uint32_t last_brightness_sequence = UINT32_MAX;
    // Ignore gesture events collected during calibration.
    sensor_stream_clear_swipes();

    // Start command recognition in the disarmed state.
    gesture_engine_init(&gesture_control);

    uint32_t last_gesture_sequence = UINT32_MAX;


    uint8_t last_sent = 0xff;
    int64_t last_tx_ms = 0;
    int64_t last_log_ms = 0;
    int last_reported_class = -2;

    while (1) {
        struct sensor_frame frame = {0};

        int ret = sensor_stream_latest(&frame);        

        struct imu_accel_sample accel =
            frame.motion.accel;

        bool valid = false;

        if (ret == 0) {

            valid =
                orientation_update(
                    &orientation,
                    &accel);
                        
            // Update brightness only when a fresh sensor
            // frame is available and calibration succeeded.
            if (brightness_ready &&
                frame.sequence != last_brightness_sequence) {

                brightness_update(
                    &brightness,
                    frame.emg.mv[0],
                    frame.timestamp_ms,
                    valid
                );

                last_brightness_sequence = frame.sequence;
            }

            
            int angle =
                orientation_angle_deg(
                    &orientation,
                    &accel);

            // Retrieve the most recent ML prediction.
            emg_class_t predicted_class;
            bool prediction_ready =
                emg_inference_latest(&predicted_class);
            
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

                // Combine orientation, EMG, and IMU into
                // one actionable gesture.
                enum myo_gesture action = gesture_engine_update(
                    &gesture_control,
                    gesture_now,
                    valid,
                    prediction_ready,
                    prediction_ready ? predicted_class : EMG_REST,
                    significant_motion,
                    pending_swipe
                );

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
                    "EMG0=%d mV | Gesture=%s | "
                    "Brightness=%u%% | Orientation=%s | Angle=%d",
                    frame.emg.mv[0],
                    gesture,
                    brightness_get(&brightness),
                    valid ? "VALID" : "INVALID",
                    angle
                );

                last_reported_class = class_code;
                last_log_ms = log_now;
            }
        } else {
            LOG_ERR("Accelerometer read failed: %d", ret);

            // Reset gate after a failed reading
            orientation.valid = false;
            orientation.candidate_since_ms = -1;
            if (brightness_ready) {
                brightness_update(
                    &brightness,
                    0,
                    k_uptime_get_32(),
                    false
                );
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

        k_msleep(SAMPLE_INTERVAL_MS);
    }

    return 0;
}