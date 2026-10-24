#ifndef SWIPE_CONFIG_H
#define SWIPE_CONFIG_H

// Generated from training.csv by analyze_swipes.py
#define SWIPE_THRESHOLD_MDPS 100000.00f
#define SWIPE_SIMILARITY 0.75000f

static const float swipe_centers[4][3] = {
    {0.48681577f, 0.20604222f, 0.84885630f}, // UP
    {-0.24785536f, 0.58038338f, -0.77570797f}, // DOWN
    {0.81636970f, 0.26121223f, -0.51508124f}, // LEFT
    {-0.67543457f, 0.05956263f, 0.73501050f}, // RIGHT
};

#endif
