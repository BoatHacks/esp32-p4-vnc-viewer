# ESP32-P4 VNC viewer

A from-scratch RFB/VNC viewer for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B:
connects to a VNC server over Wi-Fi, renders framebuffer updates to the
1024x600 MIPI-DSI panel, and turns touches into VNC pointer events.

## What's here

```
main/
  rfb_client.c/.h     RFB protocol engine (handshake, VNC auth, Raw + CopyRect
                      encodings, pointer/key events). No display code - pure
                      sockets + protocol state machine.
  vnc_display.c/.h    Owns a PSRAM shadow framebuffer, blits updates to the
                      panel, polls touch and forwards events to rfb_client.
  wifi_manager.c/.h   Wi-Fi station bring-up: connect with bounded retries,
                      scan, and detect when a previously-working connection
                      is lost (vs. just failing on the very first attempt).
  wifi_creds.c/.h     NVS-backed storage for the last-used SSID/password.
  wifi_setup_ui.c/.h  LVGL dialog: scan list -> tap a network -> on-screen
                      keyboard for the password -> connect. Shown on first
                      boot (nothing saved yet) and again any time the saved
                      network stops working.
  vnc_config.c/.h     NVS-backed storage for the last-used VNC host/port/
                      password.
  vnc_setup_ui.c/.h   LVGL dialog: host/port/password fields + on-screen
                      keyboard + Connect button, attempting a real RFB
                      handshake with a bounded timeout. Shown on first boot
                      (nothing saved yet) and again after a few consecutive
                      connection failures.
  main.c              Wires BSP display/touch init + Wi-Fi + the VNC client
                      together.
  ota_update.c/.h     Checks this repo's GitHub Releases for a newer
                      version and flashes it via esp_https_ota.
.github/workflows/
  build-firmware.yml  Builds the firmware on every published release and
                      attaches the binary OTA looks for.
```

## Wi-Fi setup flow

- **First boot** (nothing saved yet): the setup dialog appears immediately.
- **Saved network fails to connect** (wrong password changed, AP gone,
  etc.) at startup: same dialog, after a bounded 15s connection attempt.
- **A working connection is lost later** (router reboot, moved out of
  range, and automatic retries give up): the running VNC session pauses,
  the same dialog reappears over it, and once you pick a network and it
  connects, VNC reconnects automatically and the dialog is dismissed.
- Whatever you pick that works is saved to NVS (`wifi_cfg` namespace,
  keys `ssid`/`pass`) and becomes the new "saved network" for next boot.
  There's currently no on-device "forget this network" option; the
  quickest way to clear it is `idf.py erase-flash` (which also wipes
  everything else in NVS) or adding a small `wifi_creds_clear()` call
  behind a long-press gesture if you want something less blunt.

## VNC server setup flow

- **First boot** (no server saved yet): the connection dialog appears
  immediately (host/IP, port, optional password).
- **Saved server fails 3 times in a row** (wrong IP, server down,
  password changed, etc. - each attempt bounded to 8s so a bad address
  fails fast rather than hanging): same dialog reappears, pre-filled with
  the last-used values so you're only fixing what's wrong.
- Whatever connects successfully is saved to NVS (`vnc_cfg` namespace:
  `host`, `port`, `pass`) and becomes the new default for next boot and
  future reconnects.
- Unlike the Wi-Fi dialog, there's no separate "connection lost" watchdog
  here - `vnc_task` in `main.c` already polls the connection directly, so
  repeated failures are just counted in that same loop.

## OTA updates

The device checks `github.com/BoatHacks/esp32-p4-vnc-viewer`'s releases
(same ones the "send it!" workflow publishes) for a newer version, once
30s after Wi-Fi comes up and then every 24h (`OTA_CHECK_INTERVAL_HOURS` in
`main.c`). If the latest release's tag is semver-newer than the running
firmware's embedded version (see `PROJECT_VER` in `CMakeLists.txt`) and
has a release asset literally named `esp32-p4-vnc-viewer.bin`, it
downloads and flashes it via `esp_https_ota`, then reboots.

**This needs an OTA-aware partition table**, which `partitions.csv`
provides (`ota_0`/`ota_1` app slots + `otadata`, sized 3MB each - see
`sdkconfig.defaults` for the `CONFIG_PARTITION_TABLE_CUSTOM` wiring and
the `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` setting these fit under). If you
already have a different partition layout, OTA needs at least two `app`
partitions and one `data, ota` partition; adjust sizes to fit your image
rather than removing them. Note this board actually has 32MB of flash,
but declaring only 8MB is harmless - it's just an upper bound the
partition table has to fit inside, not the real capacity in use.

**Rollback safety**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on, and
`main.c` calls `ota_mark_running_app_valid()` once Wi-Fi comes up
successfully after boot. If a bad OTA image can't even get that far, the
bootloader automatically reverts to the previous working image on the
next reset - important on a device with no keyboard or serial console
normally attached to debug a bricked update from.

**Important - attaching the actual binary**: this is now automated by
`.github/workflows/build-firmware.yml`, which runs whenever a release is
published (i.e. right after `gh release create`, the last step of "send
it!"). It builds the firmware with the official ESP-IDF Docker image via
`espressif/esp-idf-ci-action`, and uploads the resulting binary to that
same release under the exact name `ota_update.c` looks for. Two things
worth knowing:
- It targets ESP-IDF `v5.3` (`IDF_VERSION` in the workflow) - bump that
  alongside any `idf.py add-dependency` changes that need a newer IDF.
- The v0.1.1 release predates this workflow, so it won't have a binary
  attached. You can backfill it (or rebuild any tag) via the Actions tab
  -> "Build and attach firmware" -> "Run workflow", entering the tag
  name - or manually, same as before:
  ```
  idf.py build
  gh release upload v0.1.1 build/esp32_p4_vnc_viewer.bin#esp32-p4-vnc-viewer.bin
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
   idf.py add-dependency "espressif/esp_lvgl_port"
   idf.py add-dependency "lvgl/lvgl"
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
4. Edit `main/main.c`: set `VNC_HOST`, `VNC_PORT`, and `VNC_PASSWORD` (leave
   the password empty if your VNC server allows Security-Type "None").
   Wi-Fi credentials are *not* hardcoded here - they're entered on-device
   via the setup dialog on first boot and saved to NVS from then on.
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
(`rfb_client.c`, `vnc_display.c`, `wifi_manager.c`, `wifi_setup_ui.c`) only
depends on stable, version-independent APIs (`esp_lcd_panel_*`,
`esp_lcd_touch_*`, `esp_wifi_*`, `esp_lvgl_port`, BSD sockets), so it
shouldn't need touching. In particular, the LVGL wiring for the Wi-Fi
setup screens deliberately goes through `esp_lvgl_port`'s
`lvgl_port_add_disp()`/`lvgl_port_add_touch()` instead of the BSP's own
`bsp_display_start()`, specifically to sidestep needing to guess that
function's internals.

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
