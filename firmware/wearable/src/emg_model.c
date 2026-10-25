#include "emg_model.h"

// Depth-3 decision tree trained on the recorded four-class EMG dataset.
// 400 ms windows, 100 ms hop, raw mV; all 8 features remain available.
// Independent validation: 375/406 windows = 92.36% accuracy.
// Do not subtract baseline: this tree was fit on absolute input voltages.
emg_class_t emg_model_predict(
    const float features[EMG_FEATURE_COUNT],
    const float baseline_mv[2])
{
    if (features == 0) return EMG_UNKNOWN;
    (void)baseline_mv;

    if (features[4] <= 6.936249924e+01f) {
        if (features[0] <= 7.766249847e+01f) {
            return EMG_REST;
        } else {
            return EMG_EXTEND;
        }
    } else {
        if (features[4] <= 8.092874756e+02f) {
            if (features[5] <= 3.665000000e+02f) {
                return EMG_EXTEND;
            } else {
                return EMG_TENSE;
            }
        } else {
            if (features[1] <= 5.905000000e+02f) {
                return EMG_FLEX;
            } else {
                return EMG_FLEX;
            }
        }
    }
}
