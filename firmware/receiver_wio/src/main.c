
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

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
#include "gesture_config.h"
#include "receiver_links.h"
#include "oled.h"

LOG_MODULE_REGISTER(receiver, LOG_LEVEL_INF);

#define LED_NODE DT_ALIAS(led0)
#define RX_TIMEOUT_MS 1500U
#define TENSE_GUARD_MS 250U
#define ADV_RETRY_MS 250U

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

    
// External common-anode RGB LED (active-low PWM).
// Assumed colors: R=P0.29, G=P0.09, B=P0.02.
#define RED_PWM_NODE DT_NODELABEL(red_pwm)
#define GREEN_PWM_NODE DT_NODELABEL(green_pwm)
#define BLUE_PWM_NODE DT_NODELABEL(blue_pwm)

static const struct pwm_dt_spec red_pwm = PWM_DT_SPEC_GET(RED_PWM_NODE);
static const struct pwm_dt_spec green_pwm = PWM_DT_SPEC_GET(GREEN_PWM_NODE);
static const struct pwm_dt_spec blue_pwm = PWM_DT_SPEC_GET(BLUE_PWM_NODE);

// Color state survives BLE disconnect and normal light toggles.
static atomic_t green_mix;
static atomic_t blue_mix;

static int set_channel(const struct pwm_dt_spec *channel, uint8_t percent)
{
    if (percent > 100U) percent = 100U;
    uint32_t pulse = (uint32_t)(
        ((uint64_t)channel->period * percent) / 100U);
    return pwm_set_dt(channel, channel->period, pulse);
}

static int set_rgb_output(uint8_t brightness,
                          uint8_t green, uint8_t blue)
{
    if (brightness > 100U) brightness = 100U;
    if (green > 100U) green = 100U;
    if (blue > 100U) blue = 100U;

    // Each FLEX/EXTEND shifts color away from red.
    uint8_t dominant = green > blue ? green : blue;
    uint8_t red = 100U - dominant;

    uint8_t r = (uint8_t)((uint32_t)red * brightness / 100U);
    uint8_t g = (uint8_t)((uint32_t)green * brightness / 100U);
    uint8_t b = (uint8_t)((uint32_t)blue * brightness / 100U);

    int err = set_channel(&red_pwm, r);
    if (err) return err;
    err = set_channel(&green_pwm, g);
    if (err) return err;
    return set_channel(&blue_pwm, b);
}

// Shared BLE connection state.
static atomic_t link_up;
static atomic_t advertising_needed;
static struct gc_receiver_links rx_links;
static atomic_t rx_valid;
static atomic_t last_rx_ms;
static atomic_t session_epoch;

// Actual light state, independent of orientation.
static atomic_t light_on;
static atomic_t brightness_pct = ATOMIC_INIT(50);
static atomic_t last_nonzero_brightness = ATOMIC_INIT(50);
static atomic_t brightness_seq_seen;
static atomic_t last_brightness_seq;

// Encoded latest accepted BLE command packet.
// Bits 16-23: packet type
// Bits  8-15: first BLE byte
// Bits  0-7:  second BLE byte
// Zero means no command received.
static atomic_t oled_last_packet;

static void record_oled_packet(
    enum oled_rx_type type,
    uint8_t first,
    uint8_t second)
{
    uint32_t packed =
        ((uint32_t)type << 16) |
        ((uint32_t)first << 8) |
        (uint32_t)second;

    atomic_set(&oled_last_packet, (atomic_val_t)packed);
}


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

/* Called on the first validated gesture-side GATT write. */
static void begin_gesture_session(void);

static bool accept_gesture_peer(struct bt_conn *conn)
{
    enum gc_claim_result result =
        gc_links_claim(&rx_links, conn, GC_PEER_GESTURE);
    if (result == GC_CLAIM_DENIED) {
        LOG_WRN("Rejected gesture write from other BLE peer");
        return false;
    }
    if (result == GC_CLAIM_NEW) {
        begin_gesture_session();
    }
    return true;
}

