#ifndef SWIPE_CONFIG_H
#define SWIPE_CONFIG_H

// Direction signatures from the uploaded swipe training CSV.
// Captured using the smoothed gyro vector over 80 ms after the
// first 60,000 mdps onset, no orientation gate.
// Held-out offline validation: 25/25 trial-level classifications.
// Class order: UP, DOWN, LEFT, RIGHT.
static const float swipe_centers[4][3] = {
    { 0.14855055f,  0.71806234f,  0.67994059f}, // UP
    {-0.89404937f, -0.29633313f, -0.33595000f}, // DOWN
    { 0.63646477f,  0.29104911f, -0.71428497f}, // LEFT
    {-0.52750536f,  0.02021194f,  0.84931123f}, // RIGHT
};

#endif
