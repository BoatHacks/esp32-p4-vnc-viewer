# esp32-p4-vnc-viewer

From-scratch RFB/VNC client for a Waveshare ESP32-P4-WIFI6-Touch-LCD-7B board
(1024x600 MIPI-DSI display). Repo: BoatHacks/esp32-p4-vnc-viewer.

## Features built
- On-screen Wi-Fi and VNC setup dialogs (LVGL, NVS persistence).
- OTA firmware updates from GitHub Releases (version check, esp_https_ota,
  rollback protection, custom partition table).
- CI via espressif/esp-idf-ci-action: merged binary + four individual binaries
  as release assets.
- VeNCrypt Plain and X509Plain authentication; serial CLI (esp_console REPL).

## Hardware bug history (fixed via real hardware testing, v0.1.x-v0.4.x)
Chip revision rejection (ESP-IDF upgraded to v5.4), SDIO mempool crash, LVGL
MIPI-DSI API mismatch, PSRAM speed config symbol differences on P4, VeNCrypt TLS
missing one-byte server ack, OTA stack/buffer sizing.

## Hardware companion
There's a matching 3D-printed enclosure: parametric two-part OpenSCAD case
(front bezel with recessed glass pocket + screen window; open-frame back tray
with PCB standoffs), with wall cutouts on all four edges for connectors. The
second revision used pixel-coordinate analysis of Waveshare's official
dimensioned board diagram to get exact connector positions. STL + parametric
source exist.

## Command conventions
- "send it!" = commit + push + new GitHub release
- "bump" = patch increment, "bumpetybump" = minor increment
(see the user-level `plugin-release` skill for the general version of this)
