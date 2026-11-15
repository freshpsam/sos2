#ifndef SOS2_WIFI_ASSERT_H
#define SOS2_WIFI_ASSERT_H
#include "wifi/port.h"
#define assert(expression) ((expression) ? (void)0 : wifi_os_abort())
#endif
