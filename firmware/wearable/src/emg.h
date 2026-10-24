#ifndef EMG_H
#define EMG_H

#include <stdint.h>

struct emg_sample {
    int16_t raw;
    int32_t mv;
};

int emg_init(void);
int emg_read(struct emg_sample *sample);

#endif
