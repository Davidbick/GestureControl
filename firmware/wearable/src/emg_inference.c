#include "emg_inference.h"

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define EMG_CHANNELS 2U
#define EMG_WINDOW 40U
#define EMG_HOP 10U
#define EMG_MAX_GAP_MS 25U
#define CONFIRM_WINDOWS 2U
#define UNKNOWN_TIMEOUT_WINDOWS 3U

#if EMG_FEATURE_COUNT != 8
#error "Dual-channel inference requires eight EMG features"
#endif

K_MUTEX_DEFINE(emg_inference_lock);

static int32_t samples[EMG_CHANNELS][EMG_WINDOW];
static float baseline_mv[EMG_CHANNELS];
static bool baseline_ready;

static uint32_t write_index;
static uint32_t samples_seen;
static uint32_t previous_sequence;
static uint32_t previous_timestamp;
static bool have_previous;

static emg_class_t stable_class = EMG_UNKNOWN;
static emg_class_t candidate_class = EMG_UNKNOWN;
static uint32_t candidate_windows;
static uint32_t unknown_windows;

// Atomically read by the main thread. 0=unavailable; 1..4=class+1.
static atomic_t latest_state;

static void reset_prediction_locked(void)
{
    write_index = 0;
    samples_seen = 0;
    have_previous = false;
    stable_class = EMG_UNKNOWN;
    candidate_class = EMG_UNKNOWN;
    candidate_windows = 0;
    unknown_windows = 0;
    atomic_set(&latest_state, 0);
}

bool emg_inference_set_baseline(int32_t emg0_mv, int32_t emg1_mv)
{
    if (emg0_mv < 0 || emg0_mv > 3300 ||
        emg1_mv < 0 || emg1_mv > 3300) {
        return false;
    }

    k_mutex_lock(&emg_inference_lock, K_FOREVER);
    baseline_mv[0] = (float)emg0_mv;
    baseline_mv[1] = (float)emg1_mv;
    reset_prediction_locked();
    baseline_ready = true;
    k_mutex_unlock(&emg_inference_lock);
    return true;
}

void emg_inference_invalidate(void)
{
    k_mutex_lock(&emg_inference_lock, K_FOREVER);
    reset_prediction_locked();
    k_mutex_unlock(&emg_inference_lock);
}

static void update_stable_class(emg_class_t predicted)
{
    if (predicted == EMG_UNKNOWN) {
        unknown_windows++;
        candidate_class = EMG_UNKNOWN;
        candidate_windows = 0;

        if (unknown_windows >= UNKNOWN_TIMEOUT_WINDOWS) {
            stable_class = EMG_UNKNOWN;
            atomic_set(&latest_state, 0);
        }
        return;
    }

    unknown_windows = 0;

    if (predicted == stable_class) {
        candidate_class = EMG_UNKNOWN;
        candidate_windows = 0;
        return;
    }

    if (predicted != candidate_class) {
        candidate_class = predicted;
        candidate_windows = 1;
        return;
    }

    candidate_windows++;
    if (candidate_windows >= CONFIRM_WINDOWS) {
        stable_class = predicted;
        candidate_class = EMG_UNKNOWN;
        candidate_windows = 0;
        atomic_set(&latest_state, (atomic_val_t)stable_class + 1);
    }
}

void emg_inference_push(
    int32_t emg0_mv,
    int32_t emg1_mv,
    uint32_t sequence,
    uint32_t timestamp_ms)
{
    k_mutex_lock(&emg_inference_lock, K_FOREVER);

    if (!baseline_ready) {
        k_mutex_unlock(&emg_inference_lock);
        return;
    }

    if (emg0_mv < 0 || emg0_mv > 3300 ||
        emg1_mv < 0 || emg1_mv > 3300) {
        reset_prediction_locked();
        k_mutex_unlock(&emg_inference_lock);
        return;
    }

    if (have_previous) {
        uint32_t dt = timestamp_ms - previous_timestamp;
        if (sequence != previous_sequence + 1U ||
            dt == 0U || dt > EMG_MAX_GAP_MS) {
            reset_prediction_locked();
        }
    }

    previous_sequence = sequence;
    previous_timestamp = timestamp_ms;
    have_previous = true;
    samples[0][write_index] = emg0_mv;
    samples[1][write_index] = emg1_mv;
    write_index = (write_index + 1U) % EMG_WINDOW;
    samples_seen++;

    // First complete window takes 400 ms. Predict each 100 ms.
    if (samples_seen < EMG_WINDOW ||
        (samples_seen - EMG_WINDOW) % EMG_HOP != 0U) {
        k_mutex_unlock(&emg_inference_lock);
        return;
    }

    float features[EMG_FEATURE_COUNT];

    for (uint32_t channel = 0; channel < EMG_CHANNELS; channel++) {
        int32_t minimum = samples[channel][0];
        int32_t maximum = samples[channel][0];
        int64_t sum = 0;

        for (uint32_t i = 0; i < EMG_WINDOW; i++) {
            int32_t value = samples[channel][i];
            sum += value;
            if (value < minimum) minimum = value;
            if (value > maximum) maximum = value;
        }

        float mean = (float)sum / (float)EMG_WINDOW;
        float variance_sum = 0.0f;

        for (uint32_t i = 0; i < EMG_WINDOW; i++) {
            float difference = (float)samples[channel][i] - mean;
            variance_sum += difference * difference;
        }

        uint32_t idx = channel * 4U;
        features[idx] = mean;
        features[idx + 1U] = (float)maximum;
        features[idx + 2U] = variance_sum / (float)EMG_WINDOW;
        features[idx + 3U] = (float)(maximum - minimum);
    }

    update_stable_class(emg_model_predict(features, baseline_mv));
    k_mutex_unlock(&emg_inference_lock);
}

bool emg_inference_latest(emg_class_t *result)
{
    if (result == NULL) return false;

    atomic_val_t state = atomic_get(&latest_state);
    if (state < 1 || state > 4) return false;

    *result = (emg_class_t)(state - 1);
    return true;
}
