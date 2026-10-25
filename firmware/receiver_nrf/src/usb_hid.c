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
#define MYO_USB_PID 0x4014

#define KEYBOARD_REPORT_SIZE 8
#define MOUSE_REPORT_SIZE 4

#define HID_KEY_A 0x04
#define HID_KEY_D 0x07

#define MOUSE_BUTTON_LEFT 0x01
#define MOUSE_SCROLL_STEP 1

static const uint8_t keyboard_report_desc[] =
    HID_KEYBOARD_REPORT_DESC();

static const uint8_t mouse_report_desc[] =
    HID_MOUSE_REPORT_DESC(2);

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

static uint32_t idle_ms[4];

K_MSGQ_DEFINE(
    hid_action_queue,
    sizeof(enum myo_hid_action),
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

static bool hid_action_ready(enum myo_hid_action action)
{
    if (action == MYO_HID_LEFT ||
        action == MYO_HID_RIGHT) {
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

static int keyboard_tap(uint8_t key)
{
    UDC_STATIC_BUF_DEFINE(
        report,
        KEYBOARD_REPORT_SIZE
    );

    memset(report, 0, sizeof(report));

    report[2] = key;

    int err = send_report(
        keyboard_dev,
        report,
        sizeof(report)
    );

    if (err) {
        return err;
    }

    k_msleep(35);

    memset(report, 0, sizeof(report));

    err = send_report(
        keyboard_dev,
        report,
        sizeof(report)
    );

    if (err) {
        return err;
    }

    k_msleep(15);

    return 0;
}

static int mouse_scroll(int8_t amount)
{
    UDC_STATIC_BUF_DEFINE(
        report,
        MOUSE_REPORT_SIZE
    );

    memset(report, 0, sizeof(report));

    // Byte 3 contains relative wheel movement.
    report[3] = (uint8_t)amount;

    return send_report(
        mouse_dev,
        report,
        sizeof(report)
    );
}

static int mouse_left_click(void)
{
    UDC_STATIC_BUF_DEFINE(
        report,
        MOUSE_REPORT_SIZE
    );

    memset(report, 0, sizeof(report));

    // Press left mouse button.
    report[0] = MOUSE_BUTTON_LEFT;

    int err = send_report(
        mouse_dev,
        report,
        sizeof(report)
    );

    if (err) {
        return err;
    }

    k_msleep(35);

    // Release left mouse button.
    memset(report, 0, sizeof(report));

    err = send_report(
        mouse_dev,
        report,
        sizeof(report)
    );

    if (err) {
        return err;
    }

    k_msleep(15);

    return 0;
}

static int execute_hid_action(
    enum myo_hid_action action)
{
    switch (action) {
    case MYO_HID_UP:
        return mouse_scroll(MOUSE_SCROLL_STEP);

    case MYO_HID_DOWN:
        return mouse_scroll(-MOUSE_SCROLL_STEP);

    case MYO_HID_LEFT:
        return keyboard_tap(HID_KEY_A);

    case MYO_HID_RIGHT:
        return keyboard_tap(HID_KEY_D);

    case MYO_HID_ENTER:
        return mouse_left_click();

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

    while (1) {
        enum myo_hid_action action;

        k_msgq_get(
            &hid_action_queue,
            &action,
            K_FOREVER
        );

        if (!hid_action_ready(action)) {
            continue;
        }

        int err = execute_hid_action(action);

        if (err && err != -ENOTCONN) {
            LOG_WRN(
                "HID action %d failed: %d",
                action,
                err
            );
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

int myo_usb_hid_enqueue(enum myo_hid_action action)
{
    if (action < MYO_HID_UP ||
        action > MYO_HID_ENTER) {
        return -EINVAL;
    }

    if (!hid_action_ready(action)) {
        return -ENOTCONN;
    }

    return k_msgq_put(
        &hid_action_queue,
        &action,
        K_NO_WAIT
    );
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