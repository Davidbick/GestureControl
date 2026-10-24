#include "emg.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(emg, LOG_LEVEL_INF);

#define EMG_NODE DT_PATH(zephyr_user)

static const struct adc_dt_spec emg_adc[EMG_CHANNEL_COUNT] = {
    ADC_DT_SPEC_GET_BY_NAME(EMG_NODE, emg0),
    ADC_DT_SPEC_GET_BY_NAME(EMG_NODE, emg1),
};

int emg_init(void)
{
    // Both channels must use the same ADC peripheral.
    if (emg_adc[0].dev != emg_adc[1].dev) {
        LOG_ERR("EMG channels use different ADC devices");
        return -EINVAL;
    }

    for (int i = 0; i < EMG_CHANNEL_COUNT; i++) {

        if (!adc_is_ready_dt(&emg_adc[i])) {
            LOG_ERR("EMG%d ADC device not ready", i);
            return -ENODEV;
        }

        int ret = adc_channel_setup_dt(&emg_adc[i]);

        if (ret < 0) {
            LOG_ERR("EMG%d ADC setup failed: %d", i, ret);
            return ret;
        }
    }

    LOG_INF("Both EMG ADC channels initialized");

    return 0;
}

int emg_read(struct emg_sample *sample)
{
    if (sample == NULL) {
        return -EINVAL;
    }

    int16_t raw[EMG_CHANNEL_COUNT] = {0};

    // Read both ADC channels in one sampling sequence.
    struct adc_sequence sequence = {
        .channels = BIT(emg_adc[0].channel_id) |
                    BIT(emg_adc[1].channel_id),
        .buffer = raw,
        .buffer_size = sizeof(raw),
        .resolution = emg_adc[0].resolution,
    };

    int ret = adc_read(emg_adc[0].dev, &sequence);

    if (ret < 0) {
        return ret;
    }

    // Convert both ADC readings to millivolts.
    for (int i = 0; i < EMG_CHANNEL_COUNT; i++) {

        sample->raw[i] = raw[i];

        int32_t mv = raw[i];

        ret = adc_raw_to_millivolts_dt(
            &emg_adc[i],
            &mv);

        if (ret < 0) {
            return ret;
        }

        sample->mv[i] = mv;
    }

    return 0;
}
