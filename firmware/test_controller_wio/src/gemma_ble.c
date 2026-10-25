#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include "gemma_ble.h"

LOG_MODULE_REGISTER(gemma_ble, LOG_LEVEL_INF);

#define GEMMA_SERVICE_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abce2)

#define GEMMA_STATUS_UUID_BYTES \
    BT_UUID_128_ENCODE(0x89ca2ebf, 0x7c18, 0x4a33, \
                       0x9877, 0xdf55001abce3)

K_MUTEX_DEFINE(gemma_lock);

static struct bt_conn *host_conn;
static bool enabled;
static bool advertising;
static atomic_t gemma_recycle_pending;

static const uint8_t status_value = 1;

static ssize_t read_status(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    void *buf,
    uint16_t len,
    uint16_t offset)
{
    return bt_gatt_attr_read(
        conn, attr, buf, len, offset,
        &status_value, sizeof(status_value));
}

BT_GATT_SERVICE_DEFINE(gemma_service,
    BT_GATT_PRIMARY_SERVICE(
        BT_UUID_DECLARE_128(GEMMA_SERVICE_UUID_BYTES)),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(GEMMA_STATUS_UUID_BYTES),
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        read_status,
        NULL,
        NULL)
);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(
        BT_DATA_FLAGS,
        BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),

    BT_DATA_BYTES(
        BT_DATA_UUID128_ALL,
        GEMMA_SERVICE_UUID_BYTES),
};

static const struct bt_data sd[] = {
    BT_DATA(
        BT_DATA_NAME_COMPLETE,
        "GestureGemma",
        sizeof("GestureGemma") - 1),
};

static void gemma_connected(
    struct bt_conn *conn,
    uint8_t err)
{
    if (err) {
        return;
    }

    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) != 0 ||
        info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    bool reject = false;

    k_mutex_lock(&gemma_lock, K_FOREVER);

    if (enabled && host_conn == NULL) {
        host_conn = bt_conn_ref(conn);
        advertising = false;
    } else {
        reject = true;
    }

    k_mutex_unlock(&gemma_lock);

    if (reject) {
        bt_conn_disconnect(
            conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    LOG_INF("Gemma laptop connected");
}

static void gemma_disconnected(
    struct bt_conn *conn,
    uint8_t reason)
{
    k_mutex_lock(&gemma_lock, K_FOREVER);

    if (host_conn != conn) {
        k_mutex_unlock(&gemma_lock);
        return;
    }

    atomic_set(&gemma_recycle_pending, 1);

    bt_conn_unref(host_conn);
    host_conn = NULL;

    k_mutex_unlock(&gemma_lock);

    LOG_INF("Gemma laptop disconnected: %u", reason);
}

static void gemma_recycled(void)
{
    atomic_set(&gemma_recycle_pending, 0);
    LOG_DBG("Gemma connection resources released");
}

BT_CONN_CB_DEFINE(gemma_callbacks) = {
    .connected = gemma_connected,
    .disconnected = gemma_disconnected,
    .recycled = gemma_recycled,
};

int gemma_ble_poll(void)
{
    k_mutex_lock(&gemma_lock, K_FOREVER);

    bool should_start =
        enabled &&
        !advertising &&
        host_conn == NULL &&
        atomic_get(&gemma_recycle_pending) == 0;

    if (should_start) {
        advertising = true;
    }

    k_mutex_unlock(&gemma_lock);

    if (!should_start) {
        return 0;
    }

    int err = bt_le_adv_start(
        BT_LE_ADV_CONN_FAST_1,
        ad, ARRAY_SIZE(ad),
        sd, ARRAY_SIZE(sd));

    if (err && err != -EALREADY) {
        k_mutex_lock(&gemma_lock, K_FOREVER);
        advertising = false;
        k_mutex_unlock(&gemma_lock);

        LOG_ERR("Gemma advertising failed: %d", err);
        return err;
    }

    LOG_INF("Advertising as GestureGemma");
    return 0;
}

int gemma_ble_start(void)
{
    k_mutex_lock(&gemma_lock, K_FOREVER);
    enabled = true;
    k_mutex_unlock(&gemma_lock);

    return gemma_ble_poll();
}

int gemma_ble_stop(void)
{
    struct bt_conn *old_conn = NULL;

    k_mutex_lock(&gemma_lock, K_FOREVER);

    enabled = false;

    bool was_advertising = advertising;
    advertising = false;

    if (host_conn != NULL) {
        atomic_set(&gemma_recycle_pending, 1);
    }

    if (host_conn != NULL) {
        old_conn = bt_conn_ref(host_conn);
    }

    k_mutex_unlock(&gemma_lock);

    if (was_advertising) {
        int err = bt_le_adv_stop();

        if (err && err != -EALREADY) {
            k_mutex_lock(&gemma_lock, K_FOREVER);
            advertising = true;
            k_mutex_unlock(&gemma_lock);

            if (old_conn) {
                bt_conn_unref(old_conn);
            }

            return err;
        }
    }

    if (old_conn != NULL) {
        int err = bt_conn_disconnect(
            old_conn,
            BT_HCI_ERR_REMOTE_USER_TERM_CONN);

        bt_conn_unref(old_conn);

        if (err &&
            err != -ENOTCONN &&
            err != -EALREADY) {
            return err;
        }
    }

    LOG_INF("Gemma BLE stopped");
    return 0;
}

bool gemma_ble_idle(void)
{
    k_mutex_lock(&gemma_lock, K_FOREVER);

    bool idle =
        host_conn == NULL &&
        !advertising &&
        atomic_get(&gemma_recycle_pending) == 0;

    k_mutex_unlock(&gemma_lock);

    return idle;
}