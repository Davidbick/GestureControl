
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
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
#define TENSE_GUARD_MS 250U

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

    
// Hardware PWM output on P0.29.
#define RED_PWM_NODE DT_NODELABEL(red_pwm)

static const struct pwm_dt_spec red_pwm =
    PWM_DT_SPEC_GET(RED_PWM_NODE);

// Set physical red LED brightness.
// PWM_POLARITY_INVERTED is configured in app.overlay.
static int set_red_brightness(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }

    uint32_t pulse_ns =
        (uint32_t)(((uint64_t)red_pwm.period *
                    percent) / 100U);

    return pwm_set_dt(
        &red_pwm,
        red_pwm.period,
        pulse_ns
    );
}

// Shared BLE connection state.
static atomic_t link_up;
static atomic_t rx_valid;
static atomic_t last_rx_ms;
static atomic_t session_epoch;

// Actual light state, independent of orientation.
static atomic_t light_on;
static atomic_t brightness_pct = ATOMIC_INIT(50);
static atomic_t brightness_seq_seen;
static atomic_t last_brightness_seq;

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

// Receive and store brightness.
static ssize_t write_brightness(
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
        len != sizeof(struct myo_brightness_packet)) {
        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        return 0;
    }

    struct myo_brightness_packet packet;

    memcpy(&packet, buf, sizeof(packet));

    if (!myo_brightness_valid(packet.percent)) {
        LOG_WRN("Invalid brightness: %u",
                packet.percent);

        return BT_GATT_ERR(
            BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    // Ignore duplicate brightness packets.
    if (atomic_get(&brightness_seq_seen) &&
        packet.sequence ==
            (uint8_t)atomic_get(&last_brightness_seq)) {

        LOG_DBG("Duplicate brightness seq=%u",
                packet.sequence);
        return len;
    }

    atomic_set(&last_brightness_seq, packet.sequence);
    atomic_set(&brightness_seq_seen, 1);
    atomic_set(&brightness_pct, packet.percent);

    LOG_INF("BLE RX: BRIGHTNESS=%u%% seq=%u",
            packet.percent,
            packet.sequence);

    return len;
}


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
        NULL),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(MYO_BRIGHTNESS_UUID_BYTES),
        BT_GATT_CHRC_WRITE |
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL,
        write_brightness,
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

    // Default to 50% until a brightness packet arrives.
    atomic_set(&brightness_pct, 50);
    atomic_set(&brightness_seq_seen, 0);
    atomic_set(&last_brightness_seq, 0);
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
    atomic_set(&brightness_pct, 50);
    atomic_set(&brightness_seq_seen, 0);

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

        
    if (!pwm_is_ready_dt(&red_pwm)) {
        LOG_ERR("Red LED PWM not ready");
        return 0;
    }

    // Start with external LED fully OFF.
    int pwm_err = set_red_brightness(0);

    if (pwm_err) {
        LOG_ERR("Initial PWM setup failed: %d",
                pwm_err);
        return 0;
    }

    LOG_INF("Red LED PWM ready on P0.29");


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
    int previous_pwm_percent = -1;

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

                
        // Physical brightness:
        // OFF = 0%
        // ON = most recently received brightness.
        uint8_t requested_percent =
            output_on
                ? (uint8_t)atomic_get(&brightness_pct)
                : 0U;

        // Defensive clamp.
        if (requested_percent > 100U) {
            requested_percent = 100U;
        }

        // Update PWM only when brightness changes.
        if ((int)requested_percent != previous_pwm_percent) {

            int ret = set_red_brightness(requested_percent);

            if (ret) {
                LOG_ERR("PWM update failed: %d", ret);
            } else {
                previous_pwm_percent = requested_percent;

                LOG_INF("Red LED PWM: %u%%",
                        requested_percent);
            }
        }


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
