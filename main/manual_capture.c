/*
 * manual_capture.c — BOOT-button (GPIO 0) push-to-talk recorder.
 *
 * Press = start recording. Release = stop. 20 ms polling loop on Core 0
 * with two-sample debouncing; we deliberately avoid `iot_button` /
 * `espressif/button` to keep the dependency surface flat. The recorder's
 * own MANUAL_SEC ceiling still guards against a stuck button.
 */

#include "manual_capture.h"

#if CONFIG_BATEAR_ROLE_WIRED_DETECTOR && CONFIG_BATEAR_TF_MANUAL_ENABLE

#include "tf_recorder.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "boot_btn";

#define BOOT_GPIO       ((gpio_num_t)0)
#define POLL_PERIOD_MS  20
#define DEBOUNCE_MS     40

static bool s_started;

static void manual_capture_task(void *arg)
{
    (void)arg;

    /* Seed `was_pressed` from the actual current level so an unintended
     * power-on press (or weak pull-up) doesn't synthesize a phantom rising
     * edge and start a recording before the user has touched the board. */
    bool was_pressed = (gpio_get_level(BOOT_GPIO) == 0);
    ESP_LOGI(TAG, "BOOT button watcher start (core %d) mode=PTT initial=%s",
             xPortGetCoreID(), was_pressed ? "DOWN" : "UP");

    for (;;) {
        /* GPIO 0 idles HIGH (internal + external pull-up); pressed = LOW.
         * Two-sample debounce filters single-frame glitches. */
        int level1 = gpio_get_level(BOOT_GPIO);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS / 2));
        int level2 = gpio_get_level(BOOT_GPIO);
        bool pressed = (level1 == 0) && (level2 == 0);

        if (pressed && !was_pressed) {
            ESP_LOGI(TAG, "PTT press → MANUAL REC start");
            tf_recorder_send_cmd(TF_CMD_MANUAL_START);
        } else if (!pressed && was_pressed) {
            ESP_LOGI(TAG, "PTT release → MANUAL REC stop");
            tf_recorder_send_cmd(TF_CMD_MANUAL_STOP);
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS - DEBOUNCE_MS / 2));
    }
}

void manual_capture_init(void)
{
    if (s_started) return;
    s_started = true;

    /* INPUT with internal pull-up enabled. The board may or may not have an
     * external pull-up on BOOT — relying on it leaves the pin floating on
     * units that don't, which reads as a phantom press at power-on. The
     * internal ~45 kΩ pull-up is harmless: we still never drive the pin, so
     * the strap reading at the *next* reset is decided by USB CDC bootloader
     * timing and `idf.py flash` keeps working unattended. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(manual_capture_task, "BootBtn",
                                             3 * 1024 / sizeof(StackType_t),
                                             NULL, 2, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "watcher task create failed");
    }
}

#endif /* CONFIG_BATEAR_ROLE_WIRED_DETECTOR && CONFIG_BATEAR_TF_MANUAL_ENABLE */
