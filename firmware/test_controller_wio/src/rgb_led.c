#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "rgb_led.h"

LOG_MODULE_REGISTER(wio_rgb, LOG_LEVEL_INF);

static const struct pwm_dt_spec pwm_channels[WIO_RGB_CHANNEL_COUNT] = {
    [WIO_RGB_PIN_02] = PWM_DT_SPEC_GET(DT_NODELABEL(wio_pwm_02)),
    [WIO_RGB_PIN_09] = PWM_DT_SPEC_GET(DT_NODELABEL(wio_pwm_09)),
    [WIO_RGB_PIN_29] = PWM_DT_SPEC_GET(DT_NODELABEL(wio_pwm_29)),
};

static const unsigned pin_numbers[WIO_RGB_CHANNEL_COUNT] = {2U, 9U, 29U};

int wio_rgb_set_channel(enum wio_rgb_channel channel, uint8_t percent)
{
    if ((unsigned)channel >= WIO_RGB_CHANNEL_COUNT || percent > 100U) {
        return -EINVAL;
    }

    const struct pwm_dt_spec *spec = &pwm_channels[channel];
    uint32_t pulse_ns = (uint32_t)(((uint64_t)spec->period * percent) / 100U);

    /* PWM_POLARITY_INVERTED: 0%=high/OFF, 100%=low/ON. */
    return pwm_set_dt(spec, spec->period, pulse_ns);
}

int wio_rgb_all_off(void)
{
    int first_error = 0;

    for (unsigned i = 0; i < WIO_RGB_CHANNEL_COUNT; ++i) {
        int err = wio_rgb_set_channel((enum wio_rgb_channel)i, 0U);

        if (err && !first_error) {
            first_error = err;
        }
    }

    return first_error;
}

int wio_rgb_init(void)
{
    for (unsigned i = 0; i < WIO_RGB_CHANNEL_COUNT; ++i) {
        if (!pwm_is_ready_dt(&pwm_channels[i])) {
            LOG_ERR("PWM for P0.%02u not ready", pin_numbers[i]);
            return -ENODEV;
        }
    }

    return wio_rgb_all_off();
}