static ssize_t config_command_write(
    struct bt_conn *conn, const struct bt_gatt_attr *attr,
    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (gc_links_claim(&rx_links, conn, GC_PEER_CONFIG) ==
        GC_CLAIM_DENIED) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    return gc_config_write(conn, attr, buf, len, offset, flags);
}

static ssize_t config_response_read(
    struct bt_conn *conn, const struct bt_gatt_attr *attr,
    void *buf, uint16_t len, uint16_t offset)
{
    if (gc_links_claim(&rx_links, conn, GC_PEER_CONFIG) ==
        GC_CLAIM_DENIED) {
        return BT_GATT_ERR(BT_ATT_ERR_READ_NOT_PERMITTED);
    }
    return gc_config_read_response(conn, attr, buf, len, offset);
}

static ssize_t config_info_read(
    struct bt_conn *conn, const struct bt_gatt_attr *attr,
    void *buf, uint16_t len, uint16_t offset)
{
    if (gc_links_claim(&rx_links, conn, GC_PEER_CONFIG) ==
        GC_CLAIM_DENIED) {
        return BT_GATT_ERR(BT_ATT_ERR_READ_NOT_PERMITTED);
    }
    return gc_config_read_info(conn, attr, buf, len, offset);
}

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
    case MYO_GESTURE_EXTEND:
        return "EXTEND";
    case MYO_GESTURE_FLEX:
        return "FLEX";
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

    if (!accept_gesture_peer(conn)) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
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

    if (!accept_gesture_peer(conn)) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
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
    // Record actual bytes received over BLE.
    record_oled_packet(
        OLED_RX_GESTURE,
        event.packet.gesture,
        event.packet.sequence
    );
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

    if (!accept_gesture_peer(conn)) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
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
    if (packet.percent > 0) {
        atomic_set(&last_nonzero_brightness, packet.percent);
    }
        
    record_oled_packet(
        OLED_RX_BRIGHTNESS,
        packet.percent,
        packet.sequence
    );

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
        NULL),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(GC_CONFIG_COMMAND_UUID_BYTES),
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE,
        NULL,
        config_command_write,
        NULL),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(GC_CONFIG_RESPONSE_UUID_BYTES),
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        config_response_read,
        NULL,
        NULL),

    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(GC_CONFIG_INFO_UUID_BYTES),
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        config_info_read,
        NULL,
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
        'M', 'y', 'o', 'L', 'i', 'g', 'h', 't'),
};

static int start_advertising(void)
{
    int err = bt_le_adv_start(
        BT_LE_ADV_CONN_FAST_1,
        ad, ARRAY_SIZE(ad),
        NULL, 0);

    if (err == -EALREADY) {
        return 0;
    }

    if (err == -ENOMEM) {
        /* Connection resources not yet recycled. */
        return err;
    }
    if (err) {
        LOG_WRN("Advertising failed: %d, retrying", err);
        return err;
    }

    LOG_INF("Advertising as MyoLight");
    return 0;
}

static void begin_gesture_session(void)
{
    atomic_inc(&session_epoch);
    k_msgq_purge(&gesture_queue);
    atomic_set(&oled_last_packet, 0);
    atomic_set(&rx_valid, 0);
    atomic_set(&last_rx_ms, 0);
    // Preserve lamp state/brightness across BLE sessions.
    atomic_set(&brightness_seq_seen, 0);
    atomic_set(&last_brightness_seq, 0);
    atomic_set(&link_up, 1);
    LOG_INF("MyoLight gesture controller active");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);
    if (err) {
        LOG_ERR("Connection failed: %u", err);
        atomic_set(&advertising_needed, 1);
        return;
    }
    gc_links_connected(&rx_links);
    atomic_set(&advertising_needed, 1);
    LOG_INF("BLE peer connected (%d/%d)",
            gc_links_count(&rx_links), CONFIG_BT_MAX_CONN);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    bool gesture_lost = gc_links_disconnected(&rx_links, conn);
    if (gesture_lost) {
        atomic_set(&link_up, 0);
        atomic_set(&rx_valid, 0);
        // Disconnect stops commands, not the lamp.
        atomic_set(&brightness_seq_seen, 0);
        atomic_set(&last_brightness_seq, 0);
        atomic_inc(&session_epoch);
        k_msgq_purge(&gesture_queue);
        atomic_set(&oled_last_packet, 0);
        LOG_INF("Gesture controller disconnected");
    } else {
        LOG_INF("Configuration/unclaimed peer disconnected");
    }
    LOG_INF("BLE disconnect reason: %u", reason);
    atomic_set(&advertising_needed, 1);
}

