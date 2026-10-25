#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <string.h>
#include "myo_ble.h"
#include "usb_hid.h"


LOG_MODULE_REGISTER(receiver, LOG_LEVEL_INF);

#define LED_NODE DT_ALIAS(led0)
#define RX_TIMEOUT_MS 1500
#define TENSE_GUARD_MS 250

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

    // Legacy: LED reflects orientation validity.
// Gesture: LED is controlled by gesture events.
enum led_control_mode {
    LED_MODE_LEGACY,
    LED_MODE_GESTURE
};

static atomic_t link_up;
static atomic_t rx_valid;
static atomic_t last_rx_ms;
static atomic_t led_mode;
static atomic_t session_epoch;

// A small event queue separates BLE callbacks
// from physical actuation.
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

// Actuation state, owned by main thread.
static bool gesture_led_on;
static bool sequence_seen;
static uint8_t last_sequence;
static uint32_t processed_session = UINT32_MAX;

static bool tense_seen;
static uint32_t last_tense_ms;

// Gesture helpers
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

// Legacy validity characteristic
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

// Do not switch back to legacy LED mode here.
// Once a gesture controls the LED, its state persists.
    LOG_INF("Received: %s",
            state == MYO_VALID ? "VALID" : "INVALID");

    return len;
}

// Receives classified gesture events.
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

   struct queued_gesture event;

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
            &gesture_queue, &event, K_NO_WAIT) != 0) {

        LOG_WRN("Gesture queue full");

        return BT_GATT_ERR(
            BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

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
        'M', 'y', 'o', 'P', 'C'),
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
        LOG_INF("Advertising as MyoPC");
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

    k_msgq_purge(&gesture_queue);

    atomic_inc(&session_epoch);
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

    atomic_inc(&session_epoch);
    k_msgq_purge(&gesture_queue);

    LOG_INF("Disconnected: %u", reason);

    start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

// Gesture actuation dispatcher
static void process_gesture(
    const struct queued_gesture *event)
{
    if (!atomic_get(&link_up) ||
        event->session !=
            (uint32_t)atomic_get(&session_epoch)) {
        return;
    }

    // New BLE session: reset duplicate tracking.
    if (event->session != processed_session) {
        processed_session = event->session;

        sequence_seen = false;
        tense_seen = false;
    }

    uint8_t gesture = event->packet.gesture;
    uint8_t sequence = event->packet.sequence;

    // Reject repeated packets carrying the
    // same sequence number.
    if (sequence_seen && sequence == last_sequence) {
        LOG_WRN("Duplicate packet: seq=%u", sequence);
        return;
    }

    sequence_seen = true;
    last_sequence = sequence;

    LOG_INF("Gesture: %s seq=%u",
            gesture_name(gesture), sequence);

    // A classified gesture now owns the actuator.
    atomic_set(&led_mode, LED_MODE_GESTURE);

    int hid_err = 0;
    switch (gesture) {

    case MYO_GESTURE_TENSE:

        // Prevent accidental rapid TENSE repeats.
        if (tense_seen &&
            (uint32_t)(event->timestamp_ms -
                        last_tense_ms) < TENSE_GUARD_MS) {

            LOG_INF("TENSE suppressed by cooldown");
            return;
        }

        tense_seen = true;
        last_tense_ms = event->timestamp_ms;

        // Toggle physical LED.
        gesture_led_on = !gesture_led_on;
        LOG_INF("LIGHT: %s",
                gesture_led_on ? "ON" : "OFF");

        // Send Enter to laptop.
        hid_err = myo_usb_hid_enqueue(
            MYO_HID_ENTER);
        break;

    case MYO_GESTURE_SWIPE_UP:
        hid_err = myo_usb_hid_enqueue(
            MYO_HID_UP);
        break;

    case MYO_GESTURE_SWIPE_DOWN:
        hid_err = myo_usb_hid_enqueue(
            MYO_HID_DOWN);
        break;

    case MYO_GESTURE_SWIPE_LEFT:
        hid_err = myo_usb_hid_enqueue(
            MYO_HID_LEFT);
        break;

    case MYO_GESTURE_SWIPE_RIGHT:
        hid_err = myo_usb_hid_enqueue(
            MYO_HID_RIGHT);
        break;

    default:
        break;
    }

    if (hid_err == -ENOTCONN) {
        LOG_DBG("USB not connected, HID skipped");
    } else if (hid_err) {
        LOG_WRN("HID enqueue failed: %d", hid_err);
    }
}

// LED output
static void update_led(void)
{
    bool led_on;

    if (atomic_get(&led_mode) == LED_MODE_GESTURE) {
        led_on = gesture_led_on;
    } else {

        // Original wearable validity behavior.
        uint32_t now = k_uptime_get_32();

        bool fresh =
            (uint32_t)(now -
                (uint32_t)atomic_get(&last_rx_ms))
            < RX_TIMEOUT_MS;

        led_on =
            atomic_get(&link_up) &&
            atomic_get(&rx_valid) &&
            fresh;
    }

    static int previous_output = -1;
    int requested_output = led_on ? 1 : 0;

    if (requested_output != previous_output) {
        int err = gpio_pin_set_dt(
            &led, requested_output);

        if (err) {
            LOG_ERR("LED update failed: %d", err);
        } else {
            previous_output = requested_output;
        }
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

    
    LOG_INF("GestureControl receiver starting");
    atomic_set(&led_mode, LED_MODE_LEGACY);

    int err = myo_usb_hid_init();

    if (err) {
        LOG_ERR("USB HID init failed: %d", err);
    }

    err = bt_enable(NULL);

    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return 0;
    }

    start_advertising();

    while (1) {
        struct queued_gesture event;

        // Process BLE events in the main thread.
        if (k_msgq_get(
                &gesture_queue,
                &event,
                K_MSEC(25)) == 0) {

            process_gesture(&event);
        }

        // Update the physical output independently.
        update_led();
    }

    return 0;
}
