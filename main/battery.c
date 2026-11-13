/*
 * battery.c — ADC-oneshot readback of a resistor-divided VBAT rail
 *
 * Heltec V3/V4 wire VBAT through a ~4.9x divider into GPIO1 (ADC1_CH0) gated
 * by GPIO37 (ADC_Ctrl).  V3: active LOW (drive low to connect divider).
 *                        V4: active HIGH (drive high to connect divider).
 * We connect the divider only while sampling to avoid a constant ~100 uA drain.
 */

#include "battery.h"
#include "pin_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

#if defined(BOARD_HAS_VBAT) && BOARD_HAS_VBAT
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

static const char *TAG = "battery";

#if defined(BOARD_HAS_VBAT) && BOARD_HAS_VBAT

/* GPIO1 is ADC1_CH0 on ESP32-S3; keep this coupled to PIN_VBAT_ADC as a
 * sanity check so a future pin-map change fails loudly at build time. */
#if PIN_VBAT_ADC != 1
#error "battery.c assumes PIN_VBAT_ADC == 1 (ADC1_CH0) on ESP32-S3"
#endif

#define VBAT_ADC_CHANNEL  ADC_CHANNEL_0
#define VBAT_ADC_UNIT     ADC_UNIT_1
#define VBAT_ADC_ATTEN    ADC_ATTEN_DB_12
#define VBAT_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define VBAT_SAMPLES      8

/* ADC_Ctrl (GPIO37) polarity differs between board revisions:
 *   V3.0/3.1 — active LOW:  drive LOW  to connect the divider.
 *   V3.2, V4 — active HIGH: drive HIGH to connect the divider.
 * V3 boards don't report their revision, so battery_init() probes both
 * levels and keeps the one that actually puts VBAT on the ADC pin. */
#if defined(CONFIG_BATEAR_BOARD_HELTEC_V4)
static int s_ctrl_connect = 1;
#else
static int s_ctrl_connect = 0;
#endif
#define VBAT_CTRL_CONNECT    (s_ctrl_connect)
#define VBAT_CTRL_DISCONNECT (!s_ctrl_connect)

/* Settle time after enabling the divider before sampling (ms) */
#ifndef VBAT_SETTLE_MS
#define VBAT_SETTLE_MS 5
#endif

static adc_oneshot_unit_handle_t s_adc   = NULL;
static adc_cali_handle_t         s_cali  = NULL;
static bool                      s_ready = false;
/* Serialises reads: the divider is gated per read, so two overlapping
 * readers (LoRa telemetry + OLED header) would cut each other off. */
static SemaphoreHandle_t          s_lock  = NULL;

static bool cali_init(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .chan     = VBAT_ADC_CHANNEL,
        .atten   = VBAT_ADC_ATTEN,
        .bitwidth = VBAT_ADC_BITWIDTH,
    };
    return adc_cali_create_scheme_curve_fitting(&cfg, &s_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .atten   = VBAT_ADC_ATTEN,
        .bitwidth = VBAT_ADC_BITWIDTH,
    };
    return adc_cali_create_scheme_line_fitting(&cfg, &s_cali) == ESP_OK;
#else
    return false;
#endif
}

void battery_init(void)
{
    if (s_ready) {
        return;
    }
    gpio_config_t ctrl_cfg = {
        .pin_bit_mask   = (1ULL << PIN_VBAT_CTRL),
        .mode           = GPIO_MODE_OUTPUT,
        .pull_up_en     = GPIO_PULLUP_DISABLE,
        .pull_down_en   = GPIO_PULLDOWN_DISABLE,
        .intr_type      = GPIO_INTR_DISABLE,
    };
    gpio_config(&ctrl_cfg);
    gpio_set_level((gpio_num_t)PIN_VBAT_CTRL, VBAT_CTRL_DISCONNECT); /* divider disconnected */

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = VBAT_ADC_ATTEN,
        .bitwidth = VBAT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc, VBAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        return;
    }
    /* cppcheck cannot see ESP-IDF's ADC_CALI_SCHEME_*_SUPPORTED macros
     * (defined via soc_caps.h from sdkconfig), so under its preprocessor
     * view both #if branches in cali_init() compile out and it concludes
     * the function always returns false. On real builds at least one
     * scheme is available. */
    // cppcheck-suppress knownConditionTrueFalse
    if (!cali_init()) {
        ESP_LOGW(TAG, "ADC calibration unavailable — readings will be rough");
    }
    s_lock  = xSemaphoreCreateMutex();
    s_ready = (s_lock != NULL);

#if !defined(CONFIG_BATEAR_BOARD_HELTEC_V4)
    /* Wrong polarity leaves the divider open and the pin reads ~0 V. If the
     * default reads near zero, try the other level and keep whichever is
     * higher (no battery and no USB → both ~0, default stays). */
    if (s_ready) {
        uint32_t mv_default = battery_read_mv();
        if (mv_default < 1000) {
            s_ctrl_connect = !s_ctrl_connect;
            uint32_t mv_flipped = battery_read_mv();
            if (mv_flipped <= mv_default) {
                s_ctrl_connect = !s_ctrl_connect;
            }
        }
        gpio_set_level((gpio_num_t)PIN_VBAT_CTRL, VBAT_CTRL_DISCONNECT);
    }
#endif
    ESP_LOGI(TAG, "battery monitor ready (adc=GPIO%d ctrl=GPIO%d active-%s ratio=%.2f)",
             PIN_VBAT_ADC, PIN_VBAT_CTRL, s_ctrl_connect ? "high" : "low",
             (double)BOARD_VBAT_DIVIDER_RATIO);
}

uint32_t battery_read_mv(void)
{
    if (!s_ready) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_set_level((gpio_num_t)PIN_VBAT_CTRL, VBAT_CTRL_CONNECT);
    vTaskDelay(pdMS_TO_TICKS(VBAT_SETTLE_MS));
    uint32_t raw_sum = 0;
    int      raw     = 0;
    int      taken   = 0;
    for (int i = 0; i < VBAT_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &raw) == ESP_OK) {
            raw_sum += (uint32_t)raw;
            taken++;
        }
    }
    gpio_set_level((gpio_num_t)PIN_VBAT_CTRL, VBAT_CTRL_DISCONNECT);
    xSemaphoreGive(s_lock);
    if (taken == 0) {
        return 0;
    }
    int raw_avg   = (int)(raw_sum / (uint32_t)taken);
    int mv_at_pin = 0;
    if (s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, raw_avg, &mv_at_pin) != ESP_OK) {
            mv_at_pin = 0;
        }
    } else {
        mv_at_pin = (raw_avg * 3100) / 4095;
    }
    float vbat_mv = (float)mv_at_pin * BOARD_VBAT_DIVIDER_RATIO;
    if (vbat_mv < 0.0f)     vbat_mv = 0.0f;
    if (vbat_mv > 65535.0f) vbat_mv = 65535.0f;
    return (uint32_t)(vbat_mv + 0.5f);
}

void battery_prepare_sleep(void)
{
    gpio_set_level((gpio_num_t)PIN_VBAT_CTRL, VBAT_CTRL_DISCONNECT);
    gpio_hold_en((gpio_num_t)PIN_VBAT_CTRL);
}

#else /* !BOARD_HAS_VBAT */

void     battery_init(void)    { }
void     battery_prepare_sleep(void) { }
uint32_t battery_read_mv(void) { return 0; }

#endif /* BOARD_HAS_VBAT */
