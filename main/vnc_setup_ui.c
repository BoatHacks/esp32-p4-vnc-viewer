#include "vnc_setup_ui.h"
#include "vnc_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Same rationale as wifi_setup_ui.c: esp_lvgl_port's lock is stable across
 * BSP versions, unlike guessing the board's own bsp_display_lock(). */
#include "esp_lvgl_port.h"

static const char *TAG = "vnc_setup_ui";

#define CONNECT_TIMEOUT_MS 8000

typedef struct {
    bool connect_pressed;
    char host[VNC_CONFIG_HOST_MAX_LEN + 1];
    char port_str[8];
    char username[VNC_CONFIG_USER_MAX_LEN + 1];
    char password[VNC_CONFIG_PASS_MAX_LEN + 1];
} ui_evt_t;

static QueueHandle_t s_evt_queue;
static lv_obj_t *s_host_ta;
static lv_obj_t *s_port_ta;
static lv_obj_t *s_user_ta;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_kb;

/* Only the currently-focused textarea should drive the shared keyboard,
 * and the port field wants a numeric layout rather than the full
 * alphabetic one. */
static void textarea_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_keyboard_set_mode(s_kb, ta == s_port_ta ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void connect_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_evt_t evt = {.connect_pressed = true};
    strlcpy(evt.host, lv_textarea_get_text(s_host_ta), sizeof(evt.host));
    strlcpy(evt.port_str, lv_textarea_get_text(s_port_ta), sizeof(evt.port_str));
    strlcpy(evt.username, lv_textarea_get_text(s_user_ta), sizeof(evt.username));
    strlcpy(evt.password, lv_textarea_get_text(s_pass_ta), sizeof(evt.password));
    xQueueSend(s_evt_queue, &evt, 0);
}

