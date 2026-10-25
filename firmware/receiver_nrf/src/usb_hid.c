#include "usb_hid.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/class/hid.h>

LOG_MODULE_REGISTER(myo_usb, LOG_LEVEL_INF);

#define MYO_USB_VID 0xCAFE
#define MYO_USB_PID 0x4015

#define KEYBOARD_REPORT_SIZE 8
#define MOUSE_REPORT_SIZE 4

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04

static const uint8_t keyboard_report_desc[] =
    HID_KEYBOARD_REPORT_DESC();

static const uint8_t mouse_report_desc[] =
    HID_MOUSE_REPORT_DESC(3);

static const struct device *const keyboard_dev =
    DEVICE_DT_GET(DT_NODELABEL(hid_dev_0));

static const struct device *const mouse_dev =
    DEVICE_DT_GET(DT_NODELABEL(hid_dev_1));

USBD_DEVICE_DEFINE(
    myo_usbd,
    DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
    MYO_USB_VID,
    MYO_USB_PID
);

USBD_DESC_LANG_DEFINE(myo_lang);

USBD_DESC_MANUFACTURER_DEFINE(
    myo_mfr, "GestureControl");

USBD_DESC_PRODUCT_DEFINE(
    myo_product, "GestureControl Keyboard and Mouse");

USBD_DESC_CONFIG_DEFINE(
    myo_cfg_desc, "GestureControl HID Configuration");

USBD_CONFIGURATION_DEFINE(
    myo_fs_cfg,
    0,
    50,
    &myo_cfg_desc
);

static atomic_t keyboard_ready;
static atomic_t mouse_ready;
static atomic_t release_requested;

/* HID worker owns these five independent gesture toggles. */
static uint8_t toggle_keys[GC_GESTURE_COUNT + 1];
static uint8_t toggle_modifiers[GC_GESTURE_COUNT + 1];

static uint32_t idle_ms[4];

struct queued_hid_action {
    uint8_t gesture;
    struct gc_mapping mapping;
};

K_MSGQ_DEFINE(
    hid_action_queue,
    sizeof(struct queued_hid_action),
    12,
    4
);

static void keyboard_iface_ready(
    const struct device *dev,
    bool ready)
{
    ARG_UNUSED(dev);

    atomic_set(&keyboard_ready, ready ? 1 : 0);

    if (!ready) {
        k_msgq_purge(&hid_action_queue);
        atomic_set(&release_requested, 1);
    }

    LOG_INF("Keyboard HID %s",
            ready ? "ready" : "not ready");
}

static void mouse_iface_ready(
    const struct device *dev,
    bool ready)
{
    ARG_UNUSED(dev);

    atomic_set(&mouse_ready, ready ? 1 : 0);

    LOG_INF("Mouse HID %s",
            ready ? "ready" : "not ready");
}

static int keyboard_get_report(
    const struct device *dev,
    uint8_t type,
    uint8_t id,
    uint16_t len,
    uint8_t *buf)
{
    ARG_UNUSED(dev);

    if (type != HID_REPORT_TYPE_INPUT || id != 0) {
        return -ENOTSUP;
    }

    if (len < KEYBOARD_REPORT_SIZE) {
        return -ENOBUFS;
    }

    memset(buf, 0, KEYBOARD_REPORT_SIZE);

    return KEYBOARD_REPORT_SIZE;
}

static int mouse_get_report(
    const struct device *dev,
    uint8_t type,
    uint8_t id,
    uint16_t len,
    uint8_t *buf)
{
    ARG_UNUSED(dev);

    if (type != HID_REPORT_TYPE_INPUT || id != 0) {
        return -ENOTSUP;
    }

    if (len < MOUSE_REPORT_SIZE) {
        return -ENOBUFS;
    }

    memset(buf, 0, MOUSE_REPORT_SIZE);

    return MOUSE_REPORT_SIZE;
}

static void hid_set_idle(
    const struct device *dev,
    uint8_t id,
    uint32_t duration)
{
    ARG_UNUSED(dev);

    if (id == 0) {
        for (int i = 1; i <= 3; i++) {
            idle_ms[i] = duration;
        }
    } else if (id <= 3) {
        idle_ms[id] = duration;
    }
}

static uint32_t hid_get_idle(
    const struct device *dev,
    uint8_t id)
{
    ARG_UNUSED(dev);

    return id <= 3 ? idle_ms[id] : 0;
}

