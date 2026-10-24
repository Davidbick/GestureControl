#ifndef EMG_MODEL_H
#define EMG_MODEL_H

typedef enum {
    EMG_REST = 0,
    EMG_FLEX = 1,
    EMG_EXTEND = 2
} emg_class_t;

#define EMG_FEATURE_COUNT 4

emg_class_t emg_model_predict(
    const float features[EMG_FEATURE_COUNT]);

#endif
