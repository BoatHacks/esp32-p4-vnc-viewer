#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VNC_CONFIG_HOST_MAX_LEN 63
#define VNC_CONFIG_USER_MAX_LEN 63
#define VNC_CONFIG_PASS_MAX_LEN 64

/* Loads the last-saved VNC server host/port/username/password from NVS.
 * Returns false (buffers/port left untouched) if nothing has been saved
 * yet. user_out is set to "" if a host was saved without a username
 * (i.e. it was never needed - None or classic VNC Auth). */
bool vnc_config_load(char *host_out, size_t host_out_len, uint16_t *port_out,
                      char *user_out, size_t user_out_len,
                      char *pass_out, size_t pass_out_len);

/* Persists host/port/username/password to NVS, overwriting whatever was
 * there before. username may be NULL/empty if the server doesn't need one. */
esp_err_t vnc_config_save(const char *host, uint16_t port, const char *username, const char *password);

#ifdef __cplusplus
}
#endif
