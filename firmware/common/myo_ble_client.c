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
static enum myo_receiver_mode current_mode = MYO_MODE_LIGHT;
static bool central_enabled = true;
static bool scan_running;
static bool connecting;

static uint16_t state_handle;
static uint16_t gesture_handle;
static uint16_t brightness_handle;
static uint8_t next_sequence;

K_MUTEX_DEFINE(ble_lock);

static struct bt_uuid_128 state_uuid =
    BT_UUID_INIT_128(MYO_STATE_UUID_BYTES);

static struct bt_uuid_128 gesture_uuid =
    BT_UUID_INIT_128(MYO_GESTURE_UUID_BYTES);

static struct bt_uuid_128 brightness_uuid =
    BT_UUID_INIT_128(MYO_BRIGHTNESS_UUID_BYTES);

static struct bt_gatt_discover_params discover_params;

static int start_scan(void);

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
        uint16_t brightness;

        k_mutex_lock(&ble_lock, K_FOREVER);
        state = state_handle;
        gesture = gesture_handle;
        brightness = brightness_handle;
        k_mutex_unlock(&ble_lock);

        LOG_INF("GATT complete: state=%u gesture=%u brightness=%u",
        state, gesture, brightness);

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

    
    if (bt_uuid_cmp(chrc->uuid, &brightness_uuid.uuid) == 0) {
        brightness_handle = chrc->value_handle;
        LOG_INF("Brightness handle=%u", brightness_handle);
    }


    k_mutex_unlock(&ble_lock);

    return BT_GATT_ITER_CONTINUE;
}

// Advertisement matching
struct advertisement_match {
    bool service_found;
    bool name_found;
    const char *target_name;
};

