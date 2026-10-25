#include "gesture_config.h"
#include "myo_ble.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/att.h>

K_MUTEX_DEFINE(gc_config_lock);

static enum gc_target active_target;
static struct gc_mapping mappings[GC_GESTURE_COUNT + 1];
static struct gc_config_response last_response;
static atomic_t config_revision;
static uint8_t device_info[4];

static bool valid_gesture(uint8_t gesture)
{
    return gesture >= MYO_GESTURE_SWIPE_UP &&
           gesture <= MYO_GESTURE_FLEX;
}

static struct gc_mapping default_mapping(uint8_t gesture)
{
    struct gc_mapping mapping = {0};

    if (active_target == GC_TARGET_COMPUTER) {
        mapping.action = GC_ACTION_KEY_TAP;
        mapping.duration_ms = GC_DEFAULT_KEY_DURATION_MS;

        switch (gesture) {
        case MYO_GESTURE_SWIPE_UP:
            mapping.keycode = 0x52;
            break;
        case MYO_GESTURE_SWIPE_DOWN:
            mapping.keycode = 0x51;
            break;
        case MYO_GESTURE_SWIPE_LEFT:
            mapping.keycode = 0x50;
            break;
        case MYO_GESTURE_SWIPE_RIGHT:
            mapping.keycode = 0x4F;
            break;
        case MYO_GESTURE_TENSE:
            mapping.keycode = 0x28;
            break;
        default:
            return (struct gc_mapping){0};
        }
    } else if (active_target == GC_TARGET_LIGHT) {
        switch (gesture) {
        case MYO_GESTURE_SWIPE_UP:
            mapping.action = GC_ACTION_LIGHT_BRIGHTNESS_UP;
            mapping.amount = GC_DEFAULT_BRIGHTNESS_STEP;
            break;
        case MYO_GESTURE_SWIPE_DOWN:
            mapping.action = GC_ACTION_LIGHT_BRIGHTNESS_DOWN;
            mapping.amount = GC_DEFAULT_BRIGHTNESS_STEP;
            break;
        case MYO_GESTURE_TENSE:
            mapping.action = GC_ACTION_LIGHT_TOGGLE;
            break;
        case MYO_GESTURE_FLEX:
            mapping.action = GC_ACTION_LIGHT_MORE_BLUE;
            mapping.amount = GC_DEFAULT_BRIGHTNESS_STEP;
            break;
        case MYO_GESTURE_EXTEND:
            mapping.action = GC_ACTION_LIGHT_MORE_GREEN;
            mapping.amount = GC_DEFAULT_BRIGHTNESS_STEP;
            break;
        default:
            break;
        }
    }

    return mapping;
}

void gc_config_init(enum gc_target target)
{
    k_mutex_lock(&gc_config_lock, K_FOREVER);

    active_target = target;
    atomic_set(&config_revision, 0);

    for (uint8_t i = 1; i <= GC_GESTURE_COUNT; i++) {
        mappings[i] = default_mapping(i);
    }

    memset(&last_response, 0, sizeof(last_response));
    last_response.version = GC_CONFIG_VERSION;

    device_info[0] = GC_CONFIG_VERSION;
    device_info[1] = (uint8_t)target;
    device_info[2] = GC_GESTURE_COUNT;
    device_info[3] = target == GC_TARGET_COMPUTER ? 0x03 : 0x04;

    k_mutex_unlock(&gc_config_lock);
}

struct gc_mapping gc_config_get(uint8_t gesture)
{
    struct gc_mapping result = {0};

    if (!valid_gesture(gesture)) {
        return result;
    }

    k_mutex_lock(&gc_config_lock, K_FOREVER);
    result = mappings[gesture];
    k_mutex_unlock(&gc_config_lock);
    return result;
}

uint32_t gc_config_revision(void)
{
    return (uint32_t)atomic_get(&config_revision);
}

static enum gc_status validate_mapping(const struct gc_mapping *m)
{
    uint8_t action = m->action;
    bool computer = active_target == GC_TARGET_COMPUTER;

    if (action == GC_ACTION_NONE) {
        return m->keycode == 0 && m->modifiers == 0 &&
               m->amount == 0 && m->duration_ms == 0
            ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
    }

    bool computer_action =
        (action >= GC_ACTION_KEY_TAP &&
         action <= GC_ACTION_MOUSE_SCROLL_DOWN) ||
        (action == GC_ACTION_MOUSE_DOUBLE_CLICK) ||
        (action == GC_ACTION_KEY_TOGGLE);
    bool light_action =
        (action >= GC_ACTION_LIGHT_TOGGLE &&
         action <= GC_ACTION_LIGHT_BRIGHTNESS_DOWN) ||
        action == GC_ACTION_LIGHT_MORE_BLUE ||
        action == GC_ACTION_LIGHT_MORE_GREEN;

    if ((computer && !computer_action) ||
        (!computer && !light_action)) {
        return GC_STATUS_UNSUPPORTED_ACTION;
    }

    if (action == GC_ACTION_KEY_TAP ||
        action == GC_ACTION_KEY_TOGGLE) {
        bool duration_ok = action == GC_ACTION_KEY_TOGGLE
            ? m->duration_ms == 0
            : (m->duration_ms >= 30 && m->duration_ms <= 2000);
        return m->keycode >= 0x04 && m->keycode <= 0xA4 &&
               (m->modifiers & 0xF0) == 0 &&
               m->amount == 0 && duration_ok
            ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
    }

    if (m->keycode != 0 || m->modifiers != 0) {
        return GC_STATUS_INVALID_PARAMETER;
    }

