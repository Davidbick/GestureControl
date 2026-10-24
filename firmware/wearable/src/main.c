#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "imu.h"
#include "orientation.h"

LOG_MODULE_REGISTER(wearable, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
#define CALIBRATION_SAMPLES 100

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static struct orientation_gate orientation;

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

    LOG_INF("Hold wearable in control orientation");
    LOG_INF("Calibrating in 2 seconds...");

    k_msleep(2000);

    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int64_t sum_z = 0;

    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {

        struct imu_accel_sample sample;

        if (imu_read_accel(&sample) == 0) {

            sum_x += sample.x_mg;
            sum_y += sample.y_mg;
            sum_z += sample.z_mg;
        }

        k_msleep(20);
    }

    struct imu_accel_sample reference = {
        .x_mg = sum_x / CALIBRATION_SAMPLES,
        .y_mg = sum_y / CALIBRATION_SAMPLES,
        .z_mg = sum_z / CALIBRATION_SAMPLES
    };

    orientation_set_reference(
        &orientation,
        &reference);

    LOG_INF("Reference: x=%d y=%d z=%d",
            reference.x_mg,
            reference.y_mg,
            reference.z_mg);

    while (1) {
        struct imu_accel_sample accel;

        int ret = imu_read_accel(&accel);

        if (ret == 0) {

            bool valid =
                orientation_update(
                    &orientation,
                    &accel);

            int angle =
                orientation_angle_deg(
                    &orientation,
                    &accel);

            LOG_INF(
                "Angle=%d deg | Orientation=%s | x=%d y=%d z=%d",
                angle,
                valid ? "VALID" : "INVALID",
                accel.x_mg,
                accel.y_mg,
                accel.z_mg);
        } else {
            LOG_ERR("Accelerometer read failed: %d", ret);
        }
        
        k_msleep(1000);
    }

    return 0;
}