#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OTA updates sourced from this project's own GitHub Releases - the same
 * releases created by the "send it!" workflow. No separate update server
 * to run: checks `GET /repos/{owner}/{repo}/releases/latest`, and if the
 * release's tag is newer than the running firmware's version (compared
 * as semver, both expected in "vX.Y.Z" form) and has an asset named
 * `asset_name`, downloads and flashes it via esp_https_ota.
 *
 * IMPORTANT: for this to do anything, whatever process cuts a release
 * needs to actually attach a compiled firmware .bin as a release asset
 * under that exact name. `gh release create` by itself (as used so far
 * in this project) only publishes source - see the README for how to
 * attach the binary, e.g. `gh release upload <tag> build/esp32_p4_vnc_viewer.bin`.
 */

/* One-shot check: fetches the latest release, compares versions, and
 * flashes + reboots on a newer release. Returns ESP_OK with no reboot if
 * already up to date, or an error if the check/download/flash failed.
 * This function does not return at all on success (esp_restart()). */
esp_err_t ota_check_and_update(const char *owner, const char *repo, const char *asset_name);

/* Starts a background task that calls ota_check_and_update() once at
 * startup (after a short delay to let Wi-Fi settle) and then again every
 * `interval_hours`. */
esp_err_t ota_start_periodic_check(const char *owner, const char *repo, const char *asset_name,
                                    uint32_t interval_hours);

/* Call once you're confident the currently-running image actually works
 * (e.g. after Wi-Fi and the first VNC connection both succeed). Cancels
 * the bootloader's pending rollback for this image - without this call,
 * a fresh OTA image left unconfirmed across a reboot gets automatically
 * rolled back to the previous one. Safe to call on a non-OTA (factory)
 * boot too; it's a no-op there. */
void ota_mark_running_app_valid(void);

#ifdef __cplusplus
}
#endif
