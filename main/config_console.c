/*
 * config_console.c — Interactive serial console for runtime configuration
 *
 * Commands:
 *   show              — display NVS-stored config and Kconfig defaults
 *   set <key> <value> — write a config value to NVS (reboot to apply)
 *   reboot            — restart the device
 *
 * Detector keys:  device_id, net_key
 * Gateway  keys:  wifi_ssid, wifi_pass, mqtt_url, mqtt_user, mqtt_pass,
 *                 device_id, net_key
 */

#include "config_console.h"
#include "sdkconfig.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "console";

/* ================================================================
 * Hex helpers
 * ================================================================ */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf("%02X", data[i]);
    }
}

/* ================================================================
 * NVS print helpers
 * ================================================================ */

static void print_nvs_str(nvs_handle_t h, const char *key, const char *label)
{
    char buf[128];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
        printf("  %-12s = %s  [NVS]\n", label, buf);
    } else {
        printf("  %-12s = (not set)\n", label);
    }
}

static void print_nvs_blob_hex(nvs_handle_t h, const char *key,
                                const char *label, size_t expected_len)
{
    uint8_t buf[32];
    size_t len = expected_len;
    if (len > sizeof(buf)) len = sizeof(buf);
    if (nvs_get_blob(h, key, buf, &len) == ESP_OK && len == expected_len) {
        printf("  %-12s = ", label);
        print_hex(buf, len);
        printf("  [NVS]\n");
    } else {
        printf("  %-12s = (not set)\n", label);
    }
}

static void print_nvs_u8(nvs_handle_t h, const char *key, const char *label)
{
    uint8_t val;
    if (nvs_get_u8(h, key, &val) == ESP_OK) {
        printf("  %-12s = %u  [NVS]\n", label, val);
    } else {
        printf("  %-12s = (not set)\n", label);
    }
}

static void print_nvs_u8_hex(nvs_handle_t h, const char *key, const char *label)
{
    uint8_t val;
    if (nvs_get_u8(h, key, &val) == ESP_OK) {
        printf("  %-12s = 0x%02X  [NVS]\n", label, val);
    } else {
        printf("  %-12s = (not set)\n", label);
    }
}

static void print_nvs_u32(nvs_handle_t h, const char *key, const char *label,
                           const char *unit)
{
    uint32_t val;
    if (nvs_get_u32(h, key, &val) == ESP_OK) {
        printf("  %-12s = %lu %s  [NVS]\n", label, (unsigned long)val, unit);
    } else {
        printf("  %-12s = (not set)\n", label);
    }
}

/* ================================================================
 * "show" command
 * ================================================================ */

static int cmd_show(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n--- Batear Configuration ---\n\n");

    /* LoRa / common config (lora_cfg namespace) */
    printf("[lora_cfg]  (shared network settings)\n");
    {
        nvs_handle_t h;
        bool opened = (nvs_open("lora_cfg", NVS_READONLY, &h) == ESP_OK);
        if (opened) {
            print_nvs_blob_hex(h, "app_key", "net_key", 16);
#if defined(CONFIG_BATEAR_ROLE_DETECTOR) || defined(CONFIG_BATEAR_ROLE_WIRED_DETECTOR)
            print_nvs_u8(h, "device_id", "device_id");
#endif
#if defined(CONFIG_BATEAR_ROLE_DETECTOR) || defined(CONFIG_BATEAR_ROLE_GATEWAY)
            print_nvs_u32(h, "lora_freq", "lora_freq", "kHz");
            print_nvs_u8_hex(h, "sync_word", "sync_word");
#endif
            nvs_close(h);
        } else {
            printf("  (namespace not found — using Kconfig defaults)\n");
        }
    }
    printf("  Kconfig defaults:\n");
    printf("    net_key    = %s\n", CONFIG_BATEAR_NET_KEY);
#if defined(CONFIG_BATEAR_ROLE_DETECTOR) || defined(CONFIG_BATEAR_ROLE_WIRED_DETECTOR)
    printf("    device_id  = %d\n", CONFIG_BATEAR_DEVICE_ID);
#endif
#ifdef CONFIG_BATEAR_LORA_FREQ
    printf("    lora_freq  = %d kHz\n", CONFIG_BATEAR_LORA_FREQ);
#endif
#ifdef CONFIG_BATEAR_LORA_SYNC_WORD
    printf("    sync_word  = 0x%02X\n", CONFIG_BATEAR_LORA_SYNC_WORD);
#endif

#ifdef CONFIG_BATEAR_ROLE_GATEWAY
    printf("\n[gateway_cfg]  (WiFi / MQTT)\n");
    {
        nvs_handle_t h;
        bool opened = (nvs_open("gateway_cfg", NVS_READONLY, &h) == ESP_OK);
        if (opened) {
            print_nvs_str(h, "wifi_ssid", "wifi_ssid");
            print_nvs_str(h, "wifi_pass", "wifi_pass");
            print_nvs_str(h, "mqtt_url",  "mqtt_url");
            print_nvs_str(h, "mqtt_user", "mqtt_user");
            print_nvs_str(h, "mqtt_pass", "mqtt_pass");
            print_nvs_str(h, "device_id", "device_id");
            nvs_close(h);
        } else {
            printf("  (namespace not found — using Kconfig defaults)\n");
        }
    }
    printf("  Kconfig defaults:\n");
    printf("    wifi_ssid  = %s\n", CONFIG_BATEAR_WIFI_SSID);
    printf("    mqtt_url   = %s\n", CONFIG_BATEAR_MQTT_BROKER_URL);
    printf("    device_id  = %s\n", CONFIG_BATEAR_GW_DEVICE_ID);
#endif

#ifdef CONFIG_BATEAR_ROLE_WIRED_DETECTOR
    printf("\n[wired_cfg]  (Ethernet / MQTT)\n");
    {
        nvs_handle_t h;
        bool opened = (nvs_open("wired_cfg", NVS_READONLY, &h) == ESP_OK);
        if (opened) {
            print_nvs_str(h, "mqtt_url",  "mqtt_url");
            print_nvs_str(h, "mqtt_user", "mqtt_user");
            print_nvs_str(h, "mqtt_pass", "mqtt_pass");
            print_nvs_str(h, "device_id", "device_id");
            nvs_close(h);
        } else {
            printf("  (namespace not found — using Kconfig defaults)\n");
        }
    }
    printf("  Kconfig defaults:\n");
    printf("    mqtt_url   = %s\n", CONFIG_BATEAR_MQTT_BROKER_URL);
    printf("    device_id  = %s\n", CONFIG_BATEAR_WIRED_DEVICE_ID);
#endif

    printf("\n");
    return 0;
}

