#pragma once

#include "esp_err.h"
#include "rfb_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Full-screen LVGL dialog: text fields for host, port, and password, an
 * on-screen keyboard, and a Connect button that attempts the real RFB
 * handshake on `client` (the same instance the caller will go on to run).
 *
 * Blocks until the connection actually succeeds - there's no "cancel",
 * mirroring wifi_setup_ui_run()'s reasoning: this device doesn't have a
 * meaningful offline mode. On success, `client` is left already connected
 * and handshaken (ready for rfb_client_run()), and the working host/port/
 * password have been saved via vnc_config_save().
 *
 * Must be called from a task that is *not* LVGL's own timer-handler task
 * (it takes the LVGL lock itself around every LVGL call).
 */
esp_err_t vnc_setup_ui_run(rfb_client_t *client);

#ifdef __cplusplus
}
#endif
