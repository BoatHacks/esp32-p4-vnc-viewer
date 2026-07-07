#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal RFB (RFC 6143) client engine.
 *
 * Deliberately has zero knowledge of the display/touch hardware - it just
 * owns the TCP socket and the protocol state machine, and calls back into
 * whoever wants the pixels. This keeps it reusable if you ever port it to
 * a different screen or want to unit-test the protocol on a PC.
 *
 * Supported encodings: Raw, CopyRect. That's enough for a correct, if not
 * maximally bandwidth-efficient, client - fine for a LAN link over Wi-Fi 6.
 * Hextile/Tight/zlib are natural follow-ups if you need more headroom over
 * a slower link (see README).
 */

typedef struct {
    /* Called once after the RFB handshake completes and ServerInit has
     * been parsed. width/height are the remote desktop's *native* size in
     * pixels - it will very likely not match your panel's 1024x600, so
     * decide up front whether you want to crop, scale, or scroll. */
    void (*on_connected)(uint16_t width, uint16_t height, const char *desktop_name, void *ctx);

    /* A rectangle of raw RGB565 pixel data has arrived. `pixels` is only
     * valid for the duration of the call - copy it if you need to keep it. */
    void (*on_raw_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const uint8_t *pixels_rgb565, void *ctx);

    /* Server says: copy an existing on-screen rectangle to a new position.
     * You must maintain enough of a framebuffer to satisfy this yourself. */
    void (*on_copy_rect)(uint16_t dst_x, uint16_t dst_y, uint16_t w, uint16_t h,
                          uint16_t src_x, uint16_t src_y, void *ctx);

    /* Socket dropped, auth failed, or rfb_client_disconnect() was called. */
    void (*on_disconnected)(esp_err_t reason, void *ctx);

    void *ctx;
} rfb_callbacks_t;

typedef struct rfb_client rfb_client_t;

/* Allocate a client instance. Does not touch the network yet. */
rfb_client_t *rfb_client_create(const rfb_callbacks_t *callbacks);

void rfb_client_destroy(rfb_client_t *client);

/* Opens the TCP connection and runs the full RFB handshake (protocol
 * version negotiation, security type selection, VNC auth if requested,
 * ClientInit/ServerInit). Blocks until the handshake finishes or fails.
 * `password` may be NULL/empty if the server offers Security-Type "None". */
esp_err_t rfb_client_connect(rfb_client_t *client, const char *host, uint16_t port,
                              const char *password);

/* Blocking receive loop - reads and dispatches server->client messages
 * (FramebufferUpdate, Bell, ServerCutText, SetColourMapEntries) until the
 * connection closes or rfb_client_disconnect() is called from another task.
 * Run this on its own task; it also sends the periodic
 * FramebufferUpdateRequest messages needed to keep updates flowing. */
esp_err_t rfb_client_run(rfb_client_t *client);

/* Thread-safe: closes the socket, which unblocks rfb_client_run(). */
void rfb_client_disconnect(rfb_client_t *client);

/* Thread-safe: send a PointerEvent (RFB button-mask: bit0 = left button). */
esp_err_t rfb_client_send_pointer(rfb_client_t *client, uint16_t x, uint16_t y, uint8_t button_mask);

/* Thread-safe: send a KeyEvent. `keysym` is an X11 keysym value. */
esp_err_t rfb_client_send_key(rfb_client_t *client, uint32_t keysym, bool down);

/* Thread-safe: ask the server to resend the whole framebuffer (non-incremental).
 * Useful after a resync or right after connecting to force a first paint. */
esp_err_t rfb_client_request_full_update(rfb_client_t *client);

#ifdef __cplusplus
}
#endif
