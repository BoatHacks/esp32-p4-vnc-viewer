#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called (from the Wi-Fi/event task's context - keep it fast, e.g. just
 * notify another task) when a connection that had previously succeeded
 * drops and automatic retries are exhausted. Not called for the very
 * first connection attempt at startup - that failure is just the return
 * value of wifi_manager_connect_saved()/wifi_manager_connect(). */
typedef void (*wifi_manager_lost_cb_t)(void *ctx);

#define WIFI_MANAGER_MAX_SCAN_RESULTS 24

/* Brings up the Wi-Fi/network stack in station mode. Does not connect to
 * anything yet - call wifi_manager_connect_saved() or wifi_manager_connect()
 * next. Safe to call once at startup, before any display/VNC setup. */
esp_err_t wifi_manager_init(void);

/* Loads SSID/password from NVS (see wifi_creds.h) and attempts to connect,
 * waiting up to timeout_ms. Returns ESP_ERR_NOT_FOUND if nothing has been
 * saved yet, ESP_FAIL if the saved credentials don't work anymore. */
esp_err_t wifi_manager_connect_saved(uint32_t timeout_ms);

/* Attempts to connect with the given credentials (password may be "" or
 * NULL for an open network), waiting up to timeout_ms. On success, saves
 * the credentials to NVS so they become the new "saved" network. */
esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/* Blocks the calling task until Wi-Fi is connected, or forever if
 * ticks_to_wait is portMAX_DELAY. Use this to gate network operations
 * (like the VNC socket) so they naturally pause during an outage instead
 * of spinning. */
esp_err_t wifi_manager_wait_connected(TickType_t ticks_to_wait);

/* Blocking active scan. Copies up to max_records results (already sorted
 * by descending RSSI by the driver) into out_records and returns how many
 * were found. */
uint16_t wifi_manager_scan(wifi_ap_record_t *out_records, uint16_t max_records);

/* Registers the "connection lost" callback described above. Only one
 * callback is supported; calling again replaces the previous one. */
void wifi_manager_set_lost_callback(wifi_manager_lost_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
