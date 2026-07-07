#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up Wi-Fi station mode and blocks until DHCP has assigned an IP
 * (or the connection attempt gives up after internal retries). Works
 * unmodified whether the radio is local or, as on this board, reached over
 * SDIO via esp_wifi_remote - that's transparent below the esp_wifi API. */
esp_err_t wifi_connect_start(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
