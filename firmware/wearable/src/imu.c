#include "imu.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

static const struct device *const imu_dev =
    DEVICE_DT_GET_ONE(st_lsm6dsl);

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

    LOG_INF("IMU initialized");

    return 0;
}

int imu_read_accel(struct imu_accel_sample *sample){
    struct sensor_value x;
    struct sensor_value y;
    struct sensor_value z;

    int ret = sensor_sample_fetch_chan(
        imu_dev,
        SENSOR_CHAN_ACCEL_XYZ);

    if (ret < 0) {
        return ret;
    }

    ret = sensor_channel_get(
        imu_dev,
        SENSOR_CHAN_ACCEL_X,
        &x);

    if (ret < 0) {
        return ret;
    }

    ret = sensor_channel_get(
        imu_dev,
        SENSOR_CHAN_ACCEL_Y,
        &y);

    if (ret < 0) {
        return ret;
    }

    ret = sensor_channel_get(
        imu_dev,
        SENSOR_CHAN_ACCEL_Z,
        &z);

    if (ret < 0) {
        return ret;
    }

    sample->x_mg = sensor_ms2_to_mg(&x);
    sample->y_mg = sensor_ms2_to_mg(&y);
    sample->z_mg = sensor_ms2_to_mg(&z);

    return 0;
}
