#include "wifi_creds.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "wifi_creds";
static const char *NVS_NAMESPACE = "wifi_cfg";
static const char *KEY_SSID = "ssid";
static const char *KEY_PASS = "pass";

bool wifi_creds_load(char *ssid_out, size_t ssid_out_len, char *pass_out, size_t pass_out_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t ssid_len = ssid_out_len;
    size_t pass_len = pass_out_len;
    esp_err_t ssid_err = nvs_get_str(h, KEY_SSID, ssid_out, &ssid_len);
    esp_err_t pass_err = nvs_get_str(h, KEY_PASS, pass_out, &pass_len);
    nvs_close(h);

    if (ssid_err != ESP_OK || ssid_out[0] == '\0') return false;
    /* A missing password key just means an open network - that's fine. */
    if (pass_err != ESP_OK) pass_out[0] = '\0';

    return true;
}

esp_err_t wifi_creds_save(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, KEY_SSID, ssid ? ssid : "");
    if (err == ESP_OK) err = nvs_set_str(h, KEY_PASS, password ? password : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved credentials for SSID '%s'", ssid);
    }
    return err;
}

esp_err_t wifi_creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
