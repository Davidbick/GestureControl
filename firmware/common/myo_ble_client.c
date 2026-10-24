#include "myo_ble_client.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

LOG_MODULE_REGISTER(myo_ble_client, LOG_LEVEL_INF);

static struct bt_conn *peer_conn;

static uint16_t state_handle;
static uint16_t gesture_handle;
static uint8_t next_sequence;

K_MUTEX_DEFINE(ble_lock);

static struct bt_uuid_128 state_uuid =
    BT_UUID_INIT_128(MYO_STATE_UUID_BYTES);

static struct bt_uuid_128 gesture_uuid =
    BT_UUID_INIT_128(MYO_GESTURE_UUID_BYTES);

static struct bt_gatt_discover_params discover_params;

static void start_scan(void);

// GATT discovery

static uint8_t discover_func(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params)
{
    ARG_UNUSED(params);

    if (!attr) {
        uint16_t state;
        uint16_t gesture;

        k_mutex_lock(&ble_lock, K_FOREVER);
        state = state_handle;
        gesture = gesture_handle;
        k_mutex_unlock(&ble_lock);

        LOG_INF("GATT complete: state=%u gesture=%u",
                state, gesture);

        if (state == 0 && gesture == 0) {
            LOG_WRN("No GestureControl characteristics");
            bt_conn_disconnect(
                conn,
                BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }

        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_chrc *chrc = attr->user_data;

    if (!chrc) {
        return BT_GATT_ITER_CONTINUE;
    }

    k_mutex_lock(&ble_lock, K_FOREVER);

    if (bt_uuid_cmp(chrc->uuid, &state_uuid.uuid) == 0) {
        state_handle = chrc->value_handle;

        LOG_INF("Legacy state handle=%u", state_handle);
    }

    if (bt_uuid_cmp(chrc->uuid, &gesture_uuid.uuid) == 0) {
        gesture_handle = chrc->value_handle;

        LOG_INF("Gesture handle=%u", gesture_handle);
    }

    k_mutex_unlock(&ble_lock);

    return BT_GATT_ITER_CONTINUE;
}

// Advertisement matching
static bool match_service(
    struct bt_data *data,
    void *user_data)
{
    bool *found = user_data;

    static const uint8_t expected[] = {
        MYO_SERVICE_UUID_BYTES
    };

    if (data->type != BT_DATA_UUID128_ALL &&
        data->type != BT_DATA_UUID128_SOME) {
        return true;
    }

    for (size_t i = 0;
         i + sizeof(expected) <= data->data_len;
         i += sizeof(expected)) {

        if (memcmp(data->data + i,
                   expected,
                   sizeof(expected)) == 0) {
            *found = true;
            return false;
        }
    }

    return true;
}

// Scanning and connection
static void device_found(
    const bt_addr_le_t *addr,
    int8_t rssi,
    uint8_t type,
    struct net_buf_simple *ad)
{
    ARG_UNUSED(rssi);

    if (type != BT_GAP_ADV_TYPE_ADV_IND) {
        return;
    }

    k_mutex_lock(&ble_lock, K_FOREVER);
    bool busy = peer_conn != NULL;
    k_mutex_unlock(&ble_lock);

    if (busy) {
        return;
    }

    bool found = false;
    bt_data_parse(ad, match_service, &found);

    if (!found) {
        return;
    }

    if (bt_le_scan_stop() != 0) {
        return;
    }

    struct bt_conn *conn = NULL;

    int err = bt_conn_le_create(
        addr,
        BT_CONN_LE_CREATE_CONN,
        BT_LE_CONN_PARAM_DEFAULT,
        &conn);

    if (err) {
        LOG_ERR("Connection request failed: %d", err);
        start_scan();
        return;
    }

    k_mutex_lock(&ble_lock, K_FOREVER);
    peer_conn = conn;
    k_mutex_unlock(&ble_lock);

    LOG_INF("Connecting to GestureControl receiver");
}

static void start_scan(void)
{
    int err = bt_le_scan_start(
        BT_LE_SCAN_PASSIVE,
        device_found);

    if (err == -EALREADY) {
        return;
    }

    if (err) {
        LOG_ERR("Scan failed: %d", err);
    } else {
        LOG_INF("Scanning for receiver");
    }
}

// Connection callbacks
static void connected(
    struct bt_conn *conn,
    uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connection failed: %u", err);

        k_mutex_lock(&ble_lock, K_FOREVER);

        if (peer_conn == conn) {
            bt_conn_unref(peer_conn);
            peer_conn = NULL;
        }

        state_handle = 0;
        gesture_handle = 0;

        k_mutex_unlock(&ble_lock);

        start_scan();
        return;
    }

    LOG_INF("BLE connected");

    k_mutex_lock(&ble_lock, K_FOREVER);

    state_handle = 0;
    gesture_handle = 0;
    next_sequence = 0;

    k_mutex_unlock(&ble_lock);

    memset(&discover_params, 0,
           sizeof(discover_params));

    // Discover all characteristics, then match
    // our two UUIDs inside discover_func().
    discover_params.uuid = NULL;
    discover_params.func = discover_func;
    discover_params.start_handle = 0x0001;
    discover_params.end_handle = 0xffff;
    discover_params.type =
        BT_GATT_DISCOVER_CHARACTERISTIC;

    int ret = bt_gatt_discover(conn, &discover_params);

    if (ret) {
        LOG_ERR("GATT discovery failed: %d", ret);
        bt_conn_disconnect(
            conn,
            BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void disconnected(
    struct bt_conn *conn,
    uint8_t reason)
{
    LOG_INF("BLE disconnected: %u", reason);

    k_mutex_lock(&ble_lock, K_FOREVER);

    state_handle = 0;
    gesture_handle = 0;

    if (peer_conn == conn) {
        bt_conn_unref(peer_conn);
        peer_conn = NULL;
    }

    k_mutex_unlock(&ble_lock);

    start_scan();
}

BT_CONN_CB_DEFINE(myo_conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

// Public initialization
int myo_ble_client_init(void)
{
    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return err;
    }

    LOG_INF("Bluetooth initialized");

    start_scan();

    return 0;
}

// Connection lookup helper
static int get_write_target(
    bool gesture,
    struct bt_conn **conn,
    uint16_t *handle)
{
    int ret = -ENOTCONN;

    *conn = NULL;
    *handle = 0;

    k_mutex_lock(&ble_lock, K_FOREVER);

    if (peer_conn != NULL) {
        uint16_t selected =
            gesture ? gesture_handle : state_handle;

        if (selected != 0) {
            *conn = bt_conn_ref(peer_conn);
            *handle = selected;
            ret = 0;
        } else {
            ret = gesture ? -ENOTSUP : -ENOTCONN;
        }
    }

    k_mutex_unlock(&ble_lock);

    return ret;
}

// Legacy validity transmission
int myo_ble_client_send_state(uint8_t state)
{
    struct bt_conn *conn;
    uint16_t handle;

    int err = get_write_target(
        false, &conn, &handle);

    if (err) {
        return err;
    }

    int ret = bt_gatt_write_without_response(
        conn,
        handle,
        &state,
        sizeof(state),
        false);

    bt_conn_unref(conn);

    return ret;
}

// New gesture transmission
int myo_ble_client_send_gesture(enum myo_gesture gesture)
{
    if (!myo_gesture_valid((uint8_t)gesture)) {
        return -EINVAL;
    }

    struct bt_conn *conn;
    uint16_t handle;

    int err = get_write_target(
        true, &conn, &handle);

    if (err) {
        return err;
    }

    struct myo_gesture_packet packet;

    packet.gesture = (uint8_t)gesture;

    k_mutex_lock(&ble_lock, K_FOREVER);
    packet.sequence = next_sequence++;
    k_mutex_unlock(&ble_lock);

    int ret = bt_gatt_write_without_response(
        conn,
        handle,
        &packet,
        sizeof(packet),
        false);

    bt_conn_unref(conn);

    return ret;
}

bool myo_ble_client_gesture_ready(void)
{
    k_mutex_lock(&ble_lock, K_FOREVER);

    bool ready =
        peer_conn != NULL && gesture_handle != 0;

    k_mutex_unlock(&ble_lock);

    return ready;
}