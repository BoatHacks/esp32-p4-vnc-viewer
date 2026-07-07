#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_manager.h"
#include "wifi_setup_ui.h"
#include "vnc_config.h"
#include "vnc_setup_ui.h"
#include "rfb_client.h"
#include "vnc_display.h"

#include "esp_lvgl_port.h"

/* --- board-specific bring-up --------------------------------------------
 *
 * This is the one part of the project that leans on the Waveshare BSP
 * component (waveshare/esp32_p4_wifi6_touch_lcd_7b) and therefore needs to
 * be checked against whatever version idf_component.yml pulled in.
 *
 * The BSP follows Espressif's standard "esp-bsp" pattern used across their
 * boards, so bsp_display_new()/bsp_display_backlight_on()/bsp_touch_new()
 * below are the conventional entry points - but component versions do
 * shift their exact signatures. Before your first build:
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
 * include/bsp/esp-bsp.h` and check board_display_touch_init() below
 * against the real signatures. Everything past that point - including how
 * LVGL gets wired up for the Wi-Fi setup screens - uses only the stable,
 * board-independent esp_lvgl_port API, so it shouldn't need changes.
 */
#include "bsp/esp-bsp.h"

static const char *TAG = "vnc_main";

/* ---- fill these in (or load from NVS / a config file) ------------------- */
#define PANEL_WIDTH     1024
#define PANEL_HEIGHT    600

#define SAVED_WIFI_CONNECT_TIMEOUT_MS   15000
#define SAVED_VNC_CONNECT_TIMEOUT_MS    8000
#define VNC_MAX_CONSECUTIVE_FAILURES    3   /* before re-showing the setup dialog */

static esp_err_t board_display_touch_init(esp_lcd_panel_handle_t *out_panel,
                                           esp_lcd_panel_io_handle_t *out_io,
                                           esp_lcd_touch_handle_t *out_touch)
{
    const bsp_display_config_t disp_cfg = {
        .max_transfer_sz = PANEL_WIDTH * 100 * 2, /* a chunk of rows at a time */
    };
    ESP_ERROR_CHECK(bsp_display_new(&disp_cfg, out_panel, out_io));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*out_panel, true));
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(bsp_touch_new(NULL, out_touch));

    return ESP_OK;
}

/* Wraps the already-initialized panel/touch handles in LVGL, purely for
 * the Wi-Fi setup screens - see the comment on esp_lvgl_port above. */
static esp_err_t lvgl_init_for_panel(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io,
                                      esp_lcd_touch_handle_t touch, lv_disp_t **out_disp)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = PANEL_WIDTH * 100,
        .double_buffer = true,
        .hres = PANEL_WIDTH,
        .vres = PANEL_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .swap_bytes = false,
        },
    };
    *out_disp = lvgl_port_add_disp(&disp_cfg);
    if (!*out_disp) return ESP_FAIL;

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = *out_disp,
        .handle = touch,
    };
    if (!lvgl_port_add_touch(&touch_cfg)) return ESP_FAIL;

    return ESP_OK;
}

/* Blanks the LVGL screen so its periodic refresh has nothing left to draw,
 * clearing the way for vnc_display.c's direct esp_lcd_panel_draw_bitmap
 * calls. We don't tear LVGL down entirely - it's cheap to leave idle, and
 * keeping it alive means the Wi-Fi setup screens can reappear instantly if
 * the connection is later lost (see wifi_lost_cb below). */
static void lvgl_yield_to_vnc(void)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_scr_load(scr);
    lvgl_port_unlock();
}

/* ---- Wi-Fi bring-up: saved network, or the on-screen setup dialog -------- */

static void ensure_wifi_connected(void)
{
    esp_err_t err = wifi_manager_connect_saved(SAVED_WIFI_CONNECT_TIMEOUT_MS);
    if (err == ESP_OK) return;

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved Wi-Fi network yet - showing setup screen");
    } else {
        ESP_LOGW(TAG, "Saved Wi-Fi network didn't connect (%s) - showing setup screen", esp_err_to_name(err));
    }
    wifi_setup_ui_run(); /* blocks until connected */
}

