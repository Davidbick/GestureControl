#ifndef EMG_INFERENCE_H
#define EMG_INFERENCE_H

#include <stdbool.h>
#include <stdint.h>
#include "emg_model.h"

// Recalibrates resting voltage on both sensors, resets inference.
bool emg_inference_set_baseline(int32_t emg0_mv, int32_t emg1_mv);

// Feed every 100 Hz acquisition sample.
void emg_inference_push(
    int32_t emg0_mv,
    int32_t emg1_mv,
    uint32_t sequence,
    uint32_t timestamp_ms);

// Called on a sensor read error; preserves the resting baseline.
void emg_inference_invalidate(void);

// False during warmup, gaps or ambiguous readings.
bool emg_inference_latest(emg_class_t *result);

#endif
