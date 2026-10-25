#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <errno.h>

#include <string.h>
#include "myo_ble.h"
#include "gesture_config.h"
#include "receiver_links.h"
#include "usb_hid.h"


LOG_MODULE_REGISTER(receiver, LOG_LEVEL_INF);

#define ADV_RETRY_MS 250
#define LED_NODE DT_ALIAS(led0)
#define RX_TIMEOUT_MS 1500
#define TENSE_GUARD_MS 250

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

static atomic_t link_up;
static atomic_t advertising_needed;
static struct gc_receiver_links rx_links;
static atomic_t rx_valid;
static atomic_t last_rx_ms;
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

// Actuation state, owned by main thread.
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

    case MYO_GESTURE_EXTEND:
        return "EXTEND";

    case MYO_GESTURE_FLEX:
        return "FLEX";

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

    if (!accept_gesture_peer(conn)) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
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

    LOG_INF("Advertising as MyoPC");
    return 0;
}

static void begin_gesture_session(void)
{
    atomic_inc(&session_epoch);
    k_msgq_purge(&gesture_queue);
    atomic_set(&rx_valid, 0);
    atomic_set(&last_rx_ms, 0);
    atomic_set(&link_up, 1);
    LOG_INF("MyoPC gesture controller active");
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
        atomic_inc(&session_epoch);
        k_msgq_purge(&gesture_queue);
        myo_usb_hid_release_all();
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

    if (gesture == MYO_GESTURE_TENSE) {
        if (tense_seen &&
            (uint32_t)(event->timestamp_ms -
                       last_tense_ms) < TENSE_GUARD_MS) {
            LOG_INF("TENSE suppressed by cooldown");
            return;
        }

        tense_seen = true;
        last_tense_ms = event->timestamp_ms;

    }

    struct gc_mapping mapping = gc_config_get(gesture);
    int hid_err = myo_usb_hid_enqueue(gesture, &mapping);

    if (hid_err == -ENOTCONN) {
        LOG_DBG("USB not connected, HID skipped");
    } else if (hid_err) {
        LOG_WRN("HID enqueue failed: %d", hid_err);
    }
}

// Status LED is independent of HID and gesture mappings.
static void update_led(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t phase = now % 1200U;
    bool connected = atomic_get(&link_up) != 0;
    bool fresh = connected &&
        (uint32_t)(now - (uint32_t)atomic_get(&last_rx_ms))
            < RX_TIMEOUT_MS;
    bool valid = atomic_get(&rx_valid) != 0;

    bool on;
    if (!connected) {
        on = phase < 120U;
    } else if (!fresh) {
        on = phase < 120U ||
             (phase >= 280U && phase < 400U);
    } else if (!valid) {
        on = phase < 500U;
    } else {
        on = true;
    }

    static int last = -1;
    int requested = on ? 1 : 0;
    if (requested != last) {
        int err = gpio_pin_set_dt(&led, requested);
        if (!err) {
            last = requested;
        } else {
            LOG_WRN("Status LED update failed: %d", err);
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

    gc_config_init(GC_TARGET_COMPUTER);

    int err = myo_usb_hid_init();

    if (err) {
        LOG_ERR("USB HID init failed: %d", err);
    }

    gc_links_init(&rx_links);

    err = bt_enable(NULL);

    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return 0;
    }

    atomic_set(&advertising_needed, 1);

    uint32_t next_adv_retry_ms = 0;

    while (1) {
        struct queued_gesture event;

        // Process BLE events in the main thread.
        if (k_msgq_get(
                &gesture_queue,
                &event,
                K_MSEC(25)) == 0) {

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
        // Update the physical output independently.
        update_led();
    }

    return 0;
}