/* ================================================================
 * "set" command
 * ================================================================ */

static int set_nvs_str(const char *ns, const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        printf("Error: cannot open NVS namespace '%s': %s\n",
               ns, esp_err_to_name(err));
        return 1;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        printf("Error: nvs_set_str failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    /* Verify by reading back */
    char verify[128] = {0};
    size_t vlen = sizeof(verify);
    esp_err_t verr = nvs_open(ns, NVS_READONLY, &h);
    if (verr == ESP_OK) {
        verr = nvs_get_str(h, key, verify, &vlen);
        nvs_close(h);
    }
    if (verr != ESP_OK || strcmp(verify, value) != 0) {
        printf("WARN: write verification failed for %s:%s\n", ns, key);
        return 1;
    }

    printf("OK: %s:%s = \"%s\" (reboot to apply)\n", ns, key, value);
    return 0;
}

static int set_nvs_u8(const char *ns, const char *key, uint8_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        printf("Error: cannot open NVS namespace '%s': %s\n",
               ns, esp_err_to_name(err));
        return 1;
    }
    err = nvs_set_u8(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        printf("Error: nvs_set_u8 failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK: %s:%s = %u (reboot to apply)\n", ns, key, value);
    return 0;
}

static int set_nvs_u32(const char *ns, const char *key, uint32_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        printf("Error: cannot open NVS namespace '%s': %s\n",
               ns, esp_err_to_name(err));
        return 1;
    }
    err = nvs_set_u32(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        printf("Error: nvs_set_u32 failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK: %s:%s = %lu (reboot to apply)\n", ns, key, (unsigned long)value);
    return 0;
}

static int set_nvs_blob(const char *ns, const char *key,
                         const uint8_t *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        printf("Error: cannot open NVS namespace '%s': %s\n",
               ns, esp_err_to_name(err));
        return 1;
    }
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        printf("Error: nvs_set_blob failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK: %s:%s = ", ns, key);
    print_hex(data, len);
    printf(" (reboot to apply)\n");
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: set <key> <value>\n");
        printf("Keys:\n");
        printf("  net_key     — 32 hex chars (AES-128 key)\n");
#if defined(CONFIG_BATEAR_ROLE_DETECTOR) || defined(CONFIG_BATEAR_ROLE_GATEWAY)
        printf("  lora_freq   — frequency in kHz (e.g. 915000, 868000)\n");
        printf("  sync_word   — hex byte (e.g. 12, 34)\n");
#endif
#if defined(CONFIG_BATEAR_ROLE_DETECTOR) || defined(CONFIG_BATEAR_ROLE_WIRED_DETECTOR)
        printf("  device_id   — 0-255\n");
#endif
#ifdef CONFIG_BATEAR_ROLE_GATEWAY
        printf("  wifi_ssid   — Wi-Fi SSID\n");
        printf("  wifi_pass   — Wi-Fi password\n");
        printf("  mqtt_url    — e.g. mqtt://192.168.1.100:1883\n");
        printf("  mqtt_user   — MQTT username\n");
        printf("  mqtt_pass   — MQTT password\n");
        printf("  mqtt_device_id — gateway ID for MQTT topics\n");
#endif
#ifdef CONFIG_BATEAR_ROLE_WIRED_DETECTOR
        printf("  mqtt_url    — e.g. mqtt://192.168.1.100:1883\n");
        printf("  mqtt_user   — MQTT username\n");
        printf("  mqtt_pass   — MQTT password\n");
        printf("  mqtt_device_id — wired detector ID for MQTT topics\n");
#endif
        return 1;
    }

    const char *key = argv[1];
    const char *value = argv[2];

    /* net_key — shared by both roles */
    if (strcmp(key, "net_key") == 0) {
        uint8_t blob[16];
        if (!hex_to_bytes(value, blob, 16)) {
            printf("Error: net_key must be exactly 32 hex characters\n");
            return 1;
        }
        return set_nvs_blob("lora_cfg", "app_key", blob, 16);
    }