    bool click =
        action == GC_ACTION_MOUSE_LEFT_CLICK ||
        action == GC_ACTION_MOUSE_RIGHT_CLICK ||
        action == GC_ACTION_MOUSE_MIDDLE_CLICK ||
        action == GC_ACTION_MOUSE_DOUBLE_CLICK;

    if (click) {
        return m->amount == 0 &&
               m->duration_ms >= 30 && m->duration_ms <= 2000
            ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
    }

    if (m->duration_ms != 0) {
        return GC_STATUS_INVALID_PARAMETER;
    }

    if (action >= GC_ACTION_MOUSE_MOVE_UP &&
        action <= GC_ACTION_MOUSE_MOVE_RIGHT) {
        return m->amount >= 1 && m->amount <= 127
            ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
    }

    if (action == GC_ACTION_MOUSE_SCROLL_UP ||
        action == GC_ACTION_MOUSE_SCROLL_DOWN) {
        return m->amount >= 1 && m->amount <= 10
            ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
    }

    if (action == GC_ACTION_LIGHT_SET_BRIGHTNESS) {
        return m->amount <= 100
            ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
    }

    if (action == GC_ACTION_LIGHT_BRIGHTNESS_UP ||
        action == GC_ACTION_LIGHT_BRIGHTNESS_DOWN ||
        action == GC_ACTION_LIGHT_MORE_BLUE ||
        action == GC_ACTION_LIGHT_MORE_GREEN) {
        return m->amount >= 1 && m->amount <= 100
            ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
    }

    return m->amount == 0
        ? GC_STATUS_OK : GC_STATUS_INVALID_PARAMETER;
}

static void process_request(const struct gc_config_request *req,
                            struct gc_config_response *rsp)
{
    memset(rsp, 0, sizeof(*rsp));
    rsp->version = GC_CONFIG_VERSION;
    rsp->transaction_id = req->transaction_id;
    rsp->operation = req->operation;
    rsp->target = (uint8_t)active_target;
    rsp->gesture = req->gesture;

    enum gc_status status = GC_STATUS_OK;

    if (req->version != GC_CONFIG_VERSION) {
        status = GC_STATUS_BAD_VERSION;
    } else if (req->target != active_target) {
        status = GC_STATUS_WRONG_TARGET;
    } else if (req->flags != 0) {
        status = GC_STATUS_INVALID_PARAMETER;
    } else if (req->operation < GC_OP_GET ||
               req->operation > GC_OP_RESET_ALL) {
        status = GC_STATUS_INVALID_OPERATION;
    } else if (req->operation != GC_OP_SET &&
               (req->action || req->keycode || req->modifiers ||
                req->amount || req->duration_lo || req->duration_hi)) {
        status = GC_STATUS_INVALID_PARAMETER;
    } else if (req->operation == GC_OP_RESET_ALL) {
        if (req->gesture != 0) {
            status = GC_STATUS_INVALID_GESTURE;
        } else {
            for (uint8_t i = 1; i <= GC_GESTURE_COUNT; i++) {
                mappings[i] = default_mapping(i);
            }
        }
    } else if (!valid_gesture(req->gesture)) {
        status = GC_STATUS_INVALID_GESTURE;
    } else if (req->operation == GC_OP_SET) {
        struct gc_mapping candidate = {
            .action = req->action,
            .keycode = req->keycode,
            .modifiers = req->modifiers,
            .amount = req->amount,
            .duration_ms = (uint16_t)req->duration_lo |
                           ((uint16_t)req->duration_hi << 8)
        };
        status = validate_mapping(&candidate);
        if (status == GC_STATUS_OK) {
            mappings[req->gesture] = candidate;
        }
    } else if (req->operation == GC_OP_RESET) {
        mappings[req->gesture] = default_mapping(req->gesture);
    }

    if (status == GC_STATUS_OK &&
        (req->operation == GC_OP_SET ||
         req->operation == GC_OP_RESET ||
         req->operation == GC_OP_RESET_ALL)) {
        atomic_inc(&config_revision);
    }

    rsp->status = status;
    if (status == GC_STATUS_OK && valid_gesture(req->gesture)) {
        struct gc_mapping current = mappings[req->gesture];
        rsp->action = current.action;
        rsp->keycode = current.keycode;
        rsp->modifiers = current.modifiers;
        rsp->amount = current.amount;
        rsp->duration_lo = (uint8_t)(current.duration_ms & 0xFF);
        rsp->duration_hi = (uint8_t)(current.duration_ms >> 8);
    }
}

ssize_t gc_config_write(struct bt_conn *conn,
                        const struct bt_gatt_attr *attr,
                        const void *buf,
                        uint16_t len,
                        uint16_t offset,
                        uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(struct gc_config_request)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct gc_config_request request;
    memcpy(&request, buf, sizeof(request));

    if (k_mutex_lock(&gc_config_lock, K_NO_WAIT) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    process_request(&request, &last_response);
    k_mutex_unlock(&gc_config_lock);
    return len;
}

ssize_t gc_config_read_response(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                void *buf,
                                uint16_t len,
                                uint16_t offset)
{
    struct gc_config_response response;

    if (k_mutex_lock(&gc_config_lock, K_NO_WAIT) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    response = last_response;
    k_mutex_unlock(&gc_config_lock);

    return bt_gatt_attr_read(
        conn, attr, buf, len, offset,
        &response, sizeof(response));
}

ssize_t gc_config_read_info(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf,
                            uint16_t len,
                            uint16_t offset)
{
    return bt_gatt_attr_read(
        conn, attr, buf, len, offset,
        device_info, sizeof(device_info));
}
