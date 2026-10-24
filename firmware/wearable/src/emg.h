#ifndef EMG_H
#define EMG_H

#include <stdint.h>

#define EMG_CHANNEL_COUNT 2

struct emg_sample {
    int16_t raw[EMG_CHANNEL_COUNT];
    int32_t mv[EMG_CHANNEL_COUNT];
};

int emg_init(void);
int emg_read(struct emg_sample *sample);

#endif
