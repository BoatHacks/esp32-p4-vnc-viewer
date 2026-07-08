#include "ota_update.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_update";

#define GITHUB_API_HOST "https://api.github.com"
#define HTTP_RESPONSE_MAX_BYTES (16 * 1024) /* release metadata JSON is a few KB; generous cap */

/* --- fetch the raw JSON body of a GET request ----------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} response_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_buf_t *rb = (response_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        if (rb->len + evt->data_len + 1 > rb->cap) {
            ESP_LOGW(TAG, "Response body exceeds %d byte cap, truncating", HTTP_RESPONSE_MAX_BYTES);
            return ESP_OK;
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        rb->buf[rb->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_get_json(const char *url, char **out_json)
{
    response_buf_t rb = {0};
    rb.cap = HTTP_RESPONSE_MAX_BYTES;
    rb.buf = malloc(rb.cap);
    if (!rb.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .timeout_ms = 10000,
        .buffer_size = 2048, /* GitHub's API responses carry a fair number of headers (rate-limit info, Link, etc.) */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    /* GitHub's API rejects requests with no User-Agent, and returns a
     * more useful response body with an explicit Accept header. */
    esp_http_client_set_header(client, "User-Agent", "esp32-p4-vnc-viewer-ota");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        free(rb.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "GET %s returned HTTP %d", url, status);
        free(rb.buf);
        return ESP_FAIL;
    }

    *out_json = rb.buf;
    return ESP_OK;
}

/* --- semver compare, expecting "vX.Y.Z" (leading 'v' optional) ------------ */

static void parse_semver(const char *s, int *major, int *minor, int *patch)
{
    if (s && (s[0] == 'v' || s[0] == 'V')) s++;
    *major = 0; *minor = 0; *patch = 0;
    if (s) sscanf(s, "%d.%d.%d", major, minor, patch);
}

/* Returns >0 if `a` is newer than `b`, 0 if equal, <0 if `a` is older. */
static int compare_semver(const char *a, const char *b)
{
    int a_maj, a_min, a_pat, b_maj, b_min, b_pat;
    parse_semver(a, &a_maj, &a_min, &a_pat);
    parse_semver(b, &b_maj, &b_min, &b_pat);
    if (a_maj != b_maj) return a_maj - b_maj;
    if (a_min != b_min) return a_min - b_min;
    return a_pat - b_pat;
}

/* --- the actual check-and-update flow -------------------------------------- */

esp_err_t ota_check_and_update(const char *owner, const char *repo, const char *asset_name)
{
    char url[160];
    snprintf(url, sizeof(url), GITHUB_API_HOST "/repos/%s/%s/releases/latest", owner, repo);

    char *json_text = NULL;
    esp_err_t err = http_get_json(url, &json_text);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(json_text);
    free(json_text);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse release JSON");
        return ESP_FAIL;
    }

    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag) || !tag->valuestring) {
        ESP_LOGE(TAG, "Release response had no tag_name");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    ESP_LOGI(TAG, "Running version %s, latest release is %s", running->version, tag->valuestring);

    if (compare_semver(tag->valuestring, running->version) <= 0) {
        ESP_LOGI(TAG, "Already up to date");
        cJSON_Delete(root);
        return ESP_OK;
    }

    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    const cJSON *asset;
    char download_url[256] = {0};
    cJSON_ArrayForEach(asset, assets) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        if (cJSON_IsString(name) && name->valuestring && strcmp(name->valuestring, asset_name) == 0
            && cJSON_IsString(url_item) && url_item->valuestring) {
            strlcpy(download_url, url_item->valuestring, sizeof(download_url));
            break;
        }
    }
    cJSON_Delete(root);

    if (download_url[0] == '\0') {
        ESP_LOGW(TAG, "Release %s has no asset named '%s' - nothing to flash "
                      "(did the release actually get a firmware binary attached?)",
                 tag->valuestring, asset_name);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Downloading and flashing %s", download_url);
    /* GitHub's release download URL (github.com/.../releases/download/...)
     * 302-redirects to the actual asset host with a long signed URL in
     * the Location header - esp_http_client's default header buffer
     * (a few hundred bytes) isn't big enough to hold it, which is what
     * caused a real "HTTP_CLIENT: Out of buffer" failure here. */
    esp_http_client_config_t http_cfg = {
        .url = download_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA succeeded, rebooting into %s", tag->valuestring);
    esp_restart();
    return ESP_OK; /* unreachable */
}

/* --- periodic background check --------------------------------------------- */

typedef struct {
    char owner[64];
    char repo[64];
    char asset_name[64];
    uint32_t interval_hours;
} periodic_ctx_t;

static void periodic_task(void *arg)
{
    periodic_ctx_t *ctx = (periodic_ctx_t *)arg;

    /* Give Wi-Fi (and, on first boot, the setup dialogs) a chance to
     * settle before the first check. */
    vTaskDelay(pdMS_TO_TICKS(30000));

    while (1) {
        esp_err_t err = ota_check_and_update(ctx->owner, ctx->repo, ctx->asset_name);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Periodic OTA check failed: %s (will retry next interval)", esp_err_to_name(err));
        }
        uint32_t interval_ms = ctx->interval_hours * 3600u * 1000u;
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

esp_err_t ota_start_periodic_check(const char *owner, const char *repo, const char *asset_name,
                                    uint32_t interval_hours)
{
    periodic_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    strlcpy(ctx->owner, owner, sizeof(ctx->owner));
    strlcpy(ctx->repo, repo, sizeof(ctx->repo));
    strlcpy(ctx->asset_name, asset_name, sizeof(ctx->asset_name));
    ctx->interval_hours = interval_hours;

    /* 8KB previously here wasn't enough: a real device crashed with a
     * Load access fault (classic stack-corruption symptom) right as it
     * started downloading an update. This task does two TLS handshakes
     * back to back (the GitHub API call, then esp_https_ota's own HTTPS
     * download) plus cJSON parsing - all stack-hungry, especially the
     * mbedTLS handshakes. 16KB internal RAM is a small, one-time cost
     * given hundreds of KB free, and this task runs rarely. */
    BaseType_t ok = xTaskCreate(periodic_task, "ota_check", 16384, ctx, 4, NULL);
    if (ok != pdPASS) {
        free(ctx);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* --- rollback confirmation --------------------------------------------------- */

void ota_mark_running_app_valid(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking this OTA image as valid (cancelling any pending rollback)");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
