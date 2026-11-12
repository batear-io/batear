/*
 * power_button.c — long-press power-off for LoRa nodes. See power_button.h.
 */

#include "power_button.h"
#include "lora_radio.h"
#include "node_caps.h"
#include "node_rx.h"
#include "battery.h"
#include "pin_config.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "power";

#define POLL_MS          50
#define RADIO_SLEEP_MS 2000

static bool pressed(void)
{
    return gpio_get_level((gpio_num_t)PIN_USER_BUTTON) == 0;
}

static void hold_level(int pin, int level)
{
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, level);
    gpio_hold_en((gpio_num_t)pin);
}

static void blink_led(int times)
{
    gpio_reset_pin((gpio_num_t)PIN_LED);
    gpio_set_direction((gpio_num_t)PIN_LED, GPIO_MODE_OUTPUT);
    for (int i = 0; i < times; i++) {
        gpio_set_level((gpio_num_t)PIN_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level((gpio_num_t)PIN_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void power_off(void)
{
    const bool display = node_caps()->rx;
    ESP_LOGW(TAG, "long press — powering off");
    if (display) {
        node_rx_fatal("Power off: release");
    } else {
        blink_led(3);
    }

    if (!lora_radio_sleep(RADIO_SLEEP_MS)) {
        ESP_LOGW(TAG, "radio did not confirm sleep — powering off anyway");
    }

    /* Wake source is the button going low, so it must be up first. */
    while (pressed()) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(100));  /* debounce the release */

    /* Hold every rail and line that would otherwise leak in deep sleep. */
    hold_level(PIN_LED, 0);
#if BOARD_HAS_VEXT
    hold_level(PIN_VEXT, 1);                       /* Vext off (active low) */
#endif
    battery_prepare_sleep();
    hold_level(PIN_LORA_CS, 1);                    /* keep SX1262 deselected */
    gpio_deep_sleep_hold_en();

    rtc_gpio_pullup_en((gpio_num_t)PIN_USER_BUTTON);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_USER_BUTTON);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_USER_BUTTON, 0);

    ESP_LOGW(TAG, "off — press the button to turn back on");
    esp_deep_sleep_start();
}

static const int k_held_pins[] = {
    PIN_LED,
#if BOARD_HAS_VEXT
    PIN_VEXT,
#endif
#if BOARD_HAS_VBAT
    PIN_VBAT_CTRL,
#endif
    PIN_LORA_CS,
};

void power_button_boot(void)
{
    gpio_deep_sleep_hold_dis();
    for (size_t i = 0; i < sizeof(k_held_pins) / sizeof(k_held_pins[0]); i++) {
        gpio_hold_dis((gpio_num_t)k_held_pins[i]);
    }
    if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT0)) {
        rtc_gpio_deinit((gpio_num_t)PIN_USER_BUTTON);
        ESP_LOGI(TAG, "powered on by button");
    }
}

void PowerButtonTask(void *pvParameters)
{
    (void)pvParameters;

    if (CONFIG_BATEAR_POWER_OFF_HOLD_S == 0) {
        vTaskDelete(NULL);
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_USER_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    /* Ignore a press still held from waking the board. */
    while (pressed()) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    const TickType_t hold = pdMS_TO_TICKS(CONFIG_BATEAR_POWER_OFF_HOLD_S * 1000);
    ESP_LOGI(TAG, "hold the button %d s to power off", CONFIG_BATEAR_POWER_OFF_HOLD_S);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (!pressed()) {
            continue;
        }
        TickType_t start = xTaskGetTickCount();
        while (pressed() && xTaskGetTickCount() - start < hold) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
        if (pressed()) {
            power_off();
        }
    }
}
