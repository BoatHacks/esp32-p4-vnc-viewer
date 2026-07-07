#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CREDS_SSID_MAX_LEN 32
#define WIFI_CREDS_PASS_MAX_LEN 64

/* Loads the last-saved SSID/password from NVS into the given buffers.
 * Returns false (buffers left untouched) if nothing has been saved yet. */
bool wifi_creds_load(char *ssid_out, size_t ssid_out_len, char *pass_out, size_t pass_out_len);

/* Persists SSID/password to NVS, overwriting whatever was there before. */
esp_err_t wifi_creds_save(const char *ssid, const char *password);

/* Wipes any saved credentials - useful if you add a "forget network" option. */
esp_err_t wifi_creds_clear(void);

#ifdef __cplusplus
}
#endif