#if defined(CONFIG_BATEAR_ROLE_DETECTOR) || defined(CONFIG_BATEAR_ROLE_GATEWAY)
    if (strcmp(key, "lora_freq") == 0) {
        long freq = atol(value);
        if (freq < 100000 || freq > 1000000) {
            printf("Error: lora_freq must be in kHz (e.g. 915000)\n");
            return 1;
        }
        return set_nvs_u32("lora_cfg", "lora_freq", (uint32_t)freq);
    }

    if (strcmp(key, "sync_word") == 0) {
        uint8_t sw_byte;
        if (strlen(value) != 2 || !hex_to_bytes(value, &sw_byte, 1)) {
            printf("Error: sync_word must be 2 hex chars (e.g. 12, 34)\n");
            return 1;
        }
        return set_nvs_u8("lora_cfg", "sync_word", sw_byte);
    }
#endif

#if defined(CONFIG_BATEAR_ROLE_DETECTOR) || defined(CONFIG_BATEAR_ROLE_WIRED_DETECTOR)
    if (strcmp(key, "device_id") == 0) {
        int id = atoi(value);
        if (id < 0 || id > 255) {
            printf("Error: device_id must be 0-255\n");
            return 1;
        }
        return set_nvs_u8("lora_cfg", "device_id", (uint8_t)id);
    }
#endif

#ifdef CONFIG_BATEAR_ROLE_GATEWAY
    if (strcmp(key, "wifi_ssid") == 0)
        return set_nvs_str("gateway_cfg", "wifi_ssid", value);
    if (strcmp(key, "wifi_pass") == 0)
        return set_nvs_str("gateway_cfg", "wifi_pass", value);
    if (strcmp(key, "mqtt_url") == 0)
        return set_nvs_str("gateway_cfg", "mqtt_url", value);
    if (strcmp(key, "mqtt_user") == 0)
        return set_nvs_str("gateway_cfg", "mqtt_user", value);
    if (strcmp(key, "mqtt_pass") == 0)
        return set_nvs_str("gateway_cfg", "mqtt_pass", value);
    if (strcmp(key, "mqtt_device_id") == 0)
        return set_nvs_str("gateway_cfg", "device_id", value);
#endif

#ifdef CONFIG_BATEAR_ROLE_WIRED_DETECTOR
    if (strcmp(key, "mqtt_url") == 0)
        return set_nvs_str("wired_cfg", "mqtt_url", value);
    if (strcmp(key, "mqtt_user") == 0)
        return set_nvs_str("wired_cfg", "mqtt_user", value);
    if (strcmp(key, "mqtt_pass") == 0)
        return set_nvs_str("wired_cfg", "mqtt_pass", value);
    if (strcmp(key, "mqtt_device_id") == 0)
        return set_nvs_str("wired_cfg", "device_id", value);
#endif

    printf("Error: unknown key '%s'\n", key);
    return 1;
}

/* ================================================================
 * "reboot" command
 * ================================================================ */

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;  /* unreachable */
}

/* ================================================================
 * Console initialisation
 * ================================================================ */

void config_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "batear> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    /* Register commands */
    const esp_console_cmd_t show_cmd = {
        .command = "show",
        .help = "Display current configuration (NVS + Kconfig defaults)",
        .hint = NULL,
        .func = cmd_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&show_cmd));

    const esp_console_cmd_t set_cmd = {
        .command = "set",
        .help = "Set a config value: set <key> <value>",
        .hint = "<key> <value>",
        .func = cmd_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&set_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the device to apply configuration changes",
        .hint = NULL,
        .func = cmd_reboot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Console ready — type 'help' for commands");
}