/* Called from the Wi-Fi event task when a previously-working connection
 * drops and automatic retries are exhausted. Keep this fast - it just
 * wakes the watchdog task, which does the actual (slow, LVGL-touching)
 * work of re-showing the setup dialog. */
static TaskHandle_t s_wifi_watchdog_task;

static void wifi_lost_cb(void *ctx)
{
    (void)ctx;
    if (s_wifi_watchdog_task) xTaskNotifyGive(s_wifi_watchdog_task);
}

static void wifi_watchdog_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGW(TAG, "Wi-Fi connection lost - pausing VNC and showing setup screen");

        vnc_display_pause_touch(true);
        wifi_setup_ui_run(); /* blocks until reconnected */
        lvgl_yield_to_vnc();
        vnc_display_pause_touch(false);

        ESP_LOGI(TAG, "Wi-Fi restored - VNC will reconnect automatically");
    }
}

/* ---- VNC session lifecycle ----------------------------------------------- */

static void vnc_task(void *arg)
{
    rfb_client_t *client = (rfb_client_t *)arg;
    int consecutive_failures = 0;

    while (1) {
        /* If Wi-Fi is down (initial outage or mid-session loss), wait
         * here instead of spinning on failed connect attempts - the
         * Wi-Fi watchdog task is the one driving recovery via its UI. */
        wifi_manager_wait_connected(portMAX_DELAY);

        char host[VNC_CONFIG_HOST_MAX_LEN + 1] = {0};
        uint16_t port = 0;
        char password[VNC_CONFIG_PASS_MAX_LEN + 1] = {0};
        bool have_cfg = vnc_config_load(host, sizeof(host), &port, password, sizeof(password));

        esp_err_t err = ESP_FAIL;
        if (have_cfg) {
            ESP_LOGI(TAG, "Connecting to VNC server at %s:%u ...", host, port);
            err = rfb_client_connect(client, host, port, password, SAVED_VNC_CONNECT_TIMEOUT_MS);
        }

        if (err != ESP_OK) {
            consecutive_failures++;
            if (!have_cfg) {
                ESP_LOGI(TAG, "No VNC server configured yet - showing setup screen");
            } else {
                ESP_LOGE(TAG, "Connect/handshake to %s:%u failed: %s (%d/%d)",
                         host, port, esp_err_to_name(err), consecutive_failures, VNC_MAX_CONSECUTIVE_FAILURES);
            }

            if (!have_cfg || consecutive_failures >= VNC_MAX_CONSECUTIVE_FAILURES) {
                vnc_display_pause_touch(true);
                vnc_setup_ui_run(client); /* blocks; client is connected on return */
                lvgl_yield_to_vnc();
                vnc_display_pause_touch(false);
                consecutive_failures = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        } else {
            consecutive_failures = 0;
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
    ESP_ERROR_CHECK(wifi_manager_init());

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_display_touch_init(&panel, &io, &touch));

    lv_disp_t *disp = NULL;
    ESP_ERROR_CHECK(lvgl_init_for_panel(panel, io, touch, &disp));

    /* First-boot / saved-network-failed path: this blocks (showing the
     * setup dialog if needed) until Wi-Fi is actually up. */
    ensure_wifi_connected();
    lvgl_yield_to_vnc();

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

    /* Watchdog task must exist before we arm the lost-connection callback. */
    xTaskCreate(wifi_watchdog_task, "wifi_watchdog", 4096, NULL, 6, &s_wifi_watchdog_task);
    wifi_manager_set_lost_callback(wifi_lost_cb, NULL);

    xTaskCreate(vnc_task, "vnc_task", 8192, client, 5, NULL);
}
