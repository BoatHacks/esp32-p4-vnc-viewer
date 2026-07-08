#pragma once

#include "esp_err.h"
#include "rfb_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Passed straight through to ota_check_and_update() by the `ota` command. */
    const char *ota_owner;
    const char *ota_repo;
    const char *ota_asset_name;

    /* If non-NULL, the `vnc` command disconnects this after saving new
     * config, so vnc_task's own reconnect loop picks the new settings up
     * immediately instead of waiting for the current session to drop on
     * its own. May be NULL if called before the client exists yet - the
     * command still saves the config either way. */
    rfb_client_t *vnc_client;
} serial_cli_cfg_t;

/* Starts a REPL (read-eval-print loop) on the console UART - the same
 * one carrying ESP_LOG output, so no extra wiring is needed beyond
 * what's already used to monitor the device. Registers these commands:
 *
 *   wifi <ssid> <password>              - connect to and save a Wi-Fi network
 *   vnc <host> <port> [user] [pass]      - set and save the VNC server to use
 *   ota                                   - check for and install an update now
 *   reboot                                - restart the device
 *   help                                  - list commands (built into esp_console)
 *
 * Runs its own internal task; this function returns once the REPL task
 * is started, it doesn't block. */
esp_err_t serial_cli_start(const serial_cli_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
