#ifndef MYO_USB_HID_H
#define MYO_USB_HID_H

#include <stdbool.h>
#include "gesture_config.h"

int myo_usb_hid_init(void);
bool usb_hid_ready(void);
int myo_usb_hid_enqueue(const struct gc_mapping *mapping);

#endif
