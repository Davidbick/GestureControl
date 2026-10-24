#ifndef IMU_H
#define IMU_H

#include <stdint.h>

struct imu_accel_sample {
    int32_t x_mg;
    int32_t y_mg;
    int32_t z_mg;
};

int imu_init(void);
int imu_read_accel(struct imu_accel_sample *sample);

#endif
