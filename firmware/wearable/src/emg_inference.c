#include "emg_inference.h"

#include <stdint.h>
#include <zephyr/sys/atomic.h>

#define EMG_WINDOW 40U
#define EMG_HOP 10U
#define EMG_MAX_GAP_MS 25U

#if EMG_FEATURE_COUNT != 4
#error "EMG inference requires four EMG0 features"
#endif

// Only the acquisition thread writes this buffer.
static int32_t samples[EMG_WINDOW];

static uint32_t write_index;
static uint32_t samples_seen;

static uint32_t previous_sequence;
static uint32_t previous_timestamp;
static bool have_previous;

// 0 = warming up / unavailable
// 1 = REST, 2 = FLEX, 3 = EXTEND
// Atomic because the acquisition and main threads
// access this value concurrently.
static atomic_t latest_state;

static void reset_window(void)
{
    write_index = 0;
    samples_seen = 0;
    atomic_set(&latest_state, 0);
}

void emg_inference_push(int32_t emg0_mv,
                        uint32_t sequence,
                        uint32_t timestamp_ms)
{
    if (have_previous) {
        uint32_t delta_ms =
            timestamp_ms - previous_timestamp;

        if (sequence != previous_sequence + 1U ||
            delta_ms == 0U ||
            delta_ms > EMG_MAX_GAP_MS) {
            reset_window();
        }
    }

    previous_sequence = sequence;
    previous_timestamp = timestamp_ms;
    have_previous = true;

    if (emg0_mv < 0 || emg0_mv > 3300) {
        reset_window();
        have_previous = false;
        return;
    }

    samples[write_index] = emg0_mv;
    write_index = (write_index + 1U) % EMG_WINDOW;
    samples_seen++;

    // Wait for a complete 400 ms window.
    if (samples_seen < EMG_WINDOW) {
        return;
    }

    // Predict at 40, 50, 60 ... samples.
    if ((samples_seen - EMG_WINDOW) % EMG_HOP != 0U) {
        return;
    }

    int32_t minimum = samples[0];
    int32_t maximum = samples[0];
    int64_t sum = 0;

    for (uint32_t i = 0; i < EMG_WINDOW; i++) {
        int32_t value = samples[i];

        sum += value;

        if (value < minimum) {
            minimum = value;
        }

        if (value > maximum) {
            maximum = value;
        }
    }

    float mean = (float)sum / (float)EMG_WINDOW;

    float variance_sum = 0.0f;

    for (uint32_t i = 0; i < EMG_WINDOW; i++) {
        float difference = (float)samples[i] - mean;
        variance_sum += difference * difference;
    }

    float variance =
        variance_sum / (float)EMG_WINDOW;

    float range = (float)(maximum - minimum);

    // EXACT Python feature ordering:
    // mean[0], maximum[0], variance[0], span[0]
    float features[EMG_FEATURE_COUNT] = {
        mean,
        (float)maximum,
        variance,
        range
    };

    emg_class_t result =
        emg_model_predict(features);

    atomic_set(&latest_state, (atomic_val_t)result + 1);
}

bool emg_inference_latest(emg_class_t *result)
{
    if (result == NULL) {
        return false;
    }

    atomic_val_t state = atomic_get(&latest_state);

    if (state < 1 || state > 3) {
        return false;
    }

    *result = (emg_class_t)(state - 1);
    return true;
}
