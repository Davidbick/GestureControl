#ifndef MYO_OLED_H
#define MYO_OLED_H

#include <stdbool.h>

int oled_init(void);
int oled_show(bool connected, bool valid, bool fresh);

#endif
