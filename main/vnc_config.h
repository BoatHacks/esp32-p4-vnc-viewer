#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VNC_CONFIG_HOST_MAX_LEN 63
#define VNC_CONFIG_PASS_MAX_LEN 64

/* Loads the last-saved VNC server host/port/password from NVS. Returns
 * false (buffers/port left untouched) if nothing has been saved yet. */
bool vnc_config_load(char *host_out, size_t host_out_len, uint16_t *port_out,
                      char *pass_out, size_t pass_out_len);

/* Persists host/port/password to NVS, overwriting whatever was there before. */
esp_err_t vnc_config_save(const char *host, uint16_t port, const char *password);

#ifdef __cplusplus
}
#endif
