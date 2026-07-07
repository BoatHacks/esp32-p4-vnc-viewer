#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_connect.h"
#include "rfb_client.h"
#include "vnc_display.h"

/* --- board-specific bring-up --------------------------------------------
 *
 * This is the one part of the project that leans on the Waveshare BSP
 * component (waveshare/esp32_p4_wifi6_touch_lcd_7b) and therefore needs to
 * be checked against whatever version idf_component.yml pulled in.
 *
 * The BSP follows Espressif's standard "esp-bsp" pattern used across their
 * boards, so the calls below (bsp_display_new / bsp_display_backlight_on /
 * bsp_touch_new) are the conventional entry points - but component
 * versions do shift their exact signatures. Before your first build:
 *
 *   idf.py add-dependency "waveshare/esp32_p4_wifi6_touch_lcd_7b"
 *   idf.py add-dependency "espressif/esp_wifi_remote"
 *   idf.py add-dependency "espressif/esp_hosted"
 *   idf.py menuconfig
 *     -> Component config -> Board Support Package (ESP32-P4) -> Display
 *        -> select the panel type for this board
 *     -> Component config -> Wi-Fi Remote -> slave target -> esp32-c6
 *
 * then open `managed_components/waveshare__esp32_p4_wifi6_touch_lcd_7b/
 * include/bsp/esp-bsp.h` (or similarly named header) and adjust the two
 * calls in board_display_touch_init() below to match. Everything else in
 * this project (rfb_client.c, vnc_display.c, wifi_connect.c) is
 * BSP-independent and shouldn't need changes.
 */
#include "bsp/esp-bsp.h"

static const char *TAG = "vnc_main";

/* ---- fill these in (or load from NVS / a config file) ------------------- */
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define VNC_HOST        "192.168.1.100"
#define VNC_PORT        5900
#define VNC_PASSWORD    "" /* leave empty if the server allows Security-Type None */

#define PANEL_WIDTH     1024
#define PANEL_HEIGHT    600

static esp_err_t board_display_touch_init(esp_lcd_panel_handle_t *out_panel,
                                           esp_lcd_touch_handle_t *out_touch)
{
    /* Standard esp-bsp two-step: allocate/reset the panel without starting
     * LVGL (we don't want LVGL's object model in the hot path - we're
     * blitting raw VNC rectangles directly), then explicitly turn the
     * backlight on. */
    const bsp_display_config_t disp_cfg = {
        .max_transfer_sz = PANEL_WIDTH * 100 * 2, /* a chunk of rows at a time */
    };
    ESP_ERROR_CHECK(bsp_display_new(&disp_cfg, out_panel, NULL));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*out_panel, true));
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(bsp_touch_new(NULL, out_touch));

    return ESP_OK;
}

/* ---- VNC session lifecycle ----------------------------------------------- */

static void vnc_task(void *arg)
{
    rfb_client_t *client = (rfb_client_t *)arg;

    while (1) {
        ESP_LOGI(TAG, "Connecting to VNC server at %s:%d ...", VNC_HOST, VNC_PORT);
        esp_err_t err = rfb_client_connect(client, VNC_HOST, VNC_PORT, VNC_PASSWORD);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Connect/handshake failed: %s - retrying in 3s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        vnc_display_start_touch_task(client);

        /* Blocks until the connection drops. */
        rfb_client_run(client);

        ESP_LOGW(TAG, "VNC session ended, reconnecting in 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_display_touch_init(&panel, &touch));

    vnc_display_cfg_t disp_cfg = {
        .panel = panel,
        .touch = touch,
        .panel_width = PANEL_WIDTH,
        .panel_height = PANEL_HEIGHT,
    };
    rfb_callbacks_t callbacks = {0};
    ESP_ERROR_CHECK(vnc_display_init(&disp_cfg, &callbacks));

    rfb_client_t *client = rfb_client_create(&callbacks);
    if (!client) {
        ESP_LOGE(TAG, "Out of memory creating rfb_client");
        return;
    }

    ESP_ERROR_CHECK(wifi_connect_start(WIFI_SSID, WIFI_PASSWORD));

    xTaskCreate(vnc_task, "vnc_task", 8192, client, 5, NULL);
}
