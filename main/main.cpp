/*
 * main.cpp — Batear entry point
 *
 * Role is selected at build time via `idf.py menuconfig` → Batear → Device role.
 *
 * Detector mode (CONFIG_BATEAR_ROLE_DETECTOR):
 *   Core 0 │ LoRaTask  — waits on queue, TX encrypted packets via SX1262
 *   Core 1 │ AudioTask — I2S mic + FFT harmonic detector
 *
 * Gateway mode (CONFIG_BATEAR_ROLE_GATEWAY):
 *   Core 0 │ GatewayTask — LoRa RX + decrypt + OLED + LED
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifdef CONFIG_BATEAR_LORAWAN_PROVISION
#include "lorawan_provision.h"
#endif

#ifdef CONFIG_BATEAR_ROLE_DETECTOR
#include "drone_detector.h"
#include "audio_task.h"
#include "lora_task.h"
#endif

#ifdef CONFIG_BATEAR_ROLE_GATEWAY
#include "gateway_task.h"
#endif

static const char *TAG = "main";

#ifdef CONFIG_BATEAR_ROLE_DETECTOR
QueueHandle_t g_drone_event_queue = NULL;
#endif

extern "C" void app_main(void)
{
#ifdef CONFIG_BATEAR_LORAWAN_PROVISION
    lorawan_keys_t lora_keys;
    if (lorawan_provision_keys(&lora_keys) == ESP_OK) {
        lorawan_log_keys(TAG, &lora_keys);
    } else {
        ESP_LOGE(TAG, "LoRaWAN provisioning failed — cannot read MAC");
    }
#endif

#ifdef CONFIG_BATEAR_ROLE_DETECTOR

    ESP_LOGI(TAG, "Batear DETECTOR (dev_id=%d)", CONFIG_BATEAR_DEVICE_ID);

    g_drone_event_queue = xQueueCreate(4, sizeof(DroneEvent_t));
    if (g_drone_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue — halting");
        return;
    }

    static TaskHandle_t audio_h, lora_h;

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
        LoRaTask, "LoRaTask",
        4 * 1024 / sizeof(StackType_t),
        NULL, configMAX_PRIORITIES - 4,
        &lora_h, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "LoRaTask create failed — halting");
        vTaskDelete(audio_h);
        return;
    }

    ESP_LOGI(TAG, "Detector running — AudioTask(Core1) + LoRaTask(Core0)");

#elif defined(CONFIG_BATEAR_ROLE_GATEWAY)

    ESP_LOGI(TAG, "Batear GATEWAY");

    static TaskHandle_t gw_h;

    BaseType_t ret = xTaskCreatePinnedToCore(
        GatewayTask, "GatewayTask",
        8 * 1024 / sizeof(StackType_t),
        NULL, configMAX_PRIORITIES - 2,
        &gw_h, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "GatewayTask create failed — halting");
        return;
    }

    ESP_LOGI(TAG, "Gateway running — GatewayTask(Core0)");

#else
    #error "Select a role in menuconfig: Batear → Device role"
#endif
}
