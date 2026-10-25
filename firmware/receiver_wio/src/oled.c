#include "oled.h"
#include "myo_ble.h"

#include <errno.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>

static const struct device *const oled =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

int oled_init(void)
{
    if (!device_is_ready(oled)) {
        return -ENODEV;
    }

    int err = display_set_pixel_format(
        oled, PIXEL_FORMAT_MONO10);

    if (err) {
        err = display_set_pixel_format(
            oled, PIXEL_FORMAT_MONO01);

        if (err) {
            return err;
        }
    }

    err = cfb_framebuffer_init(oled);
    if (err) {
        return err;
    }

    err = cfb_framebuffer_clear(oled, true);
    if (err) {
        return err;
    }

    err = display_blanking_off(oled);

    if (err && err != -ENOSYS) {
        return err;
    }

    int fonts = cfb_get_numof_fonts(oled);

    for (int i = 0; i < fonts; i++) {
        uint8_t width, height;

        if (cfb_get_font_size(
                oled, i, &width, &height) == 0 &&
            width <= 10 && height <= 16) {

            return cfb_framebuffer_set_font(oled, i);
        }
    }

    return -ENOTSUP;
}

// Decode the actual gesture byte.
static const char *gesture_text(uint8_t gesture)
{
    switch (gesture) {
    case MYO_GESTURE_SWIPE_UP:
        return "SWIPE UP";

    case MYO_GESTURE_SWIPE_DOWN:
        return "SWIPE DOWN";

    case MYO_GESTURE_SWIPE_LEFT:
        return "SWIPE LEFT";

    case MYO_GESTURE_SWIPE_RIGHT:
        return "SWIPE RIGHT";

    case MYO_GESTURE_TENSE:
        return "TENSE";

    default:
        return "UNKNOWN";
    }
}

int oled_show(const struct oled_status *status)
{
    if (status == NULL) {
        return -EINVAL;
    }

    char line0[24];
    char line1[24];
    char line2[24];
    char line3[24];

    // Row 1: BLE connection and orientation status.
    // V:01 means valid, V:00 means invalid.
    // V:-- means no fresh validity heartbeat.
    if (!status->connected) {
        snprintf(line0, sizeof(line0),
                 "BLE:WAIT");
    } else if (!status->fresh) {
        snprintf(line0, sizeof(line0),
                 "BLE:ON V:--");
    } else {
        snprintf(line0, sizeof(line0),
                 "BLE:ON V:%02X",
                 status->valid ? 1U : 0U);
    }

    // Rows 2 and 3: exact received packet bytes
    // and their decoded meaning.
    switch (status->rx_type) {

    case OLED_RX_GESTURE:
        snprintf(line1, sizeof(line1),
                 "RX G:%02X %02X",
                 status->first_byte,
                 status->second_byte);

        snprintf(line2, sizeof(line2),
                 "%s",
                 gesture_text(status->first_byte));
        break;

    case OLED_RX_BRIGHTNESS:
        snprintf(line1, sizeof(line1),
                 "RX B:%02X %02X",
                 status->first_byte,
                 status->second_byte);

        snprintf(line2, sizeof(line2),
                 "BRIGHT %u%%",
                 status->first_byte);
        break;

    case OLED_RX_NONE:
    default:
        snprintf(line1, sizeof(line1),
                 "RX:--");

        snprintf(line2, sizeof(line2),
                 "NO COMMAND");
        break;
    }

    // Row 4: current commanded light state and
    // saved brightness.
    snprintf(line3, sizeof(line3),
             "LED %s %u%%",
             status->light_on ? "ON" : "OFF",
             status->brightness);

    int err = cfb_framebuffer_clear(oled, false);
    if (err) return err;

    err = cfb_print(oled, line0, 0, 0);
    if (err) return err;

    err = cfb_print(oled, line1, 0, 16);
    if (err) return err;

    err = cfb_print(oled, line2, 0, 32);
    if (err) return err;

    err = cfb_print(oled, line3, 0, 48);
    if (err) return err;

    return cfb_framebuffer_finalize(oled);
}
