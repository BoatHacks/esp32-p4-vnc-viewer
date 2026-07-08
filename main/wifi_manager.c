#include "wifi_manager.h"
#include "wifi_creds.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_manager";

#define CONNECTED_BIT   BIT0
#define FAIL_BIT        BIT1
#define MAX_RETRY_PER_ATTEMPT 5

static EventGroupHandle_t s_event_group;
static int s_retry_count;
static bool s_ever_connected;   /* have we successfully connected at least once? */
static bool s_attempting;       /* is a connect() call currently in flight?      */
static bool s_have_config;      /* has esp_wifi_set_config() been called with a real SSID? */

static wifi_manager_lost_cb_t s_lost_cb;
static void *s_lost_cb_ctx;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Real hardware log: this used to unconditionally call
         * esp_wifi_connect() here, including on first boot before any
         * SSID was ever configured - that produced a connect attempt
         * with an empty SSID racing against the setup UI's own scan,
         * surfacing as "esp_wifi_scan_start failed: ESP_ERR_WIFI_STATE"
         * and a pointless "disconnected, retrying" cycle. Only auto-
         * connect once we actually have real credentials configured. */
        if (s_have_config) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, CONNECTED_BIT);

        if (s_retry_count < MAX_RETRY_PER_ATTEMPT) {
            s_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/%d)", s_retry_count, MAX_RETRY_PER_ATTEMPT);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi retries exhausted");
            bool was_established = s_ever_connected;
            xEventGroupSetBits(s_event_group, FAIL_BIT);
            /* Only fire the "lost" callback for a connection that had
             * been working before - the very first attempt's failure is
             * just reported back via the blocking connect() return value. */
            if (was_established && !s_attempting && s_lost_cb) {
                s_lost_cb(s_lost_cb_ctx);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_ever_connected = true;
        xEventGroupClearBits(s_event_group, FAIL_BIT);
        xEventGroupSetBits(s_event_group, CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static esp_err_t connect_and_wait(const char *ssid, const char *password, uint32_t timeout_ms)
{
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password) strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    /* WIFI_AUTH_OPEN as a threshold accepts open networks too; the driver
     * still negotiates whatever the AP actually requires above this floor. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    s_retry_count = 0;
    s_attempting = true;
    xEventGroupClearBits(s_event_group, CONNECTED_BIT | FAIL_BIT);

    esp_wifi_disconnect(); /* no-op if not currently connected */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_have_config = true;
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
        s_attempting = false;
        return connect_err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group, CONNECTED_BIT | FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s_attempting = false;

    if (bits & CONNECTED_BIT) return ESP_OK;
    return ESP_FAIL;
}

esp_err_t wifi_manager_connect_saved(uint32_t timeout_ms)
{
    char ssid[WIFI_CREDS_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_CREDS_PASS_MAX_LEN + 1] = {0};
    if (!wifi_creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Trying saved network '%s'", ssid);
    return connect_and_wait(ssid, pass, timeout_ms);
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "Connecting to '%s' ...", ssid);
    esp_err_t err = connect_and_wait(ssid, password, timeout_ms);
    if (err == ESP_OK) {
        wifi_creds_save(ssid, password);
    }
    return err;
}

esp_err_t wifi_manager_wait_connected(TickType_t ticks_to_wait)
{
    EventBits_t bits = xEventGroupWaitBits(s_event_group, CONNECTED_BIT, pdFALSE, pdFALSE, ticks_to_wait);
    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint16_t wifi_manager_scan(wifi_ap_record_t *out_records, uint16_t max_records)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block until done */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t count = max_records;
    err = esp_wifi_scan_get_ap_records(&count, out_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        return 0;
    }
    return count;
}

void wifi_manager_set_lost_callback(wifi_manager_lost_cb_t cb, void *ctx)
{
    s_lost_cb = cb;
    s_lost_cb_ctx = ctx;
}
