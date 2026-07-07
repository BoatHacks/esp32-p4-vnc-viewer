#include "vnc_display.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vnc_display";

/* Single global instance: this device drives exactly one VNC session onto
 * its one built-in panel, so a module-level singleton keeps the rfb_client
 * C-callback signatures simple (no closures in C). If you ever want more
 * than one simultaneous session, thread a context pointer through instead. */
static struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;
    uint16_t panel_w, panel_h;
    uint16_t remote_w, remote_h;
    uint16_t *fb;      /* shadow copy, RGB565, panel_w * panel_h            */
} s;

/* --- rfb_client callbacks -------------------------------------------------- */

static void on_connected(uint16_t width, uint16_t height, const char *name, void *ctx)
{
    (void)ctx;
    s.remote_w = width;
    s.remote_h = height;
    ESP_LOGI(TAG, "Connected to '%s' (%ux%u); panel is %ux%u%s", name, width, height,
             s.panel_w, s.panel_h,
             (width != s.panel_w || height != s.panel_h)
                 ? " - sizes differ, clipping to top-left (see README for scaling)"
                 : "");
    memset(s.fb, 0, (size_t)s.panel_w * s.panel_h * 2);
}

static inline bool rect_visible(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    return x < s.panel_w && y < s.panel_h && w > 0 && h > 0;
}

static void on_raw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint8_t *pixels, void *ctx)
{
    (void)ctx;
    if (!rect_visible(x, y, w, h)) return;

    /* Clip to panel bounds (remote desktop may be larger than our screen). */
    uint16_t draw_w = (x + w > s.panel_w) ? (s.panel_w - x) : w;
    if (draw_w == 0) return;

    const uint16_t *src = (const uint16_t *)pixels;
    uint16_t *dst_row = s.fb + (size_t)y * s.panel_w + x;
    memcpy(dst_row, src, (size_t)draw_w * 2);

    /* h is always 1 here (rfb_client feeds us row-by-row to bound its own
     * scratch buffer size) - push straight to the panel. */
    esp_lcd_panel_draw_bitmap(s.panel, x, y, x + draw_w, y + 1, dst_row);
}

static void on_copy_rect(uint16_t dst_x, uint16_t dst_y, uint16_t w, uint16_t h,
                          uint16_t src_x, uint16_t src_y, void *ctx)
{
    (void)ctx;
    if (!rect_visible(dst_x, dst_y, w, h) || !rect_visible(src_x, src_y, w, h)) return;

    uint16_t copy_w = w;
    if (dst_x + copy_w > s.panel_w) copy_w = s.panel_w - dst_x;
    if (src_x + copy_w > s.panel_w) copy_w = s.panel_w - src_x;

    /* Rows may overlap (e.g. scrolling a window down by a few pixels), so
     * pick a copy direction that doesn't clobber source rows before we've
     * read them. */
    bool forward = dst_y <= src_y;
    for (uint16_t i = 0; i < h; i++) {
        uint16_t row = forward ? i : (h - 1 - i);
        if (dst_y + row >= s.panel_h || src_y + row >= s.panel_h) continue;
        uint16_t *dst_row = s.fb + (size_t)(dst_y + row) * s.panel_w + dst_x;
        const uint16_t *src_row = s.fb + (size_t)(src_y + row) * s.panel_w + src_x;
        memmove(dst_row, src_row, (size_t)copy_w * 2);
    }
    /* Re-blit the whole affected destination block in one go. */
    for (uint16_t row = 0; row < h; row++) {
        if (dst_y + row >= s.panel_h) break;
        uint16_t *dst_row = s.fb + (size_t)(dst_y + row) * s.panel_w + dst_x;
        esp_lcd_panel_draw_bitmap(s.panel, dst_x, dst_y + row, dst_x + copy_w, dst_y + row + 1, dst_row);
    }
}

static void on_disconnected(esp_err_t reason, void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "Disconnected (reason: %s)", esp_err_to_name(reason));
}

esp_err_t vnc_display_init(const vnc_display_cfg_t *cfg, rfb_callbacks_t *out_callbacks)
{
    if (!cfg || !cfg->panel || !out_callbacks) return ESP_ERR_INVALID_ARG;

    memset(&s, 0, sizeof(s));
    s.panel = cfg->panel;
    s.touch = cfg->touch;
    s.panel_w = cfg->panel_width;
    s.panel_h = cfg->panel_height;

    size_t fb_bytes = (size_t)s.panel_w * s.panel_h * 2;
    s.fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.fb) {
        ESP_LOGE(TAG, "Failed to allocate %u byte shadow framebuffer in PSRAM", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }

    out_callbacks->on_connected = on_connected;
    out_callbacks->on_raw_rect = on_raw_rect;
    out_callbacks->on_copy_rect = on_copy_rect;
    out_callbacks->on_disconnected = on_disconnected;
    out_callbacks->ctx = NULL;
    return ESP_OK;
}

void vnc_display_set_remote_size(uint16_t width, uint16_t height)
{
    s.remote_w = width;
    s.remote_h = height;
}

/* --- touch -> PointerEvent task -------------------------------------------- */

static rfb_client_t *s_touch_client;
static volatile bool s_touch_paused;
static bool s_touch_task_started;

void vnc_display_pause_touch(bool paused)
{
    s_touch_paused = paused;
}

static void touch_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    uint16_t last_rx = 0, last_ry = 0;

    while (1) {
        if (s_touch_paused) {
            was_pressed = false; /* don't send a stale release once resumed */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint16_t tx[1], ty[1];
        uint8_t tcnt = 0;

        esp_lcd_touch_read_data(s.touch);
        bool pressed = esp_lcd_touch_get_coordinates(s.touch, tx, ty, NULL, &tcnt, 1);

        if (pressed && tcnt > 0) {
            /* Scale panel coordinates into the remote desktop's space so
             * taps land correctly even if the resolutions don't match. */
            uint16_t rx = (s.remote_w && s.panel_w) ? (uint32_t)tx[0] * s.remote_w / s.panel_w : tx[0];
            uint16_t ry = (s.remote_h && s.panel_h) ? (uint32_t)ty[0] * s.remote_h / s.panel_h : ty[0];
            rfb_client_send_pointer(s_touch_client, rx, ry, 0x01 /* left button down */);
            last_rx = rx;
            last_ry = ry;
            was_pressed = true;
        } else if (was_pressed) {
            /* Send a final release at the last known (already-scaled) position. */
            rfb_client_send_pointer(s_touch_client, last_rx, last_ry, 0x00 /* all buttons up */);
            was_pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20)); /* ~50Hz poll, plenty for a touch UI */
    }
}

esp_err_t vnc_display_start_touch_task(rfb_client_t *client)
{
    s_touch_client = client; /* update in case rfb_client_t* changed across a reconnect */

    if (s_touch_task_started) return ESP_OK;

    if (!s.touch) {
        ESP_LOGE(TAG, "No touch handle configured (vnc_display_init cfg->touch was NULL) - skipping touch task");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(touch_task, "vnc_touch", 4096, NULL, 5, NULL);
    if (ok == pdPASS) s_touch_task_started = true;
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
