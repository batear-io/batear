/*
 * node_caps.c — resolve LoRa node capabilities from NVS / Kconfig / probing
 */

#include "node_caps.h"
#include "audio_task.h"
#include "sdkconfig.h"

#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "caps";

static node_caps_t s_caps;

static uint8_t load_cap(nvs_handle_t h, bool opened, const char *key, uint8_t def)
{
    uint8_t v;
    if (opened && nvs_get_u8(h, key, &v) == ESP_OK && v <= NODE_CAP_AUTO) {
        return v;
    }
    return def;
}

static bool wifi_ssid_configured(void)
{
    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    nvs_handle_t h;
    if (nvs_open("gateway_cfg", NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_str(h, "wifi_ssid", ssid, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            return ssid[0] != '\0';
        }
    }
    return CONFIG_BATEAR_WIFI_SSID[0] != '\0';
}

static const char *cap_src(uint8_t v)
{
    return v == NODE_CAP_AUTO ? "auto" : v == NODE_CAP_ON ? "on" : "off";
}

void node_caps_init(void)
{
    nvs_handle_t h;
    bool opened = (nvs_open("node_cfg", NVS_READONLY, &h) == ESP_OK);
    uint8_t mic    = load_cap(h, opened, "cap_mic",    CONFIG_BATEAR_CAP_MIC_DEFAULT);
    uint8_t rx     = load_cap(h, opened, "cap_rx",     CONFIG_BATEAR_CAP_RX_DEFAULT);
    uint8_t bridge = load_cap(h, opened, "cap_bridge", CONFIG_BATEAR_CAP_BRIDGE_DEFAULT);
    uint8_t relay  = load_cap(h, opened, "cap_relay",  CONFIG_BATEAR_CAP_RELAY_DEFAULT);
    uint8_t flip;
    if (!opened || nvs_get_u8(h, "oled_flip", &flip) != ESP_OK) {
#if CONFIG_BATEAR_OLED_FLIP
        flip = 1;
#else
        flip = 0;
#endif
    }
    s_caps.oled_flip = (flip != 0);
    if (opened) {
        nvs_close(h);
    }

    s_caps.mic    = (mic == NODE_CAP_AUTO) ? audio_mic_probe() : (mic == NODE_CAP_ON);
    s_caps.bridge = (bridge == NODE_CAP_AUTO) ? wifi_ssid_configured() : (bridge == NODE_CAP_ON);
    s_caps.relay  = (relay == NODE_CAP_ON);
    s_caps.rx     = (rx == NODE_CAP_AUTO) ? !s_caps.mic : (rx == NODE_CAP_ON);
    if ((s_caps.bridge || s_caps.relay) && !s_caps.rx) {
        ESP_LOGW(TAG, "bridge/relay need rx — enabling rx");
        s_caps.rx = true;
    }

    ESP_LOGI(TAG, "mic=%d (%s)  rx=%d (%s)  bridge=%d (%s)  relay=%d (%s)",
             s_caps.mic, cap_src(mic), s_caps.rx, cap_src(rx),
             s_caps.bridge, cap_src(bridge), s_caps.relay, cap_src(relay));
}

const node_caps_t *node_caps(void)
{
    return &s_caps;
}
