
#ifndef MYO_USB_HID_H
#define MYO_USB_HID_H

#include <stdbool.h>

enum myo_hid_action {
    MYO_HID_UP,
    MYO_HID_DOWN,
    MYO_HID_LEFT,
    MYO_HID_RIGHT,
    MYO_HID_ENTER
};

int myo_usb_hid_init(void);

bool usb_hid_ready(void);

int myo_usb_hid_enqueue(enum myo_hid_action action);

#endif
