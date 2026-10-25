
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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
#include "oled.h"

LOG_MODULE_REGISTER(receiver, LOG_LEVEL_INF);

#define LED_NODE DT_ALIAS(led0)
#define RX_TIMEOUT_MS 1500U
#define RGB_LED_PIN 29
#define TENSE_GUARD_MS 250U

static const struct device *rgb_gpio =
    DEVICE_DT_GET(DT_NODELABEL(gpio0));

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

// Shared BLE connection state.
static atomic_t link_up;
static atomic_t rx_valid;
static atomic_t last_rx_ms;
static atomic_t session_epoch;

// Actual light state, independent of orientation.
static atomic_t light_on;

// Queue BLE packets for the main thread.
struct queued_gesture {
    struct myo_gesture_packet packet;
    uint32_t session;
    uint32_t timestamp_ms;
};

K_MSGQ_DEFINE(
    gesture_queue,
    sizeof(struct queued_gesture),
    8,
    4
);

// Main-thread gesture tracking.
static uint32_t processed_session = UINT32_MAX;
static bool sequence_seen;
static uint8_t last_sequence;

static bool tense_seen;
static uint32_t last_tense_ms;

static const char *gesture_name(uint8_t gesture)
{
    switch (gesture) {
    case MYO_GESTURE_SWIPE_UP:
        return "SWIPE_UP";
    case MYO_GESTURE_SWIPE_DOWN:
        return "SWIPE_DOWN";
    case MYO_GESTURE_SWIPE_LEFT:
        return "SWIPE_LEFT";
    case MYO_GESTURE_SWIPE_RIGHT:
        return "SWIPE_RIGHT";
    case MYO_GESTURE_TENSE:
        return "TENSE";
    default:
        return "UNKNOWN";
    }
}

// Existing legacy validity characteristic.
// This is now diagnostic only.
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

// NEW: Receive two-byte gesture packets.
static ssize_t write_gesture(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    const void *buf,
    uint16_t len,
    uint16_t offset,
    uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    if (offset != 0 ||
        len != sizeof(struct myo_gesture_packet)) {
        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        return 0;
    }

    struct queued_gesture event = {0};

    memcpy(&event.packet, buf, sizeof(event.packet));

    if (!myo_gesture_valid(event.packet.gesture)) {
        LOG_WRN("Invalid gesture ID: %u",
                event.packet.gesture);

        return BT_GATT_ERR(
            BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    event.session =
        (uint32_t)atomic_get(&session_epoch);

    event.timestamp_ms = k_uptime_get_32();

    if (k_msgq_put(
            &gesture_queue,
            &event,
            K_NO_WAIT) != 0) {

        LOG_WRN("Gesture queue full");

        return BT_GATT_ERR(
            BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

    return len;
}

// Both existing state and new gesture characteristics.
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
        NULL),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(MYO_GESTURE_UUID_BYTES),
        BT_GATT_CHRC_WRITE |
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL,
        write_gesture,
        NULL)
);

// Existing BLE advertising.
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

    // Begin a new command session.
    atomic_inc(&session_epoch);
    k_msgq_purge(&gesture_queue);

    atomic_set(&light_on, 0);
    atomic_set(&rx_valid, 0);
    atomic_set(&link_up, 1);

    LOG_INF("Wearable connected");
}