static void keyboard_set_protocol(
    const struct device *dev,
    uint8_t protocol)
{
    ARG_UNUSED(dev);

    LOG_INF("Keyboard protocol: %u", protocol);
}

static void mouse_set_protocol(
    const struct device *dev,
    uint8_t protocol)
{
    ARG_UNUSED(dev);

    LOG_INF("Mouse protocol: %u", protocol);
}

static int keyboard_set_report(
    const struct device *dev,
    uint8_t type,
    uint8_t id,
    uint16_t len,
    const uint8_t *buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(buf);

    if (type == HID_REPORT_TYPE_OUTPUT &&
        id == 0 &&
        len == 1) {
        return 0;
    }

    return -ENOTSUP;
}

static const struct hid_device_ops keyboard_ops = {
    .iface_ready = keyboard_iface_ready,
    .get_report = keyboard_get_report,
    .set_report = keyboard_set_report,
    .set_idle = hid_set_idle,
    .get_idle = hid_get_idle,
    .set_protocol = keyboard_set_protocol,
};

static const struct hid_device_ops mouse_ops = {
    .iface_ready = mouse_iface_ready,
    .get_report = mouse_get_report,
    .set_idle = hid_set_idle,
    .get_idle = hid_get_idle,
    .set_protocol = mouse_set_protocol,
};

static void usb_msg_cb(
    struct usbd_context *const ctx,
    const struct usbd_msg *const msg)
{
    if (msg->type == USBD_MSG_VBUS_READY) {
        int err = usbd_enable(ctx);

        if (err) {
            LOG_ERR("USB enable failed: %d", err);
        }
    }

    if (msg->type == USBD_MSG_VBUS_REMOVED) {
        atomic_set(&keyboard_ready, 0);
        atomic_set(&mouse_ready, 0);

        k_msgq_purge(&hid_action_queue);
        atomic_set(&release_requested, 1);
        int err = usbd_disable(ctx);

        if (err) {
            LOG_ERR("USB disable failed: %d", err);
        }
    }
}

bool usb_hid_ready(void)
{
    return atomic_get(&keyboard_ready) != 0;
}

static bool hid_action_ready(const struct gc_mapping *mapping)
{
    if (mapping->action == GC_ACTION_NONE) {
        return true;
    }

    if (mapping->action == GC_ACTION_KEY_TAP ||
        mapping->action == GC_ACTION_KEY_TOGGLE) {
        return atomic_get(&keyboard_ready) != 0;
    }

    return atomic_get(&mouse_ready) != 0;
}

static int send_report(
    const struct device *dev,
    const uint8_t *report,
    uint16_t length)
{
    if (dev == keyboard_dev &&
        !atomic_get(&keyboard_ready)) {
        return -ENOTCONN;
    }

    if (dev == mouse_dev &&
        !atomic_get(&mouse_ready)) {
        return -ENOTCONN;
    }

    return hid_device_submit_report(
        dev,
        length,
        report
    );
}

/* Combine toggled keys with a temporary tap in one 6-key HID report. */
static int keyboard_send_state(uint8_t temporary_key, uint8_t temporary_mods)
{
    UDC_STATIC_BUF_DEFINE(report, KEYBOARD_REPORT_SIZE);
    memset(report, 0, sizeof(report));
    report[0] = temporary_mods;

    uint8_t used = 0;
    for (uint8_t g = 1; g <= GC_GESTURE_COUNT; g++) {
        uint8_t key = toggle_keys[g];
        if (key == 0) {
            continue;
        }
        report[0] |= toggle_modifiers[g];
        bool found = false;
        for (uint8_t i = 0; i < used; i++) {
            if (report[2 + i] == key) {
                found = true;
                break;
            }
        }
        if (!found && used < 6) {
            report[2 + used++] = key;
        }
    }
    if (temporary_key != 0) {
        bool found = false;
        for (uint8_t i = 0; i < used; i++) {
            if (report[2 + i] == temporary_key) {
                found = true;
                break;
            }
        }
        if (!found && used < 6) {
            report[2 + used] = temporary_key;
        }
    }
    return send_report(keyboard_dev, report, sizeof(report));
}

static void keyboard_release_all(void)
{
    memset(toggle_keys, 0, sizeof(toggle_keys));
    memset(toggle_modifiers, 0, sizeof(toggle_modifiers));
    if (atomic_get(&keyboard_ready)) {
        int err = keyboard_send_state(0, 0);
        if (err) {
            LOG_WRN("Keyboard release failed: %d", err);
        }
    }
}

