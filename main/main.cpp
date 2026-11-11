/*
 * main.cpp — Batear entry point
 *
 * Role is selected at build time via `idf.py menuconfig` → Batear → Device role.
 *
 * LoRa node (CONFIG_BATEAR_LORA_NODE — node, detector or gateway preset):
 *   capabilities are resolved at boot (node_caps.h), then
 *   Core 0 │ LoRaTask  — owns the SX1262: TX own events, RX → OLED/LED/MQTT
 *   Core 1 │ AudioTask — I2S mic + FFT harmonic detector      (if mic)
 *   Core 1 │ MqttTask  — WiFi + MQTT publish + HA Discovery   (if bridge)
 *
 * Wired Detector mode (CONFIG_BATEAR_ROLE_WIRED_DETECTOR):
 *   Core 0 │ EthMqttTask — W5500 Ethernet + MQTT publish + HA Discovery
 *   Core 1 │ AudioTask   — I2S mic + FFT harmonic detector
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "lora_provision.h"
#include "config_console.h"

#ifdef CONFIG_BATEAR_LORA_NODE
#include "drone_detector.h"
#include "audio_task.h"
#include "lora_radio.h"
#include "mqtt_task.h"
#include "node_caps.h"
#endif

#ifdef CONFIG_BATEAR_ROLE_WIRED_DETECTOR
#include "drone_detector.h"
#include "audio_task.h"
#include "eth_mqtt_task.h"
#include "esp_ota_ops.h"
#endif

static const char *TAG = "main";

#if defined(CONFIG_BATEAR_LORA_NODE) || defined(CONFIG_BATEAR_ROLE_WIRED_DETECTOR)
QueueHandle_t g_drone_event_queue = NULL;
#endif

extern "C" void app_main(void)
{
    {
        esp_err_t nret = nvs_flash_init();
        if (nret == ESP_ERR_NVS_NO_FREE_PAGES || nret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS partition changed — erasing all stored config");
            nvs_flash_erase();
            nret = nvs_flash_init();
        }
        if (nret != ESP_OK) {
            ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nret));
        }
    }

    lora_provision_init();
    lora_log_keys(TAG);

    config_console_init();

#ifdef CONFIG_BATEAR_LORA_NODE

    ESP_LOGI(TAG, "Batear LoRa NODE (dev_id=%u)", lora_get_keys()->device_id);

    node_caps_init();
    const node_caps_t *caps = node_caps();

    g_drone_event_queue = xQueueCreate(4, sizeof(DroneEvent_t));
    if (g_drone_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue — halting");
        return;
    }

    if (caps->bridge) {
        g_mqtt_event_queue = xQueueCreate(8, sizeof(MqttEvent_t));
        if (g_mqtt_event_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create MQTT event queue — halting");
            return;
        }
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        LoRaTask, "LoRaTask",
        8 * 1024 / sizeof(StackType_t),
        NULL, configMAX_PRIORITIES - 2,
        NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "LoRaTask create failed — halting");
        return;
    }

    if (caps->mic) {
        ret = xTaskCreatePinnedToCore(
            AudioTask, "AudioTask",
            6 * 1024 / sizeof(StackType_t),
            NULL, configMAX_PRIORITIES - 2,
            NULL, 1);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "AudioTask create failed");
        }
    }

    if (caps->bridge) {
        ret = xTaskCreatePinnedToCore(
            MqttTask, "MqttTask",
            6 * 1024 / sizeof(StackType_t),
            NULL, configMAX_PRIORITIES - 3,
            NULL, 1);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "MqttTask create failed");
        }
    }

    ESP_LOGI(TAG, "Node running — LoRaTask(Core0)%s%s",
             caps->mic ? " + AudioTask(Core1)" : "",
             caps->bridge ? " + MqttTask(Core1)" : "");

#elif defined(CONFIG_BATEAR_ROLE_WIRED_DETECTOR)

    ESP_LOGI(TAG, "Batear WIRED DETECTOR (dev_id=%u)", lora_get_keys()->device_id);

    /* Confirm OTA rollback — if this boot is after an OTA update, mark the
       firmware valid so the bootloader won't roll back on next reboot. */
    esp_ota_mark_app_valid_cancel_rollback();

    g_drone_event_queue = xQueueCreate(4, sizeof(DroneEvent_t));
    if (g_drone_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue — halting");
        return;
    }

    static TaskHandle_t audio_h, eth_mqtt_h;

    BaseType_t ret = xTaskCreatePinnedToCore(
        AudioTask, "AudioTask",
        6 * 1024 / sizeof(StackType_t),
        NULL, configMAX_PRIORITIES - 2,
        &audio_h, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "AudioTask create failed — halting");
        return;
    }

    ret = xTaskCreatePinnedToCore(
        EthMqttTask, "EthMqttTask",
        6 * 1024 / sizeof(StackType_t),
        NULL, configMAX_PRIORITIES - 3,
        &eth_mqtt_h, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "EthMqttTask create failed — halting");
        vTaskDelete(audio_h);
        return;
    }

    ESP_LOGI(TAG, "Wired Detector running — AudioTask(Core1) + EthMqttTask(Core0)");

#else
    #error "Select a role in menuconfig: Batear → Device role"
#endif
}