static void disconnected(
    struct bt_conn *conn,
    uint8_t reason)
{
    ARG_UNUSED(conn);

    atomic_set(&link_up, 0);
    atomic_set(&rx_valid, 0);
    atomic_set(&light_on, 0);

    // Invalidate packets from previous connection.
    atomic_inc(&session_epoch);
    k_msgq_purge(&gesture_queue);

    LOG_INF("Disconnected: %u", reason);

    start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

// Process received gestures outside BLE callbacks.
static void process_gesture(
    const struct queued_gesture *event)
{
    if (!atomic_get(&link_up) ||
        event->session !=
            (uint32_t)atomic_get(&session_epoch)) {
        return;
    }

    // Reset duplicate detection for new sessions.
    if (event->session != processed_session) {
        processed_session = event->session;
        sequence_seen = false;
        tense_seen = false;
    }

    uint8_t gesture = event->packet.gesture;
    uint8_t sequence = event->packet.sequence;

    if (sequence_seen && sequence == last_sequence) {
        LOG_WRN("Duplicate gesture: seq=%u", sequence);
        return;
    }

    sequence_seen = true;
    last_sequence = sequence;

    LOG_INF("BLE RX: %s id=%u seq=%u",
            gesture_name(gesture),
            gesture,
            sequence);

    switch (gesture) {
    case MYO_GESTURE_TENSE: {

        // Extra protection against repeated toggles.
        if (tense_seen &&
            (uint32_t)(event->timestamp_ms -
                       last_tense_ms) < TENSE_GUARD_MS) {

            LOG_INF("TENSE ignored: cooldown");
            return;
        }

        tense_seen = true;
        last_tense_ms = event->timestamp_ms;

        bool next = !atomic_get(&light_on);
        atomic_set(&light_on, next);

        LOG_INF("LIGHT: %s", next ? "ON" : "OFF");
        break;
    }

    case MYO_GESTURE_SWIPE_UP:
    case MYO_GESTURE_SWIPE_DOWN:
    case MYO_GESTURE_SWIPE_LEFT:
    case MYO_GESTURE_SWIPE_RIGHT:
        // Recognized and logged.
        // Room-specific actions can be mapped later.
        break;

    default:
        break;
    }
}

int main(void)
{
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED not ready");
        return 0;
    }

    gpio_pin_configure_dt(
        &led, GPIO_OUTPUT_INACTIVE);

    if (!device_is_ready(rgb_gpio)) {
        LOG_ERR("RGB GPIO not ready");
        return 0;
    }

    // Active LOW: HIGH = OFF, LOW = ON.
    int rgb_err = gpio_pin_configure(
        rgb_gpio, RGB_LED_PIN, GPIO_OUTPUT_HIGH);

    if (rgb_err) {
        LOG_ERR("RGB GPIO configuration failed: %d",
                rgb_err);
        return 0;
    }

    LOG_INF("GestureControl receiver starting");

    int oled_err = oled_init();

    if (oled_err) {
        LOG_WRN("OLED initialization failed: %d", oled_err);
    } else {
        oled_show(false, false, false, false);
    }

    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return 0;
    }

    start_advertising();

    bool first_render = true;
    bool previous_link = false;
    bool previous_valid = false;
    bool previous_fresh = false;
    bool previous_output = false;

    while (1) {
        // Process all pending gesture commands.
        struct queued_gesture event;

        while (k_msgq_get(
                &gesture_queue,
                &event,
                K_NO_WAIT) == 0) {

            process_gesture(&event);
        }

        uint32_t now = k_uptime_get_32();

        bool fresh =
            (uint32_t)(now -
                (uint32_t)atomic_get(&last_rx_ms))
            < RX_TIMEOUT_MS;

        bool linked = atomic_get(&link_up) != 0;
        bool valid = atomic_get(&rx_valid) != 0;

        // Actual light is controlled by TENSE,
        // not by the validity heartbeat.
        bool output_on =
            linked && (atomic_get(&light_on) != 0);

        gpio_pin_set_dt(&led, output_on ? 1 : 0);

        // External RGB LED, active LOW.
        gpio_pin_set(
            rgb_gpio,
            RGB_LED_PIN,
            output_on ? 0 : 1);

        // Redraw OLED only when visible data changes.
        if (!oled_err &&
            (first_render ||
             linked != previous_link ||
             valid != previous_valid ||
             fresh != previous_fresh ||
             output_on != previous_output)) {

            int ret = oled_show(
                linked, valid, fresh, output_on);

            if (ret) {
                LOG_WRN("OLED update failed: %d", ret);
                oled_err = ret;
            }

            previous_link = linked;
            previous_valid = valid;
            previous_fresh = fresh;
            previous_output = output_on;
            first_render = false;
        }

        k_msleep(50);
    }

    return 0;
}
