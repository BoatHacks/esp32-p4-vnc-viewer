#include "serial_cli.h"
#include "wifi_manager.h"
#include "vnc_config.h"
#include "ota_update.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_cli";
static serial_cli_cfg_t s_cfg;

static int cmd_wifi(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: wifi <ssid> <password>\n");
        return 1;
    }
    printf("Connecting to '%s' ...\n", argv[1]);
    fflush(stdout);

    esp_err_t err = wifi_manager_connect(argv[1], argv[2], 15000);
    if (err == ESP_OK) {
        printf("Connected and saved as the default network.\n");
    } else {
        printf("Failed: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_vnc(int argc, char **argv)
{
    if (argc < 3 || argc > 5) {
        printf("Usage: vnc <host> <port> [username] [password]\n");
        return 1;
    }

    const char *host = argv[1];
    long port = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535) {
        printf("Port must be between 1 and 65535.\n");
        return 1;
    }
    const char *username = (argc >= 4) ? argv[3] : "";
    const char *password = (argc >= 5) ? argv[4] : "";

    esp_err_t err = vnc_config_save(host, (uint16_t)port, username, password);
    if (err != ESP_OK) {
        printf("Failed to save: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Saved VNC server %s:%ld.", host, port);
    if (s_cfg.vnc_client) {
        rfb_client_disconnect(s_cfg.vnc_client);
        printf(" Reconnecting now...\n");
    } else {
        printf(" Will connect once the VNC client starts.\n");
    }
    return 0;
}

static int cmd_ota(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Checking %s/%s for an update...\n", s_cfg.ota_owner, s_cfg.ota_repo);
    fflush(stdout);

    /* On success this reboots into the new firmware and never returns. */
    esp_err_t err = ota_check_and_update(s_cfg.ota_owner, s_cfg.ota_repo, s_cfg.ota_asset_name);
    if (err == ESP_OK) {
        printf("Already up to date.\n");
    } else {
        printf("Update check failed: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100)); /* give the UART time to actually send the line above */
    esp_restart();
    return 0; /* unreachable */
}

void serial_cli_set_vnc_client(rfb_client_t *client)
{
    s_cfg.vnc_client = client;
}

esp_err_t serial_cli_start(const serial_cli_cfg_t *cfg)
{
    s_cfg = *cfg;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp32-p4-vnc>";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_console_register_help_command();

    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Connect to a Wi-Fi network and save it: wifi <ssid> <password>",
        .hint = NULL,
        .func = &cmd_wifi,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    const esp_console_cmd_t vnc_cmd = {
        .command = "vnc",
        .help = "Set the VNC server to use: vnc <host> <port> [username] [password]",
        .hint = NULL,
        .func = &cmd_vnc,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&vnc_cmd));

    const esp_console_cmd_t ota_cmd = {
        .command = "ota",
        .help = "Check for and install a firmware update now",
        .hint = NULL,
        .func = &cmd_ota,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ota_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the device",
        .hint = NULL,
        .func = &cmd_reboot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_start_repl failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Serial CLI ready - type 'help' for commands");
    return ESP_OK;
}
