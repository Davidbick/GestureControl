#include "emg_model.h"

// Provisional thresholds: tune using recorded EMG data.
#define REST_MAX_DELTA_MV       35.0f
#define ACTION_MIN_DELTA_MV     85.0f
#define TENSE_MIN_DELTA_MV      70.0f
#define DOMINANCE_RATIO          1.65f
#define DOMINANCE_MARGIN_MV     25.0f
#define TENSE_BALANCE_RATIO      0.50f

static float positive(float value)
{
    return value > 0.0f ? value : 0.0f;
}

emg_class_t emg_model_predict(
    const float features[EMG_FEATURE_COUNT],
    const float baseline_mv[2])
{
    if (features == 0 || baseline_mv == 0) return EMG_UNKNOWN;

    // The initial classifier uses the two channel means above baseline.
    // All eight features are available for a trained model later.
    float emg0 = positive(features[0] - baseline_mv[0]);
    float emg1 = positive(features[4] - baseline_mv[1]);

    if (emg0 <= REST_MAX_DELTA_MV &&
        emg1 <= REST_MAX_DELTA_MV) {
        return EMG_REST;
    }

    if (emg0 >= TENSE_MIN_DELTA_MV &&
        emg1 >= TENSE_MIN_DELTA_MV &&
        emg0 >= emg1 * TENSE_BALANCE_RATIO &&
        emg1 >= emg0 * TENSE_BALANCE_RATIO) {
        return EMG_TENSE;
    }

    if (emg0 >= ACTION_MIN_DELTA_MV &&
        emg0 >= emg1 * DOMINANCE_RATIO +
                DOMINANCE_MARGIN_MV) {
        return EMG_EXTEND;
    }

    if (emg1 >= ACTION_MIN_DELTA_MV &&
        emg1 >= emg0 * DOMINANCE_RATIO +
                DOMINANCE_MARGIN_MV) {
        return EMG_FLEX;
    }

    return EMG_UNKNOWN;
}