static void recycled(void)
{
    /* Retry advertising in main loop after a connection is freed. */
    atomic_set(&advertising_needed, 1);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

static void execute_light_action(const struct gc_mapping *mapping)
{
    uint8_t action = mapping->action;
    int brightness = atomic_get(&brightness_pct);
    bool is_on = atomic_get(&light_on) != 0;

    switch (action) {
    case GC_ACTION_NONE:
        return;

    case GC_ACTION_LIGHT_TOGGLE:
        is_on = !is_on;
        if (is_on && brightness == 0) {
            brightness = atomic_get(&last_nonzero_brightness);
        }
        break;

    case GC_ACTION_LIGHT_ON:
        is_on = true;
        if (brightness == 0) {
            brightness = atomic_get(&last_nonzero_brightness);
        }
        break;

    case GC_ACTION_LIGHT_OFF:
        is_on = false;
        break;

    case GC_ACTION_LIGHT_SET_BRIGHTNESS:
        brightness = mapping->amount;
        is_on = brightness > 0;
        break;

    case GC_ACTION_LIGHT_BRIGHTNESS_UP:
        brightness += mapping->amount;
        if (brightness > 100) {
            brightness = 100;
        }
        is_on = brightness > 0;
        break;

    case GC_ACTION_LIGHT_BRIGHTNESS_DOWN:
        brightness -= mapping->amount;
        if (brightness < 0) {
            brightness = 0;
        }
        is_on = brightness > 0;
        break;

    case GC_ACTION_LIGHT_MORE_BLUE: {
        int next = atomic_get(&blue_mix) + mapping->amount;
        if (next > 100) next = 100;
        atomic_set(&blue_mix, next);
        break;
    }

    case GC_ACTION_LIGHT_MORE_GREEN: {
        int next = atomic_get(&green_mix) + mapping->amount;
        if (next > 100) next = 100;
        atomic_set(&green_mix, next);
        break;
    }

    default:
        LOG_WRN("Unsupported light action: %u", action);
        return;
    }

    if (brightness > 0) {
        atomic_set(&last_nonzero_brightness, brightness);
    }

    atomic_set(&brightness_pct, brightness);
    atomic_set(&light_on, is_on ? 1 : 0);

    LOG_INF("Light action=%u on=%u brightness=%u",
            action, is_on ? 1 : 0, brightness);
}

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

    if (gesture == MYO_GESTURE_TENSE) {
        if (tense_seen &&
            (uint32_t)(event->timestamp_ms -
                       last_tense_ms) < TENSE_GUARD_MS) {
            LOG_INF("TENSE ignored: cooldown");
            return;
        }

        tense_seen = true;
        last_tense_ms = event->timestamp_ms;
    }

    struct gc_mapping mapping = gc_config_get(gesture);
    execute_light_action(&mapping);
}

