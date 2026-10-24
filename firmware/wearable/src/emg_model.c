#include "emg_model.h"

emg_class_t emg_model_predict(
    const float features[EMG_FEATURE_COUNT])
{
    if (features[0] <= 7.891000061e+02f) {
        if (features[1] <= 3.535000000e+02f) {
            if (features[2] <= 1.274959412e+03f) {
                if (features[0] <= 8.198749924e+01f) {
                    return EMG_REST;
                } else {
                    return EMG_REST;
                }
            } else {
                if (features[1] <= 3.105000000e+02f) {
                    return EMG_FLEX;
                } else {
                    return EMG_REST;
                }
            }
        } else {
            if (features[2] <= 1.047375898e+05f) {
                if (features[0] <= 2.629125061e+02f) {
                    return EMG_FLEX;
                } else {
                    return EMG_FLEX;
                }
            } else {
                return EMG_EXTEND;
            }
        }
    } else {
        if (features[0] <= 1.129800049e+03f) {
            if (features[2] <= 4.155788672e+04f) {
                if (features[2] <= 3.173604004e+04f) {
                    return EMG_EXTEND;
                } else {
                    return EMG_EXTEND;
                }
            } else {
                if (features[3] <= 1.286000000e+03f) {
                    return EMG_FLEX;
                } else {
                    return EMG_EXTEND;
                }
            }
        } else {
            if (features[3] <= 2.030000000e+03f) {
                if (features[3] <= 1.284000000e+03f) {
                    return EMG_EXTEND;
                } else {
                    return EMG_EXTEND;
                }
            } else {
                if (features[3] <= 2.695500000e+03f) {
                    return EMG_EXTEND;
                } else {
                    return EMG_EXTEND;
                }
            }
        }
    }
}
