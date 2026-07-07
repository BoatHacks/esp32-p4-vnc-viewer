#include "vnc_config.h"

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "vnc_config";
static const char *NVS_NAMESPACE = "vnc_cfg";
static const char *KEY_HOST = "host";
static const char *KEY_PORT = "port";
static const char *KEY_PASS = "pass";

bool vnc_config_load(char *host_out, size_t host_out_len, uint16_t *port_out,
                      char *pass_out, size_t pass_out_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t host_len = host_out_len;
    esp_err_t host_err = nvs_get_str(h, KEY_HOST, host_out, &host_len);

    uint16_t port = 5900;
    esp_err_t port_err = nvs_get_u16(h, KEY_PORT, &port);

    size_t pass_len = pass_out_len;
    esp_err_t pass_err = nvs_get_str(h, KEY_PASS, pass_out, &pass_len);

    nvs_close(h);

    if (host_err != ESP_OK || host_out[0] == '\0') return false;
    *port_out = (port_err == ESP_OK) ? port : 5900;
    if (pass_err != ESP_OK) pass_out[0] = '\0'; /* missing password = server allows "None" auth */

    return true;
}

esp_err_t vnc_config_save(const char *host, uint16_t port, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, KEY_HOST, host ? host : "");
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_PORT, port);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_PASS, password ? password : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved VNC server %s:%u", host, port);
    }
    return err;
}