int main(void)
{
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED not ready");
        return 0;
    }

    gpio_pin_configure_dt(
        &led, GPIO_OUTPUT_INACTIVE);

        
    if (!pwm_is_ready_dt(&red_pwm) ||
        !pwm_is_ready_dt(&green_pwm) ||
        !pwm_is_ready_dt(&blue_pwm)) {
        LOG_ERR("RGB PWM device not ready");
        return 0;
    }

    int pwm_err = set_rgb_output(0, 0, 0);
    if (pwm_err) {
        LOG_ERR("RGB PWM initialization failed: %d", pwm_err);
        return 0;
    }

    LOG_INF("RGB PWM ready; verify physical color pin order");


    LOG_INF("GestureControl receiver starting");

    int oled_err = oled_init();

    if (oled_err) {
        LOG_WRN("OLED initialization failed: %d", oled_err);
    } else {
        struct oled_status initial_status = {
            .connected = false,
            .valid = false,
            .fresh = false,
            .light_on = false,
            .brightness = 50,
            .rx_type = OLED_RX_NONE,
            .first_byte = 0,
            .second_byte = 0
        };

        oled_show(&initial_status);
    }

    gc_config_init(GC_TARGET_LIGHT);

    gc_links_init(&rx_links);

    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return 0;
    }

    atomic_set(&advertising_needed, 1);

    uint32_t next_adv_retry_ms = 0;

    bool first_render = true;
    bool previous_link = false;
    bool previous_valid = false;
    bool previous_fresh = false;
    bool previous_output = false;
    int previous_pwm_percent = -1;
    int previous_green_mix = -1;
    int previous_blue_mix = -1;
    uint8_t previous_brightness = 255;
    uint32_t previous_packet = UINT32_MAX;

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

        if (atomic_get(&advertising_needed) &&
            gc_links_count(&rx_links) < CONFIG_BT_MAX_CONN &&
            (int32_t)(now - next_adv_retry_ms) >= 0) {

            next_adv_retry_ms = now + ADV_RETRY_MS;

            if (start_advertising() == 0) {
                atomic_set(&advertising_needed, 0);
            }
        }

        bool fresh =
            (uint32_t)(now -
                (uint32_t)atomic_get(&last_rx_ms))
            < RX_TIMEOUT_MS;

        bool linked = atomic_get(&link_up) != 0;
        bool valid = atomic_get(&rx_valid) != 0;

        // Actual light is controlled by TENSE,
        // not by the validity heartbeat.
        bool output_on = atomic_get(&light_on) != 0;

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

        // Apply changed brightness or saved color even if disconnected.
        uint8_t green = (uint8_t)atomic_get(&green_mix);
        uint8_t blue = (uint8_t)atomic_get(&blue_mix);

        if ((int)requested_percent != previous_pwm_percent ||
            (int)green != previous_green_mix ||
            (int)blue != previous_blue_mix) {
            int ret = set_rgb_output(requested_percent, green, blue);
            if (ret) {
                LOG_ERR("RGB PWM update failed: %d", ret);
            } else {
                previous_pwm_percent = requested_percent;
                previous_green_mix = green;
                previous_blue_mix = blue;
                LOG_INF("RGB brightness=%u%% green=%u%% blue=%u%%",
                        requested_percent, green, blue);
            }
        }
                
        // Show only the most recently accepted gesture
        // or brightness BLE packet.
        uint32_t last_packet =
            (uint32_t)atomic_get(&oled_last_packet);

        uint8_t saved_brightness =
            (uint8_t)atomic_get(&brightness_pct);

        if (!oled_err &&
            (first_render ||
            linked != previous_link ||
            valid != previous_valid ||
            fresh != previous_fresh ||
            output_on != previous_output ||
            saved_brightness != previous_brightness ||
            last_packet != previous_packet)) {

            struct oled_status screen = {
                .connected = linked,
                .valid = valid,
                .fresh = fresh,
                .light_on = output_on,
                .brightness = saved_brightness,

                .rx_type = (enum oled_rx_type)
                    ((last_packet >> 16) & 0xffU),

                .first_byte = (uint8_t)
                    ((last_packet >> 8) & 0xffU),

                .second_byte = (uint8_t)
                    (last_packet & 0xffU)
            };

            int ret = oled_show(&screen);

            if (ret) {
                LOG_WRN("OLED update failed: %d", ret);
                oled_err = ret;
            }

            previous_link = linked;
            previous_valid = valid;
            previous_fresh = fresh;
            previous_output = output_on;
            previous_brightness = saved_brightness;
            previous_packet = last_packet;
            first_render = false;
        }
        k_msleep(50);

    }

    return 0;
}
