#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Full-screen LVGL dialog: scans for networks, lets the user tap one,
 * shows an on-screen keyboard for the password if the network needs one,
 * attempts to connect, and loops back to the list on failure.
 *
 * Blocks until a connection succeeds (there's deliberately no "cancel" -
 * this device has no other useful offline mode). On success, the working
 * credentials have already been saved to NVS via wifi_manager_connect(),
 * and the screen is torn down before returning.
 *
 * Must be called from a task that is *not* LVGL's own timer-handler task
 * (it takes the LVGL lock itself around every LVGL call, via
 * lvgl_port_lock()/lvgl_port_unlock()).
 */
esp_err_t wifi_setup_ui_run(void);

#ifdef __cplusplus
}
#endif
