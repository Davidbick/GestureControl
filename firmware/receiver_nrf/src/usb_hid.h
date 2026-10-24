
#ifndef MYO_USB_HID_H
#define MYO_USB_HID_H

#include <stdbool.h>

enum myo_hid_action {
    MYO_HID_SCROLL_UP,
    MYO_HID_SCROLL_DOWN,
    MYO_HID_PREVIOUS_TAB,
    MYO_HID_NEXT_TAB,
    MYO_HID_DOUBLE_CLICK,
    MYO_HID_VOLUME_UP,
    MYO_HID_VOLUME_DOWN
};

int myo_usb_hid_init(void);

bool usb_hid_ready(void);

int myo_usb_hid_enqueue(enum myo_hid_action action);

#endif
