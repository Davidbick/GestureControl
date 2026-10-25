#ifndef MYO_USB_HID_H
#define MYO_USB_HID_H

#include <stdbool.h>
#include <stdint.h>
#include "gesture_config.h"

int myo_usb_hid_init(void);
bool usb_hid_ready(void);
int myo_usb_hid_enqueue(uint8_t gesture, const struct gc_mapping *mapping);
void myo_usb_hid_release_all(void);

#endif
