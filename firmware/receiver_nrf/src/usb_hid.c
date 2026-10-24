
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

LOG_MODULE_REGISTER(myo_usb, LOG_LEVEL_INF);

#define HID_ID_KEYBOARD 1
#define HID_ID_MOUSE    2
#define HID_ID_MEDIA    3

#define MYO_USB_VID 0xCAFE
#define MYO_USB_PID 0x4011

/*
 * One HID interface with three application
 * collections and separate report IDs.
 *
 * Keyboard report: ID + 8 bytes
 * Mouse report:    ID + 4 bytes
 * Media report:    ID + 2 bytes
 */
static const uint8_t hid_report_desc[] = {

    0x05, 0x01,        /* Generic Desktop */
    0x09, 0x06,        /* Keyboard */
    0xA1, 0x01,        /* Application */
    0x85, HID_ID_KEYBOARD,

    0x05, 0x07,        /* Keyboard usage page */
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,        /* Modifier keys */

    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x03,        /* Reserved */

    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x15, 0x00,
    0x25, 0x65,
    0x75, 0x08,
    0x95, 0x06,
    0x81, 0x00,        /* Six key slots */
    0xC0,

    /* Mouse */
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, HID_ID_MOUSE,

    0x09, 0x01,
    0xA1, 0x00,

    0x05, 0x09,        /* Buttons */
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,

    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,        /* Padding */

    0x05, 0x01,
    0x09, 0x30,        /* X */
    0x09, 0x31,        /* Y */
    0x09, 0x38,        /* Wheel */
    0x15, 0x81,        /* -127 */
    0x25, 0x7F,        /* +127 */
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,
    0xC0,
    0xC0,

    /* Consumer / media control */
    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, HID_ID_MEDIA,

    0x15, 0x00,
    0x26, 0xFF, 0x03,
    0x19, 0x00,
    0x2A, 0xFF, 0x03,
    0x75, 0x10,
    0x95, 0x01,
    0x81, 0x00,
    0xC0
};

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

static void hid_iface_ready(
    const struct device *dev,
    bool ready)
{
    ARG_UNUSED(dev);

    atomic_set(&hid_ready, ready ? 1 : 0);

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

    if (type != HID_REPORT_TYPE_INPUT) {
        return -ENOTSUP;
    }

    size_t size;

    switch (id) {
    case HID_ID_KEYBOARD:
        size = 9;
        break;
    case HID_ID_MOUSE:
        size = 5;
        break;
    case HID_ID_MEDIA:
        size = 3;
        break;
    default:
        return -ENOTSUP;
    }

    if (len < size) {
        return -ENOBUFS;
    }

    memset(buf, 0, size);
    buf[0] = id;

    return (int)size;
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

static const struct hid_device_ops hid_ops = {
    .iface_ready = hid_iface_ready,
    .get_report = hid_get_report,
    .set_idle = hid_set_idle,
    .get_idle = hid_get_idle,
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

int usb_hid_init(void)
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

    /* Register USB string descriptors. */
    err = usbd_add_descriptor(&myo_usbd, &myo_lang);
    if (err) return err;

    err = usbd_add_descriptor(&myo_usbd, &myo_mfr);
    if (err) return err;

    err = usbd_add_descriptor(&myo_usbd, &myo_product);
    if (err) return err;

    /* Register full-speed configuration. */
    err = usbd_add_configuration(
        &myo_usbd,
        USBD_SPEED_FS,
        &myo_fs_cfg
    );
    if (err) return err;

    /* Register only our HID class instance. */
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

    /* Some boards cannot detect VBUS directly. */
    if (!usbd_can_detect_vbus(&myo_usbd)) {
        err = usbd_enable(&myo_usbd);
        if (err) return err;
    }

    LOG_INF("USB HID initialized");
    return 0;
}
