#ifndef IMU_H
#define IMU_H

#include <stdint.h>

struct imu_accel_sample {
    int32_t x_mg;
    int32_t y_mg;
    int32_t z_mg;
};


struct imu_gyro_sample {
    int32_t x_mdps;
    int32_t y_mdps;
    int32_t z_mdps;
};

struct imu_motion_sample {
    struct imu_accel_sample accel;
    struct imu_gyro_sample gyro;
};


int imu_init(void);
// Read all six axes using one sensor fetch.
int imu_read_motion(struct imu_motion_sample *sample);
int imu_read_accel(struct imu_accel_sample *sample);

#endif
