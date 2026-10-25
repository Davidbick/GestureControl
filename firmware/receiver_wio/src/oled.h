
#ifndef MYO_OLED_H
#define MYO_OLED_H

#include <stdbool.h>
#include <stdint.h>

enum oled_rx_type {
    OLED_RX_NONE = 0,
    OLED_RX_GESTURE = 1,
    OLED_RX_BRIGHTNESS = 2
};

struct oled_status {
    bool connected;
    bool valid;
    bool fresh;
    bool light_on;

    uint8_t brightness;

    enum oled_rx_type rx_type;
    uint8_t first_byte;
    uint8_t second_byte;
};

int oled_init(void);
int oled_show(const struct oled_status *status);

#endif