static int keyboard_toggle(uint8_t gesture, uint8_t key,
                           uint8_t modifiers)
{
    if (gesture == 0 || gesture > GC_GESTURE_COUNT) {
        return -EINVAL;
    }
    uint8_t old_key = toggle_keys[gesture];
    uint8_t old_mods = toggle_modifiers[gesture];
    if (old_key == 0) {
        toggle_keys[gesture] = key;
        toggle_modifiers[gesture] = modifiers;
    } else {
        toggle_keys[gesture] = 0;
        toggle_modifiers[gesture] = 0;
    }
    int err = keyboard_send_state(0, 0);
    if (err) {
        toggle_keys[gesture] = old_key;
        toggle_modifiers[gesture] = old_mods;
        return err;
    }
    k_msleep(15);
    return 0;
}

static int keyboard_tap(uint8_t key,
                        uint8_t modifiers,
                        uint16_t duration_ms)
{
    int err = keyboard_send_state(key, modifiers);
    if (err) {
        return err;
    }
    k_msleep(duration_ms);
    /* Keep toggles pressed after the short-lived tap releases. */
    err = keyboard_send_state(0, 0);
    if (err) {
        return err;
    }
    k_msleep(15);
    return 0;
}

static int mouse_move(int8_t dx, int8_t dy)
{
    UDC_STATIC_BUF_DEFINE(report, MOUSE_REPORT_SIZE);

    memset(report, 0, sizeof(report));
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;

    return send_report(mouse_dev, report, sizeof(report));
}

static int mouse_scroll(int8_t amount)
{
    UDC_STATIC_BUF_DEFINE(report, MOUSE_REPORT_SIZE);

    memset(report, 0, sizeof(report));
    report[3] = (uint8_t)amount;

    return send_report(mouse_dev, report, sizeof(report));
}

static int mouse_click(uint8_t button,
                       uint8_t count,
                       uint16_t duration_ms)
{
    UDC_STATIC_BUF_DEFINE(report, MOUSE_REPORT_SIZE);

    for (uint8_t i = 0; i < count; i++) {
        memset(report, 0, sizeof(report));
        report[0] = button;

        int err = send_report(mouse_dev, report, sizeof(report));
        if (err) {
            return err;
        }

        k_msleep(duration_ms);

        // Release the mouse button after every click.
        memset(report, 0, sizeof(report));
        err = send_report(mouse_dev, report, sizeof(report));
        if (err) {
            return err;
        }

        k_msleep(count == 2 && i == 0 ? 80 : 15);
    }

    return 0;
}

static int execute_hid_action(uint8_t gesture,
                              const struct gc_mapping *mapping)
{
    int8_t amount = (int8_t)mapping->amount;

    switch (mapping->action) {
    case GC_ACTION_NONE:
        return 0;

    case GC_ACTION_KEY_TAP:
        return keyboard_tap(mapping->keycode,
                            mapping->modifiers,
                            mapping->duration_ms);

    case GC_ACTION_KEY_TOGGLE:
        return keyboard_toggle(gesture, mapping->keycode,
                               mapping->modifiers);

    case GC_ACTION_MOUSE_LEFT_CLICK:
        return mouse_click(MOUSE_BUTTON_LEFT, 1, mapping->duration_ms);

    case GC_ACTION_MOUSE_RIGHT_CLICK:
        return mouse_click(MOUSE_BUTTON_RIGHT, 1, mapping->duration_ms);

    case GC_ACTION_MOUSE_MIDDLE_CLICK:
        return mouse_click(MOUSE_BUTTON_MIDDLE, 1, mapping->duration_ms);

    case GC_ACTION_MOUSE_DOUBLE_CLICK:
        return mouse_click(MOUSE_BUTTON_LEFT, 2, mapping->duration_ms);

    case GC_ACTION_MOUSE_MOVE_UP:
        return mouse_move(0, -amount);

    case GC_ACTION_MOUSE_MOVE_DOWN:
        return mouse_move(0, amount);

    case GC_ACTION_MOUSE_MOVE_LEFT:
        return mouse_move(-amount, 0);

    case GC_ACTION_MOUSE_MOVE_RIGHT:
        return mouse_move(amount, 0);

    case GC_ACTION_MOUSE_SCROLL_UP:
        return mouse_scroll(amount);

    case GC_ACTION_MOUSE_SCROLL_DOWN:
        return mouse_scroll(-amount);

    default:
        return -EINVAL;
    }
}

