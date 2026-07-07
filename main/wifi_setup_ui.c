#include "wifi_setup_ui.h"
#include "wifi_manager.h"
#include "wifi_creds.h"

#include <string.h>
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* lvgl_port_lock()/lvgl_port_unlock() are esp_lvgl_port's standard,
 * version-stable guard around LVGL calls made from a task other than its
 * own internal timer-handler task. */
#include "esp_lvgl_port.h"

static const char *TAG = "wifi_setup_ui";

typedef enum {
    UI_EVT_SELECT_SSID,
    UI_EVT_CONNECT_PRESSED,
    UI_EVT_RESCAN,
} ui_evt_type_t;

typedef struct {
    ui_evt_type_t type;
    int index;                                  /* for UI_EVT_SELECT_SSID   */
    char password[WIFI_CREDS_PASS_MAX_LEN + 1]; /* for UI_EVT_CONNECT_PRESSED */
} ui_evt_t;

static QueueHandle_t s_evt_queue;
static wifi_ap_record_t s_scan_results[WIFI_MANAGER_MAX_SCAN_RESULTS];

/* --- screen: scanning / network list --------------------------------------- */

static void ssid_btn_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    ui_evt_t evt = { .type = UI_EVT_SELECT_SSID, .index = index };
    xQueueSend(s_evt_queue, &evt, 0);
}

static void rescan_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_evt_t evt = { .type = UI_EVT_RESCAN };
    xQueueSend(s_evt_queue, &evt, 0);
}

/* Renders the network list. Caller must already hold the display lock. */
static void show_network_list_screen(const wifi_ap_record_t *aps, uint16_t n, const char *status_line)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 16, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Select a Wi-Fi network");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    if (status_line && status_line[0]) {
        lv_obj_t *status = lv_label_create(scr);
        lv_label_set_text(status, status_line);
        lv_obj_set_style_text_color(status, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_align_to(status, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    }

    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, lv_pct(100), lv_pct(78));
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -56);

    for (uint16_t i = 0; i < n; i++) {
        char label[48];
        int rssi_bars = aps[i].rssi > -55 ? 3 : (aps[i].rssi > -75 ? 2 : 1);
        const char *lock = (aps[i].authmode == WIFI_AUTH_OPEN) ? "" : " " LV_SYMBOL_CLOSE " ";
        snprintf(label, sizeof(label), "%s%s", aps[i].ssid, lock);
        lv_obj_t *btn = lv_list_add_btn(list, rssi_bars >= 3 ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING, label);
        lv_obj_add_event_cb(btn, ssid_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (n == 0) {
        lv_list_add_text(list, "No networks found nearby.");
    }

    lv_obj_t *rescan_btn = lv_btn_create(scr);
    lv_obj_align(rescan_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *rescan_label = lv_label_create(rescan_btn);
    lv_label_set_text(rescan_label, LV_SYMBOL_REFRESH " Scan again");
    lv_obj_add_event_cb(rescan_btn, rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_scr_load(scr);
}

/* --- screen: password entry ------------------------------------------------- */

static lv_obj_t *s_pw_textarea;

static void connect_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_evt_t evt = { .type = UI_EVT_CONNECT_PRESSED };
    const char *text = lv_textarea_get_text(s_pw_textarea);
    strlcpy(evt.password, text, sizeof(evt.password));
    xQueueSend(s_evt_queue, &evt, 0);
}

static void show_password_screen(const char *ssid, const char *status_line)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 16, 0);

    lv_obj_t *title = lv_label_create(scr);
    char title_text[64];
    snprintf(title_text, sizeof(title_text), "Password for \"%s\"", ssid);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    if (status_line && status_line[0]) {
        lv_obj_t *status = lv_label_create(scr);
        lv_label_set_text(status, status_line);
        lv_obj_set_style_text_color(status, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_align_to(status, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    }

    s_pw_textarea = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_pw_textarea, true);
    lv_textarea_set_password_mode(s_pw_textarea, true);
    lv_textarea_set_placeholder_text(s_pw_textarea, "Password");
    lv_obj_set_width(s_pw_textarea, lv_pct(70));
    lv_obj_align(s_pw_textarea, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *connect_btn = lv_btn_create(scr);
    lv_obj_align_to(connect_btn, s_pw_textarea, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_add_event_cb(connect_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_keyboard_set_textarea(kb, s_pw_textarea);
    lv_obj_set_size(kb, lv_pct(100), lv_pct(55));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_scr_load(scr);
}

/* --- screen: transient "connecting..." status ------------------------------- */

static void show_connecting_screen(const char *ssid)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *label = lv_label_create(scr);
    char text[64];
    snprintf(text, sizeof(text), "Connecting to \"%s\" ...", ssid);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align_to(spinner, label, LV_ALIGN_OUT_TOP_MID, 0, -30);
    lv_scr_load(scr);
}

/* --- main state machine ---------------------------------------------------- */

esp_err_t wifi_setup_ui_run(void)
{
    s_evt_queue = xQueueCreate(4, sizeof(ui_evt_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;

    const char *list_status = NULL;

    while (1) {
        lvgl_port_lock(0);
        uint16_t n = wifi_manager_scan(s_scan_results, WIFI_MANAGER_MAX_SCAN_RESULTS);
        show_network_list_screen(s_scan_results, n, list_status);
        lvgl_port_unlock();
        list_status = NULL;

        ui_evt_t evt;
        xQueueReceive(s_evt_queue, &evt, portMAX_DELAY);
        if (evt.type == UI_EVT_RESCAN) continue;
        if (evt.type != UI_EVT_SELECT_SSID) continue;

        wifi_ap_record_t chosen = s_scan_results[evt.index];
        bool needs_password = (chosen.authmode != WIFI_AUTH_OPEN);
        char password[WIFI_CREDS_PASS_MAX_LEN + 1] = {0};

        if (needs_password) {
            lvgl_port_lock(0);
            show_password_screen((const char *)chosen.ssid, NULL);
            lvgl_port_unlock();

            xQueueReceive(s_evt_queue, &evt, portMAX_DELAY);
            if (evt.type != UI_EVT_CONNECT_PRESSED) continue;
            strlcpy(password, evt.password, sizeof(password));
        }

        lvgl_port_lock(0);
        show_connecting_screen((const char *)chosen.ssid);
        lvgl_port_unlock();

        esp_err_t err = wifi_manager_connect((const char *)chosen.ssid, password, 15000);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Connected via setup UI");
            vQueueDelete(s_evt_queue);
            s_evt_queue = NULL;
            /* Deliberately leave the "Connecting..." screen up - the
             * caller (main.c) hands the panel back to the raw VNC blit
             * path immediately, which will overwrite it with the first
             * framebuffer update. */
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Connect to '%s' failed: %s", chosen.ssid, esp_err_to_name(err));
        list_status = "Couldn't connect - check the password and try again.";
        /* Loop back around to the (re-scanned) network list. */
    }
}
