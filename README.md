# ESP32-P4 VNC viewer

A from-scratch RFB/VNC viewer for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B:
connects to a VNC server over Wi-Fi, renders framebuffer updates to the
1024x600 MIPI-DSI panel, and turns touches into VNC pointer events.

## What's here

```
main/
  rfb_client.c/.h    RFB protocol engine (handshake, VNC auth, Raw + CopyRect
                     encodings, pointer/key events). No display code - pure
                     sockets + protocol state machine.
  vnc_display.c/.h   Owns a PSRAM shadow framebuffer, blits updates to the
                     panel, polls touch and forwards events to rfb_client.
  wifi_connect.c/.h  Standard Wi-Fi station bring-up.
  main.c             Wires BSP display/touch init + Wi-Fi + the VNC client
                     together. Fill in your Wi-Fi/VNC server details here.
```

## One-time setup

1. Install ESP-IDF >= 5.3 (this board needs a fairly recent IDF; the Wiki
   recommends using the VS Code ESP-IDF extension or the command-line
   installer).
2. From the project root:
   ```
   idf.py set-target esp32p4
   idf.py add-dependency "waveshare/esp32_p4_wifi6_touch_lcd_7b"
   idf.py add-dependency "espressif/esp_wifi_remote"
   idf.py add-dependency "espressif/esp_hosted"
   ```
   (These are already listed in `main/idf_component.yml`; running
   `add-dependency` just pulls them down and confirms the exact versions
   the registry resolves to on your machine.)
3. `idf.py menuconfig`:
   - **Component config -> Board Support Package (ESP32-P4) -> Display**:
     select this board's LCD type.
   - **Component config -> Wi-Fi Remote**: set slave target to `esp32-c6`
     (this is what lets `esp_wifi_*` calls transparently reach the onboard
     ESP32-C6 over SDIO).
4. Edit `main/main.c`: set `WIFI_SSID`, `WIFI_PASSWORD`, `VNC_HOST`,
   `VNC_PORT`, and `VNC_PASSWORD` (leave the password empty if your VNC
   server allows Security-Type "None").
5. `idf.py build flash monitor`.

## The one part you'll likely need to adjust

`board_display_touch_init()` in `main.c` calls `bsp_display_new()` and
`bsp_touch_new()`, following Espressif's standard "esp-bsp" pattern that's
used consistently across their board-support packages. I couldn't compile
against this specific BSP version's actual header to confirm the exact
signature, so before your first build, open:

```
managed_components/waveshare__esp32_p4_wifi6_touch_lcd_7b/include/bsp/esp-bsp.h
```

and check `bsp_display_new()`'s and `bsp_touch_new()`'s real parameter types
against what `main.c` passes. Everything else in the project
(`rfb_client.c`, `vnc_display.c`, `wifi_connect.c`) only depends on stable,
version-independent ESP-IDF APIs (`esp_lcd_panel_*`, `esp_lcd_touch_*`,
`esp_wifi_*`, BSD sockets), so it shouldn't need touching.

## Design notes

- **Encodings**: only Raw and CopyRect are implemented. That's protocol-
  correct and simple to reason about, but Raw is bandwidth-heavy. Over
  Wi-Fi 6 on a LAN this is normally fine for a 1024x600 screen; if you're
  going over a slower or higher-latency link, Hextile or Tight+zlib would
  cut bandwidth substantially - `rfb_client.c`'s `SetEncodings` message
  and `handle_framebuffer_update()`'s rectangle-encoding switch are the two
  places to extend.
- **No keyboard input yet.** This board has no physical keyboard, so
  `rfb_client_send_key()` exists but nothing calls it. A natural follow-up
  is an on-screen keyboard (LVGL's `lv_keyboard`, drawn over the VNC view)
  that calls it with X11 keysyms on press/release.
- **Resolution mismatch**: if the remote desktop isn't 1024x600, the
  current code clips to the top-left corner rather than scaling - simplest
  thing that's still correct. `vnc_display.c`'s `on_raw_rect`/`on_copy_rect`
  are where you'd add scaling (or panning/scrolling) if you want the whole
  remote desktop visible.
- **Auth**: supports Security-Type None and classic VNC (DES challenge-
  response) authentication, matching RFB protocol versions 3.3/3.7/3.8.
  It does not implement TLS-based security types (VeNCrypt etc.) - if your
  server requires those, you'll need mbedtls TLS wrapped around the socket
  in `rfb_client_connect()`.
- **Framebuffer memory**: the shadow copy lives in PSRAM
  (`MALLOC_CAP_SPIRAM`), sized to the panel's resolution
  (1024*600*2 bytes ~= 1.2 MB) - comfortably inside this board's PSRAM.

## Testing without real hardware first

Since `rfb_client.c` has no ESP-IDF dependencies beyond BSD sockets,
mbedtls DES, and `esp_err_t`, you can stub those three things out and run
the protocol engine against a desktop VNC server (TigerVNC, x11vnc, etc.)
on a PC to validate the handshake and rectangle parsing before ever
touching the board - worth doing if you want to iterate faster than a
flash/monitor cycle allows.
