#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "rfb_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_handle_t panel;   /* already reset/init'd/on by caller     */
    esp_lcd_touch_handle_t touch;   /* already init'd by caller              */
    uint16_t panel_width;
    uint16_t panel_height;
} vnc_display_cfg_t;

/* Allocates the PSRAM shadow framebuffer (panel_width * panel_height * 2
 * bytes) and returns a set of rfb_client_t callbacks wired to it. Pass the
 * returned struct straight into rfb_client_create(). */
esp_err_t vnc_display_init(const vnc_display_cfg_t *cfg, rfb_callbacks_t *out_callbacks);

/* Starts a task that polls the touch controller and forwards taps/drags to
 * `client` as RFB PointerEvents, scaled from panel coordinates into the
 * remote desktop's coordinate space (set once on_connected fires). */
esp_err_t vnc_display_start_touch_task(rfb_client_t *client);

/* Called from your on_connected callback (or after it) once you know the
 * remote desktop's real resolution, so touch coordinates can be scaled
 * correctly even when it doesn't match the panel's 1024x600. */
void vnc_display_set_remote_size(uint16_t width, uint16_t height);

#ifdef __cplusplus
}
#endif
