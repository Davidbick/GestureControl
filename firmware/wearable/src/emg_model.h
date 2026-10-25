#ifndef EMG_MODEL_H
#define EMG_MODEL_H

typedef enum {
    EMG_REST = 0,
    EMG_FLEX = 1,
    EMG_EXTEND = 2,
    EMG_TENSE = 3,
    EMG_UNKNOWN = 4
} emg_class_t;

// mean, max, variance and range for EMG0 and EMG1
#define EMG_FEATURE_COUNT 8

emg_class_t emg_model_predict(
    const float features[EMG_FEATURE_COUNT],
    const float baseline_mv[2]);

#endif
