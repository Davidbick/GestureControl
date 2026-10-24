#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "myo_ble.h"

LOG_MODULE_REGISTER(receiver, LOG_LEVEL_INF);

#define LED_NODE DT_ALIAS(led0)
#define RX_TIMEOUT_MS 1500

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

static atomic_t link_up;
static atomic_t rx_valid;
static atomic_t last_rx_ms;

// Called whenever the wearable writes a classification
static ssize_t write_state(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    const void *buf,
    uint16_t len,
    uint16_t offset,
    uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    if (offset != 0 || len != 1) {
        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        return 0;
    }

    uint8_t state = ((const uint8_t *)buf)[0];

    if (state != MYO_VALID && state != MYO_INVALID) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    atomic_set(&rx_valid, state == MYO_VALID);
    atomic_set(&last_rx_ms, k_uptime_get_32());

    LOG_INF("Received: %s",
            state == MYO_VALID ? "VALID" : "INVALID");

    return len;
}

// Custom GestureControl GATT service
BT_GATT_SERVICE_DEFINE(myo_service,
    BT_GATT_PRIMARY_SERVICE(
        BT_UUID_DECLARE_128(MYO_SERVICE_UUID_BYTES)),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(MYO_STATE_UUID_BYTES),
        BT_GATT_CHRC_WRITE |
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL,
        write_state,
        NULL)
);

// Put UUID in advertisements so Sense can find this
static const struct bt_data ad[] = {
    BT_DATA_BYTES(
        BT_DATA_FLAGS,
        BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),

    BT_DATA_BYTES(
        BT_DATA_UUID128_ALL,
        MYO_SERVICE_UUID_BYTES),

    BT_DATA_BYTES(
        BT_DATA_NAME_COMPLETE,
        'M', 'y', 'o', 'R', 'x'),
};

static void start_advertising(void)
{
    int err = bt_le_adv_start(
        BT_LE_ADV_CONN_FAST_1,
        ad, ARRAY_SIZE(ad),
        NULL, 0);

    if (err) {
        LOG_ERR("Advertising failed: %d", err);
    } else {
        LOG_INF("Advertising as MyoRx");
    }
}

static void connected(
    struct bt_conn *conn,
    uint8_t err)
{
    ARG_UNUSED(conn);

    if (err) {
        LOG_ERR("Connection failed: %u", err);
        return;
    }

    atomic_set(&link_up, 1);
    atomic_set(&rx_valid, 0);

    LOG_INF("Wearable connected");
}

static void disconnected(
    struct bt_conn *conn,
    uint8_t reason)
{
    ARG_UNUSED(conn);

    atomic_set(&link_up, 0);
    atomic_set(&rx_valid, 0);

    LOG_INF("Disconnected: %u", reason);

    start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

int main(void)
{
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED not ready");
        return 0;
    }

    gpio_pin_configure_dt(
        &led, GPIO_OUTPUT_INACTIVE);

    LOG_INF("GestureControl receiver starting");

    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return 0;
    }

    start_advertising();

    while (1) {
        uint32_t now = k_uptime_get_32();

        bool fresh =
            (uint32_t)(now -
                (uint32_t)atomic_get(&last_rx_ms))
            < RX_TIMEOUT_MS;

        bool led_on =
            atomic_get(&link_up) &&
            atomic_get(&rx_valid) &&
            fresh;

        gpio_pin_set_dt(&led, led_on ? 1 : 0);

        k_msleep(50);
    }

    return 0;
}