static void hid_worker(
    void *arg1,
    void *arg2,
    void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    uint32_t revision = gc_config_revision();
    while (1) {
        uint32_t current_revision = gc_config_revision();
        if (atomic_set(&release_requested, 0) ||
            current_revision != revision) {
            k_msgq_purge(&hid_action_queue);
            keyboard_release_all();
            revision = gc_config_revision();
        }

        struct queued_hid_action event;
        if (k_msgq_get(&hid_action_queue, &event,
                       K_MSEC(20)) != 0) {
            continue;
        }
        if (atomic_get(&release_requested) ||
            gc_config_revision() != revision) {
            continue;
        }
        if (!hid_action_ready(&event.mapping)) {
            continue;
        }
        int err = execute_hid_action(event.gesture, &event.mapping);
        if (err && err != -ENOTCONN) {
            LOG_WRN("HID action %u failed: %d",
                    event.mapping.action, err);
        }
    }
}

K_THREAD_DEFINE(
    hid_worker_id,
    1536,
    hid_worker,
    NULL, NULL, NULL,
    7,
    0,
    0
);

void myo_usb_hid_release_all(void)
{
    k_msgq_purge(&hid_action_queue);
    atomic_set(&release_requested, 1);
}

int myo_usb_hid_enqueue(uint8_t gesture,
                       const struct gc_mapping *mapping)
{
    if (mapping == NULL || gesture == 0 ||
        gesture > GC_GESTURE_COUNT) {
        return -EINVAL;
    }
    if (mapping->action == GC_ACTION_NONE) {
        return 0;
    }
    if (!hid_action_ready(mapping)) {
        return -ENOTCONN;
    }
    struct queued_hid_action event = {
        .gesture = gesture,
        .mapping = *mapping
    };
    return k_msgq_put(&hid_action_queue, &event, K_NO_WAIT);
}

int myo_usb_hid_init(void)
{
    int err;

    if (!device_is_ready(keyboard_dev)) {
        LOG_ERR("Keyboard HID device unavailable");
        return -ENODEV;
    }

    if (!device_is_ready(mouse_dev)) {
        LOG_ERR("Mouse HID device unavailable");
        return -ENODEV;
    }

    err = hid_device_register(
        keyboard_dev,
        keyboard_report_desc,
        sizeof(keyboard_report_desc),
        &keyboard_ops
    );

    if (err) {
        LOG_ERR("Keyboard registration failed: %d", err);
        return err;
    }

    err = hid_device_register(
        mouse_dev,
        mouse_report_desc,
        sizeof(mouse_report_desc),
        &mouse_ops
    );

    if (err) {
        LOG_ERR("Mouse registration failed: %d", err);
        return err;
    }

    err = usbd_add_descriptor(&myo_usbd, &myo_lang);
    if (err) {
        return err;
    }

    err = usbd_add_descriptor(&myo_usbd, &myo_mfr);
    if (err) {
        return err;
    }

    err = usbd_add_descriptor(&myo_usbd, &myo_product);
    if (err) {
        return err;
    }

    err = usbd_add_configuration(
        &myo_usbd,
        USBD_SPEED_FS,
        &myo_fs_cfg
    );

    if (err) {
        return err;
    }

    err = usbd_register_class(
        &myo_usbd,
        "hid_0",
        USBD_SPEED_FS,
        1
    );

    if (err) {
        LOG_ERR("Keyboard class registration failed: %d", err);
        return err;
    }

    err = usbd_register_class(
        &myo_usbd,
        "hid_1",
        USBD_SPEED_FS,
        1
    );

    if (err) {
        LOG_ERR("Mouse class registration failed: %d", err);
        return err;
    }

    err = usbd_msg_register_cb(
        &myo_usbd,
        usb_msg_cb
    );

    if (err) {
        return err;
    }

    err = usbd_init(&myo_usbd);

    if (err) {
        LOG_ERR("USB initialization failed: %d", err);
        return err;
    }

    if (!usbd_can_detect_vbus(&myo_usbd)) {
        err = usbd_enable(&myo_usbd);

        if (err) {
            return err;
        }
    }

    LOG_INF("Keyboard and mouse HID initialized");
    return 0;
}