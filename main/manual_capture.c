/*
 * manual_capture.c — BOOT-button (GPIO 0) state machine.
 *
 * 20 ms polling loop on Core 0. We deliberately avoid `iot_button` /
 * `espressif/button` to keep the dependency surface flat — debouncing a
 * single GPIO is trivial here.
 */

#include "manual_capture.h"

#if CONFIG_BATEAR_TF_RECORD_ENABLE && CONFIG_BATEAR_TF_MANUAL_ENABLE

#include "tf_recorder.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "boot_btn";

#define BOOT_GPIO          ((gpio_num_t)0)
#define POLL_PERIOD_MS     20
#define DEBOUNCE_MS        40
#define SHORT_PRESS_MAX_MS 400

static bool s_started;

static void manual_capture_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "BOOT button watcher start (core %d) hold=%dms",
             xPortGetCoreID(), CONFIG_BATEAR_TF_MANUAL_HOLD_MS);

    bool      was_pressed       = false;
    int64_t   press_start_us    = 0;
    bool      long_press_fired  = false;
    bool      manual_active     = false;  /* tracks whether we triggered a manual recording */

    for (;;) {
        /* GPIO 0 idles HIGH on T-ETH-Lite S3 (board has external pull-up).
         * Pressed = LOW. We sample twice with DEBOUNCE_MS gap to filter glitches. */
        int level1 = gpio_get_level(BOOT_GPIO);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS / 2));
        int level2 = gpio_get_level(BOOT_GPIO);
        bool pressed = (level1 == 0) && (level2 == 0);
        int64_t now = esp_timer_get_time();

        if (pressed && !was_pressed) {
            /* edge: high -> low */
            press_start_us   = now;
            long_press_fired = false;
        } else if (pressed && was_pressed) {
            /* held — fire long-press once when the threshold is reached */
            int64_t held_ms = (now - press_start_us) / 1000;
            if (!long_press_fired &&
                held_ms >= (int64_t)CONFIG_BATEAR_TF_MANUAL_HOLD_MS) {
                long_press_fired = true;
                if (!manual_active) {
                    ESP_LOGI(TAG, "long-press → MANUAL REC start");
                    tf_recorder_send_cmd(TF_CMD_MANUAL_START);
                    manual_active = true;
                } else {
                    ESP_LOGI(TAG, "long-press during manual rec — ignored");
                }
            }
        } else if (!pressed && was_pressed) {
            /* edge: low -> high (release) */
            int64_t held_ms = (now - press_start_us) / 1000;
            if (!long_press_fired && held_ms <= SHORT_PRESS_MAX_MS) {
                if (manual_active) {
                    ESP_LOGI(TAG, "short-press → MANUAL REC stop");
                    tf_recorder_send_cmd(TF_CMD_MANUAL_STOP);
                    manual_active = false;
                } else {
                    ESP_LOGD(TAG, "short-press ignored (no active manual rec)");
                }
            }
            /* If we fired a long-press, the recording is auto-stopped by the
             * recorder's own timeout or by another short-press. */
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS - DEBOUNCE_MS / 2));
    }
}

void manual_capture_init(void)
{
    if (s_started) return;
    s_started = true;

    /* INPUT only, no internal pull (board has external pull-up). Crucially
     * we never drive this pin, so the strap reading at next reset is
     * unaffected and `idf.py flash` keeps working. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
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

#endif /* CONFIG_BATEAR_TF_RECORD_ENABLE && CONFIG_BATEAR_TF_MANUAL_ENABLE */
