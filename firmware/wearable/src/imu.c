#include "imu.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

static const struct device *const imu_dev =
    DEVICE_DT_GET_ONE(st_lsm6dsl);

static int32_t gyro_to_mdps(const struct sensor_value *value)
{
    int64_t micro_rad =
        ((int64_t)value->val1 * 1000000LL) +
        value->val2;

    return (int32_t)(
        (micro_rad * 180000LL) / 3141593LL);
}

int imu_init(void){
    if (!device_is_ready(imu_dev)) {
        LOG_ERR("IMU device is not ready");
        return -ENODEV;
    }

    struct sensor_value odr = {
        .val1 = 104,
        .val2 = 0
    };

    int ret = sensor_attr_set(
        imu_dev,
        SENSOR_CHAN_ACCEL_XYZ,
        SENSOR_ATTR_SAMPLING_FREQUENCY,
        &odr);

    if (ret < 0) {
        LOG_ERR("Failed to set accelerometer sampling rate: %d", ret);
        return ret;
    }

     ret = sensor_attr_set(
        imu_dev,
        SENSOR_CHAN_GYRO_XYZ,
        SENSOR_ATTR_SAMPLING_FREQUENCY,
        &odr);

    if (ret < 0) {
        LOG_ERR("Gyroscope ODR failed: %d", ret);
        return ret;
    }

    LOG_INF("IMU initialized: accel + gyro at 104 Hz");

    return 0;
}

int imu_read_motion(struct imu_motion_sample *sample)
{
    if (sample == NULL) {
        return -EINVAL;
    }

    struct sensor_value accel[3];
    struct sensor_value gyro[3];

    int ret = sensor_sample_fetch(imu_dev);

    if (ret < 0) {
        return ret;
    }

    ret = sensor_channel_get(
        imu_dev,
        SENSOR_CHAN_ACCEL_XYZ,
        accel);

    if (ret < 0) {
        return ret;
    }

    ret = sensor_channel_get(
        imu_dev,
        SENSOR_CHAN_GYRO_XYZ,
        gyro);

    if (ret < 0) {
        return ret;
    }

    sample->accel.x_mg = sensor_ms2_to_mg(&accel[0]);
    sample->accel.y_mg = sensor_ms2_to_mg(&accel[1]);
    sample->accel.z_mg = sensor_ms2_to_mg(&accel[2]);

    sample->gyro.x_mdps = gyro_to_mdps(&gyro[0]);
    sample->gyro.y_mdps = gyro_to_mdps(&gyro[1]);
    sample->gyro.z_mdps = gyro_to_mdps(&gyro[2]);

    return 0;
}


int imu_read_accel(struct imu_accel_sample *sample){
    if (sample == NULL) {
        return -EINVAL;
    }

    struct imu_motion_sample motion;

    int ret = imu_read_motion(&motion);

    if (ret == 0) {
        *sample = motion.accel;
    }

    return ret;
}
