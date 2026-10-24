#ifndef EMG_INFERENCE_H
#define EMG_INFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "emg_model.h"

// Called once for every valid 100 Hz EMG sample.
void emg_inference_push(int32_t emg0_mv,
                        uint32_t sequence,
                        uint32_t timestamp_ms);

// Get the most recent prediction.
// Returns false until the first 40-sample window is ready.
bool emg_inference_latest(emg_class_t *result);

#endif
