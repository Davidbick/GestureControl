#ifndef SWIPE_TUNING_H
#define SWIPE_TUNING_H

// Demo-tuned swipe parameters. Direction vectors are in swipe_config.h.
// Orientation gate is diagnostic only; normal swipes are ungated.
// Angular velocities use millidegrees per second.
#define SWIPE_ONSET_MDPS           60000.0f
#define SWIPE_PREMOTION_MDPS       30000.0f
#define SWIPE_NEUTRAL_MDPS         20000.0f
#define SWIPE_MIN_CAPTURE_MDPS     35000.0f
#define SWIPE_MIN_SIMILARITY           0.65f
#define SWIPE_MIN_SCORE_MARGIN         0.12f

#define SWIPE_CAPTURE_MS              80U
#define SWIPE_GATE_GRACE_MS          200U
#define SWIPE_NEUTRAL_HOLD_MS        150U
#define SWIPE_LOCKOUT_MS            500U
#define SWIPE_SAMPLE_GAP_MS          40U

#endif