static bool match_target_ad(
    struct bt_data *data,
    void *user_data)
{
    struct advertisement_match *match = user_data;

    static const uint8_t expected_service[] = {
        MYO_SERVICE_UUID_BYTES
    };

    if (data->type == BT_DATA_UUID128_ALL ||
        data->type == BT_DATA_UUID128_SOME) {
        for (size_t i = 0;
             i + sizeof(expected_service) <= data->data_len;
             i += sizeof(expected_service)) {

            if (memcmp(data->data + i, expected_service,
                       sizeof(expected_service)) == 0) {
                match->service_found = true;
                break;
            }
        }
    }

    if (data->type == BT_DATA_NAME_COMPLETE) {
        size_t expected_length = strlen(match->target_name);
        if (data->data_len == expected_length &&
            memcmp(data->data, match->target_name,
                   expected_length) == 0) {
            match->name_found = true;
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

    bool busy =
        !central_enabled ||
        peer_conn != NULL ||
        connecting;

    enum myo_receiver_mode requested = current_mode;

    k_mutex_unlock(&ble_lock);

    if (busy) {
        return;
    }

    const char *target_name =
        requested == MYO_MODE_LIGHT
            ? "MyoLight" : "MyoPC";

    struct advertisement_match match = {
        .service_found = false,
        .name_found = false,
        .target_name = target_name
    };

    bt_data_parse(ad, match_target_ad, &match);

    if (!match.service_found || !match.name_found) {
        return;
    }

    if (bt_le_scan_stop() != 0) {
        return;
    }

    k_mutex_lock(&ble_lock, K_FOREVER);

    scan_running = false;

    bool allowed =
        central_enabled &&
        peer_conn == NULL &&
        !connecting &&
        current_mode == requested;

    if (allowed) {
        connecting = true;
    }

    k_mutex_unlock(&ble_lock);

    if (!allowed) {
        start_scan();
        return;
    }

    struct bt_conn *conn = NULL;

    int err = bt_conn_le_create(
        addr,
        BT_CONN_LE_CREATE_CONN,
        BT_LE_CONN_PARAM_DEFAULT,
        &conn);

    k_mutex_lock(&ble_lock, K_FOREVER);

    connecting = false;

    if (!err) {
        peer_conn = conn;
    }

    bool still_allowed =
        central_enabled &&
        current_mode == requested;

    k_mutex_unlock(&ble_lock);

    if (err) {
        LOG_ERR("Connection request failed: %d", err);
        start_scan();
        return;
    }

    if (!still_allowed) {
        bt_conn_disconnect(
            conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    LOG_INF("Connecting to %s", target_name);
}

static int start_scan(void)
{
    k_mutex_lock(&ble_lock, K_FOREVER);

    bool allowed =
        central_enabled &&
        peer_conn == NULL &&
        !connecting &&
        !scan_running;

    if (allowed) {
        scan_running = true;
    }

    k_mutex_unlock(&ble_lock);

    if (!allowed) {
        return 0;
    }

    int err = bt_le_scan_start(
        BT_LE_SCAN_PASSIVE,
        device_found);

    if (err && err != -EALREADY) {
        k_mutex_lock(&ble_lock, K_FOREVER);
        scan_running = false;
        k_mutex_unlock(&ble_lock);

        LOG_ERR("Scan failed: %d", err);
        return err;
    }

    k_mutex_lock(&ble_lock, K_FOREVER);

    bool must_stop = !central_enabled;

    k_mutex_unlock(&ble_lock);

    if (must_stop) {
        err = bt_le_scan_stop();

        if (err && err != -EALREADY) {
            return err;
        }

        k_mutex_lock(&ble_lock, K_FOREVER);
        scan_running = false;
        k_mutex_unlock(&ble_lock);

        return 0;
    }

    LOG_INF("Scanning for receiver");
    return 0;
}

// Connection callbacks
static void connected(
    struct bt_conn *conn,
    uint8_t err)
{
    k_mutex_lock(&ble_lock, K_FOREVER);

    bool ours = peer_conn == conn;
    bool active = central_enabled;

    k_mutex_unlock(&ble_lock);

    // Ignore inbound Gemma peripheral connections.
    if (!ours) {
        return;
    }

    // A mode switch might have happened during connect.
    if (!err && !active) {
        bt_conn_disconnect(
            conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    if (err) {
        LOG_ERR("BLE connection failed: %u", err);

        k_mutex_lock(&ble_lock, K_FOREVER);

        if (peer_conn == conn) {
            bt_conn_unref(peer_conn);
            peer_conn = NULL;
        }

        state_handle = 0;
        gesture_handle = 0;
        brightness_handle = 0;

        k_mutex_unlock(&ble_lock);

        start_scan();
        return;
    }

    LOG_INF("BLE connected");

    k_mutex_lock(&ble_lock, K_FOREVER);

    state_handle = 0;
    gesture_handle = 0;
    brightness_handle = 0;
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
    k_mutex_lock(&ble_lock, K_FOREVER);

    bool ours = peer_conn == conn;

    k_mutex_unlock(&ble_lock);

    if (!ours) {
        return;
    }

    LOG_INF("BLE disconnected: %u", reason);

    k_mutex_lock(&ble_lock, K_FOREVER);

    state_handle = 0;
    gesture_handle = 0;
    brightness_handle = 0;

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

int myo_ble_client_init_mode(enum myo_receiver_mode mode)
{
    if (mode != MYO_MODE_LIGHT &&
        mode != MYO_MODE_COMPUTER) {
        return -EINVAL;
    }

    k_mutex_lock(&ble_lock, K_FOREVER);

    current_mode = mode;
    central_enabled = true;
    scan_running = false;
    connecting = false;

    k_mutex_unlock(&ble_lock);

    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return err;
    }

    LOG_INF("Bluetooth initialized");

    return start_scan();
}

// Existing wearable behavior remains LIGHT by default.
int myo_ble_client_init(void)
{
    return myo_ble_client_init_mode(MYO_MODE_LIGHT);
}

enum myo_write_target {
    MYO_WRITE_STATE,
    MYO_WRITE_GESTURE,
    MYO_WRITE_BRIGHTNESS
};

static int get_write_target(
    enum myo_write_target target,
    struct bt_conn **conn,
    uint16_t *handle)
{
    int ret = -ENOTCONN;

    *conn = NULL;
    *handle = 0;

    k_mutex_lock(&ble_lock, K_FOREVER);

    if (peer_conn != NULL) {
        uint16_t selected = 0;

        switch (target) {
        case MYO_WRITE_STATE:
            selected = state_handle;
            break;

        case MYO_WRITE_GESTURE:
            selected = gesture_handle;
            break;

        case MYO_WRITE_BRIGHTNESS:
            selected = brightness_handle;
            break;
        }

        if (selected != 0) {
            *conn = bt_conn_ref(peer_conn);
            *handle = selected;
            ret = 0;
        } else {
            ret = -ENOTSUP;
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
        MYO_WRITE_STATE, &conn, &handle);

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
        MYO_WRITE_GESTURE, &conn, &handle);

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

int myo_ble_client_send_brightness(uint8_t percent)
{
    if (!myo_brightness_valid(percent)) {
        return -EINVAL;
    }

    struct bt_conn *conn;
    uint16_t handle;

    int err = get_write_target(
        MYO_WRITE_BRIGHTNESS,
        &conn,
        &handle
    );

    if (err) {
        return err;
    }

    struct myo_brightness_packet packet;

    packet.percent = percent;

    // Shared counter with gesture packets.
    k_mutex_lock(&ble_lock, K_FOREVER);
    packet.sequence = next_sequence++;
    k_mutex_unlock(&ble_lock);

    int ret = bt_gatt_write_without_response(
        conn,
        handle,
        &packet,
        sizeof(packet),
        false
    );

    bt_conn_unref(conn);

    return ret;
}

bool myo_ble_client_brightness_ready(void)
{
    k_mutex_lock(&ble_lock, K_FOREVER);

    bool ready =
        peer_conn != NULL &&
        brightness_handle != 0;

    k_mutex_unlock(&ble_lock);

    return ready;
}

enum myo_receiver_mode myo_ble_client_get_mode(void)
{
    k_mutex_lock(&ble_lock, K_FOREVER);
    enum myo_receiver_mode mode = current_mode;
    k_mutex_unlock(&ble_lock);
    return mode;
}

int myo_ble_client_toggle_mode(void)
{
    struct bt_conn *old_conn = NULL;

    k_mutex_lock(&ble_lock, K_FOREVER);

    current_mode = current_mode == MYO_MODE_LIGHT
        ? MYO_MODE_COMPUTER : MYO_MODE_LIGHT;

    enum myo_receiver_mode selected = current_mode;

    if (peer_conn != NULL) {
        old_conn = bt_conn_ref(peer_conn);
        // Block commands from reaching the old receiver.
        state_handle = 0;
        gesture_handle = 0;
        brightness_handle = 0;
    }

    k_mutex_unlock(&ble_lock);

    LOG_INF("MODE SWITCH: %s",
            selected == MYO_MODE_LIGHT ? "LIGHT" : "COMPUTER");

    if (old_conn != NULL) {
        // disconnected() restarts the scan automatically.
        int err = bt_conn_disconnect(
            old_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(old_conn);

        if (err && err != -ENOTCONN) {
            LOG_ERR("Mode disconnect failed: %d", err);
            return err;
        }
    } else {
        // If already scanning, start_scan() ignores -EALREADY.
        start_scan();
    }

    return 0;
}

int myo_ble_client_pause(void)
{
    struct bt_conn *old_conn = NULL;

    k_mutex_lock(&ble_lock, K_FOREVER);

    central_enabled = false;

    bool stop_scan = scan_running;

    // Prevent further gesture writes during handoff.
    state_handle = 0;
    gesture_handle = 0;
    brightness_handle = 0;

    if (peer_conn != NULL) {
        old_conn = bt_conn_ref(peer_conn);
    }

    k_mutex_unlock(&ble_lock);

    if (stop_scan) {
        int err = bt_le_scan_stop();

        if (err && err != -EALREADY) {
            if (old_conn) {
                bt_conn_unref(old_conn);
            }
            return err;
        }

        k_mutex_lock(&ble_lock, K_FOREVER);
        scan_running = false;
        k_mutex_unlock(&ble_lock);
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

    LOG_INF("Central BLE paused");
    return 0;
}

bool myo_ble_client_idle(void)
{
    k_mutex_lock(&ble_lock, K_FOREVER);

    bool idle =
        !scan_running &&
        !connecting &&
        peer_conn == NULL;

    k_mutex_unlock(&ble_lock);

    return idle;
}

int myo_ble_client_resume_computer(void)
{
    k_mutex_lock(&ble_lock, K_FOREVER);

    if (peer_conn != NULL || connecting) {
        k_mutex_unlock(&ble_lock);
        return -EBUSY;
    }

    current_mode = MYO_MODE_COMPUTER;
    central_enabled = true;

    k_mutex_unlock(&ble_lock);

    LOG_INF("Resuming MyoPC connection");

    return start_scan();
}