static void show_connection_screen(const char *status_line, const char *prev_host,
                                    const char *prev_port, const char *prev_user,
                                    const char *prev_pass)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 16, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Connect to a VNC server");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    if (status_line && status_line[0]) {
        lv_obj_t *status = lv_label_create(scr);
        lv_label_set_text(status, status_line);
        lv_obj_set_style_text_color(status, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_align_to(status, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    }

    /* Row 1: host + port */
    lv_obj_t *host_label = lv_label_create(scr);
    lv_label_set_text(host_label, "Host / IP address");
    lv_obj_align(host_label, LV_ALIGN_TOP_LEFT, 0, 56);

    s_host_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_host_ta, true);
    lv_textarea_set_max_length(s_host_ta, VNC_CONFIG_HOST_MAX_LEN);
    lv_textarea_set_placeholder_text(s_host_ta, "192.168.1.100");
    if (prev_host) lv_textarea_set_text(s_host_ta, prev_host);
    lv_obj_set_width(s_host_ta, lv_pct(45));
    lv_obj_align(s_host_ta, LV_ALIGN_TOP_LEFT, 0, 80);
    lv_obj_add_event_cb(s_host_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *port_label = lv_label_create(scr);
    lv_label_set_text(port_label, "Port");
    lv_obj_align_to(port_label, host_label, LV_ALIGN_OUT_RIGHT_TOP, lv_pct(48), 0);

    s_port_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_port_ta, true);
    lv_textarea_set_max_length(s_port_ta, 5);
    lv_textarea_set_accepted_chars(s_port_ta, "0123456789");
    lv_textarea_set_text(s_port_ta, (prev_port && prev_port[0]) ? prev_port : "5900");
    lv_obj_set_width(s_port_ta, lv_pct(20));
    lv_obj_align_to(s_port_ta, s_host_ta, LV_ALIGN_OUT_RIGHT_TOP, 16, 0);
    lv_obj_add_event_cb(s_port_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

    /* Row 2: username (optional - only some servers, e.g. via VeNCrypt
     * Plain auth, actually use it; leave blank for classic VNC Auth or
     * no-auth servers). */
    lv_obj_t *user_label = lv_label_create(scr);
    lv_label_set_text(user_label, "Username (leave blank if not needed)");
    lv_obj_align_to(user_label, host_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 56);

    s_user_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_user_ta, true);
    lv_textarea_set_max_length(s_user_ta, VNC_CONFIG_USER_MAX_LEN);
    if (prev_user) lv_textarea_set_text(s_user_ta, prev_user);
    lv_obj_set_width(s_user_ta, lv_pct(45));
    lv_obj_align_to(s_user_ta, user_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_add_event_cb(s_user_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

    /* Row 3: password + connect button */
    lv_obj_t *pass_label = lv_label_create(scr);
    lv_label_set_text(pass_label, "Password (leave blank if none)");
    lv_obj_align_to(pass_label, user_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 56);

    s_pass_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_max_length(s_pass_ta, VNC_CONFIG_PASS_MAX_LEN);
    if (prev_pass) lv_textarea_set_text(s_pass_ta, prev_pass);
    lv_obj_set_width(s_pass_ta, lv_pct(45));
    lv_obj_align_to(s_pass_ta, pass_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_add_event_cb(s_pass_ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *connect_btn = lv_btn_create(scr);
    lv_obj_align_to(connect_btn, s_pass_ta, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_add_event_cb(connect_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    s_kb = lv_keyboard_create(scr);
    lv_keyboard_set_textarea(s_kb, s_host_ta);
    lv_obj_set_size(s_kb, lv_pct(100), lv_pct(38));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_scr_load(scr);
}

static void show_connecting_screen(const char *host, uint16_t port)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *label = lv_label_create(scr);
    char text[80];
    snprintf(text, sizeof(text), "Connecting to %s:%u ...", host, port);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align_to(spinner, label, LV_ALIGN_OUT_TOP_MID, 0, -30);
    lv_scr_load(scr);
}

esp_err_t vnc_setup_ui_run(rfb_client_t *client)
{
    s_evt_queue = xQueueCreate(2, sizeof(ui_evt_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;

    char host[VNC_CONFIG_HOST_MAX_LEN + 1] = {0};
    uint16_t port = 5900;
    char username[VNC_CONFIG_USER_MAX_LEN + 1] = {0};
    char password[VNC_CONFIG_PASS_MAX_LEN + 1] = {0};
    bool have_prev = vnc_config_load(host, sizeof(host), &port, username, sizeof(username), password, sizeof(password));
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", have_prev ? port : 5900);

    const char *status = NULL;

    while (1) {
        lvgl_port_lock(0);
        show_connection_screen(status, host, port_str, username, password);
        lvgl_port_unlock();
        status = NULL;

        ui_evt_t evt;
        xQueueReceive(s_evt_queue, &evt, portMAX_DELAY);
        if (!evt.connect_pressed) continue;

        strlcpy(host, evt.host, sizeof(host));
        strlcpy(port_str, evt.port_str, sizeof(port_str));
        strlcpy(username, evt.username, sizeof(username));
        strlcpy(password, evt.password, sizeof(password));

        if (host[0] == '\0') {
            status = "Enter a host or IP address.";
            continue;
        }
        long parsed_port = strtol(port_str, NULL, 10);
        if (parsed_port <= 0 || parsed_port > 65535) {
            status = "Port must be between 1 and 65535.";
            continue;
        }
        port = (uint16_t)parsed_port;

        lvgl_port_lock(0);
        show_connecting_screen(host, port);
        lvgl_port_unlock();

        esp_err_t err = rfb_client_connect(client, host, port, username, password, CONNECT_TIMEOUT_MS);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Connected to %s:%u via setup UI", host, port);
            vnc_config_save(host, port, username, password);
            vQueueDelete(s_evt_queue);
            s_evt_queue = NULL;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Connect to %s:%u failed: %s", host, port, esp_err_to_name(err));
        status = (err == ESP_ERR_TIMEOUT)
                      ? "Couldn't reach that host - check the address and try again."
                      : "Connection failed - check the details and try again.";
    }
}
