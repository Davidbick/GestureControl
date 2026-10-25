#ifndef WIO_RGB_LED_H
#define WIO_RGB_LED_H

#include <stdint.h>

/* Raw pin channels, not color names: color order is being measured. */
enum wio_rgb_channel {
    WIO_RGB_PIN_02 = 0,
    WIO_RGB_PIN_09,
    WIO_RGB_PIN_29,
    WIO_RGB_CHANNEL_COUNT
};

/* The common-anode RGB LED uses active-low PWM outputs. */
int wio_rgb_init(void);
int wio_rgb_set_channel(enum wio_rgb_channel channel, uint8_t percent);
int wio_rgb_all_off(void);

/* Boot-time pin/color/brightness test; ends with all channels off. */
int wio_rgb_self_test(void);

#endif
