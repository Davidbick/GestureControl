#include "oled.h"

#include <errno.h>
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


int oled_show(bool connected, bool valid,
              bool fresh, bool output_on)
{

    const char *ble_text =
        connected ? "BLE: CONNECT" : "BLE: WAIT";

    const char *rx_text =
        (!connected || !fresh) ? "RX: --" :
        valid ? "RX: VALID" : "RX: INVALID";

    const char *output_text =
        output_on ? "OUTPUT: ON" : "OUTPUT: OFF";

    int err = cfb_framebuffer_clear(oled, false);
    if (err) return err;

    err = cfb_print(oled, "GESTURECONTROL", 0, 0);
    if (err) return err;

    err = cfb_print(oled, ble_text, 0, 16);
    if (err) return err;

    err = cfb_print(oled, rx_text, 0, 32);
    if (err) return err;

    err = cfb_print(oled, output_text, 0, 48);
    if (err) return err;

    return cfb_framebuffer_finalize(oled);
}
