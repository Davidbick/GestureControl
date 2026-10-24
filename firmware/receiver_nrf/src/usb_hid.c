
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

#define HID_ID_KEYBOARD 1
#define HID_ID_MOUSE    2
#define HID_ID_MEDIA    3

#define MYO_USB_VID 0xCAFE
#define MYO_USB_PID 0x4013

/*
 * One HID interface with three application
 * collections and separate report IDs.
 *
 * Keyboard report: ID + 8 bytes
 * Mouse report:    ID + 4 bytes
 * Media report:    ID + 2 bytes
 */

static const uint8_t hid_report_desc[] =
    HID_KEYBOARD_REPORT_DESC();

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
    myo_product, "GestureControl USB HID");

USBD_DESC_CONFIG_DEFINE(
    myo_cfg_desc, "GestureControl HID Configuration");

USBD_CONFIGURATION_DEFINE(
    myo_fs_cfg,
    0,
    50,
    &myo_cfg_desc
);

static atomic_t hid_ready;
static uint32_t idle_ms[4];

K_MSGQ_DEFINE(
    hid_action_queue,
    sizeof(enum myo_hid_action),
    12,
    4
);

static void hid_iface_ready(
    const struct device *dev,
    bool ready)
{
    ARG_UNUSED(dev);

    atomic_set(&hid_ready, ready ? 1 : 0);

    if (!ready) {
        k_msgq_purge(&hid_action_queue);
    }

    LOG_INF("USB HID %s",
            ready ? "ready" : "not ready");
}


static int hid_get_report(
    const struct device *dev,
    uint8_t type,
    uint8_t id,
    uint16_t len,
    uint8_t *buf)
{
    ARG_UNUSED(dev);

    if (type != HID_REPORT_TYPE_INPUT ||
        id != 0) {
        return -ENOTSUP;
    }

    if (len < 8) {
        return -ENOBUFS;
    }

    memset(buf, 0, 8);

    return 8;
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

static void hid_set_protocol(
    const struct device *dev,
    uint8_t protocol)
{
    ARG_UNUSED(dev);

    LOG_INF("Keyboard protocol: %u", protocol);
}

static int hid_set_report(
    const struct device *dev,
    uint8_t type,
    uint8_t id,
    uint16_t len,
    const uint8_t *buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(buf);

    // Accept the host's keyboard LED output.
    if (type == HID_REPORT_TYPE_OUTPUT &&
        id == 0 &&
        len == 1) {
        return 0;
    }

    return -ENOTSUP;
}

static const struct hid_device_ops hid_ops = {
    .iface_ready = hid_iface_ready,
    .get_report = hid_get_report,
    .set_report = hid_set_report,
    .set_idle = hid_set_idle,
    .get_idle = hid_get_idle,
    .set_protocol = hid_set_protocol,
};

/* Handle USB cable connection and removal. */
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
        atomic_set(&hid_ready, 0);
        k_msgq_purge(&hid_action_queue);
        int err = usbd_disable(ctx);

        if (err) {
            LOG_ERR("USB disable failed: %d", err);
        }
    }
}

bool usb_hid_ready(void)
{
    return atomic_get(&hid_ready) != 0;
}


// Standard USB HID keyboard usage codes.
#define HID_KEY_ENTER 0x28
#define HID_KEY_RIGHT 0x4F
#define HID_KEY_LEFT  0x50
#define HID_KEY_DOWN  0x51
#define HID_KEY_UP    0x52

/* Submit one HID input report. */
static int send_report(
    const uint8_t *report,
    uint16_t length)
{
    if (!usb_hid_ready()) {
        return -ENOTCONN;
    }

    const struct device *dev =
        DEVICE_DT_GET_ONE(zephyr_hid_device);

    return hid_device_submit_report(
        dev, length, report);
}



static int keyboard_tap(uint8_t key)
{
    UDC_STATIC_BUF_DEFINE(report, 8);

    // Standard 8-byte keyboard report.
    memset(report, 0, sizeof(report));

    // Byte 0: modifiers
    // Byte 1: reserved
    // Byte 2: first key
    report[2] = key;

    int err = send_report(report, sizeof(report));

    if (err) {
        return err;
    }

    k_msleep(35);

    // Release all keys.
    memset(report, 0, sizeof(report));

    err = send_report(report, sizeof(report));

    if (err) {
        return err;
    }

    k_msleep(15);

    return 0;
}


//Execute one queued HID action
static int execute_hid_action(
    enum myo_hid_action action)
{
    switch (action) {

    case MYO_HID_UP:
        return keyboard_tap(HID_KEY_UP);

    case MYO_HID_DOWN:
        return keyboard_tap(HID_KEY_DOWN);

    case MYO_HID_LEFT:
        return keyboard_tap(HID_KEY_LEFT);

    case MYO_HID_RIGHT:
        return keyboard_tap(HID_KEY_RIGHT);

    case MYO_HID_ENTER:
        return keyboard_tap(HID_KEY_ENTER);

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
            K_FOREVER);

        if (!usb_hid_ready()) {
            continue;
        }

        int err = execute_hid_action(action);

        if (err && err != -ENOTCONN) {
            LOG_WRN("HID action %d failed: %d",
                    action, err);
        }
    }
}

// Independent thread for HID actions
K_THREAD_DEFINE(
    hid_worker_id,
    1536,
    hid_worker,
    NULL, NULL, NULL,
    7,
    0,
    0
);

// Public entry point for the BLE dispatcher
int myo_usb_hid_enqueue(enum myo_hid_action action)
{
    if (action < MYO_HID_UP ||
        action > MYO_HID_ENTER) {
        return -EINVAL;
    }

    if (!usb_hid_ready()) {
        return -ENOTCONN;
    }

    return k_msgq_put(
        &hid_action_queue,
        &action,
        K_NO_WAIT);
}

int myo_usb_hid_init(void)
{
    int err;

    const struct device *hid_dev =
        DEVICE_DT_GET_ONE(zephyr_hid_device);

    if (!device_is_ready(hid_dev)) {
        LOG_ERR("HID device unavailable");
        return -ENODEV;
    }

    /* Register HID report descriptors. */
    err = hid_device_register(
        hid_dev,
        hid_report_desc,
        sizeof(hid_report_desc),
        &hid_ops
    );

    if (err) {
        LOG_ERR("HID registration failed: %d", err);
        return err;
    }

    err = usbd_add_descriptor(&myo_usbd, &myo_lang);
    if (err) return err;

    err = usbd_add_descriptor(&myo_usbd, &myo_mfr);
    if (err) return err;

    err = usbd_add_descriptor(&myo_usbd, &myo_product);
    if (err) return err;

    err = usbd_add_configuration(
        &myo_usbd,
        USBD_SPEED_FS,
        &myo_fs_cfg
    );
    if (err) return err;

    err = usbd_register_class(
        &myo_usbd,
        "hid_0",
        USBD_SPEED_FS,
        1
    );
    if (err) return err;

    err = usbd_msg_register_cb(
        &myo_usbd, usb_msg_cb);
    if (err) return err;

    err = usbd_init(&myo_usbd);
    if (err) {
        LOG_ERR("USB initialization failed: %d", err);
        return err;
    }

    if (!usbd_can_detect_vbus(&myo_usbd)) {
        err = usbd_enable(&myo_usbd);
        if (err) return err;
    }

    LOG_INF("USB HID initialized");
    return 0;
}
