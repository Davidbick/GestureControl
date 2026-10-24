#include "emg.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(emg, LOG_LEVEL_INF);

#define EMG_NODE DT_PATH(zephyr_user)

static const struct adc_dt_spec emg_adc =
    ADC_DT_SPEC_GET_BY_NAME(EMG_NODE, emg0);

int emg_init(void)
{
    if (!adc_is_ready_dt(&emg_adc)) {
        LOG_ERR("ADC device is not ready");
        return -ENODEV;
    }

    int ret = adc_channel_setup_dt(&emg_adc);

    if (ret < 0) {
        LOG_ERR("ADC channel setup failed: %d", ret);
        return ret;
    }

    LOG_INF("EMG ADC initialized");

    return 0;
}

int emg_read(struct emg_sample *sample)
{
    int16_t raw = 0;

    struct adc_sequence sequence = {
        .buffer = &raw,
        .buffer_size = sizeof(raw),
    };

    int ret = adc_sequence_init_dt(
        &emg_adc,
        &sequence);

    if (ret < 0) {
        return ret;
    }

    ret = adc_read_dt(
        &emg_adc,
        &sequence);

    if (ret < 0) {
        return ret;
    }

    sample->raw = raw;

    int32_t mv = raw;

    ret = adc_raw_to_millivolts_dt(
        &emg_adc,
        &mv);

    if (ret < 0) {
        return ret;
    }

    sample->mv = mv;

    return 0;
}
