/*
 * EspIdfHal.cpp — RadioLibHal implementation for pure ESP-IDF 5.x / 6.x
 */

#include "EspIdfHal.h"

#include <string.h>
#include "esp_timer.h"
#include "esp_rom_sys.h"           /* esp_rom_delay_us()  */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "hal";

/* =========================================================================
 * Constructor
 * ====================================================================== */
EspIdfHal::EspIdfHal(uint8_t sck, uint8_t miso, uint8_t mosi)
    : RadioLibHal(
        /*input*/           GPIO_MODE_INPUT,
        /*output*/          GPIO_MODE_OUTPUT,
        /*low*/             0,
        /*high*/            1,
        /*rising*/          GPIO_INTR_POSEDGE,
        /*falling*/         GPIO_INTR_NEGEDGE
    ),
    _sck(sck), _miso(miso), _mosi(mosi)
{}

/* =========================================================================
 * Lifecycle — called by RadioLib's Module::init() / ~Module()
 * ====================================================================== */

// cppcheck-suppress unusedFunction
void EspIdfHal::init()
{
    spiBegin();
}

// cppcheck-suppress unusedFunction
void EspIdfHal::term()
{
    spiEnd();
}

/* =========================================================================
 * GPIO
 * ====================================================================== */

// cppcheck-suppress unusedFunction
void EspIdfHal::pinMode(uint32_t pin, uint32_t mode)
{
    gpio_config_t io = {
        .pin_bit_mask  = (1ULL << pin),
        .mode          = static_cast<gpio_mode_t>(mode),
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

// cppcheck-suppress unusedFunction
void EspIdfHal::digitalWrite(uint32_t pin, uint32_t value)
{
    gpio_set_level(static_cast<gpio_num_t>(pin), value);
}

// cppcheck-suppress unusedFunction
uint32_t EspIdfHal::digitalRead(uint32_t pin)
{
    return static_cast<uint32_t>(gpio_get_level(static_cast<gpio_num_t>(pin)));
}

/* =========================================================================
 * Interrupts
 *
 * RadioLib calls attachInterrupt() for the DIO1 line after begin().
 * ESP-IDF requires gpio_install_isr_service() to be called once globally
 * before any gpio_isr_handler_add() calls.
 * ====================================================================== */

// cppcheck-suppress unusedFunction
void EspIdfHal::attachInterrupt(uint32_t interruptNum,
                                 void (*interruptCb)(void),
                                 uint32_t mode)
{
    if (!_isr_service_installed) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            /* ESP_ERR_INVALID_STATE means already installed — that's fine. */
            ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
        }
        _isr_service_installed = true;
    }

    gpio_set_intr_type(static_cast<gpio_num_t>(interruptNum),
                       static_cast<gpio_int_type_t>(mode));
    gpio_isr_handler_add(static_cast<gpio_num_t>(interruptNum),
                         reinterpret_cast<gpio_isr_t>(interruptCb),
                         nullptr);
    gpio_intr_enable(static_cast<gpio_num_t>(interruptNum));
}

// cppcheck-suppress unusedFunction
void EspIdfHal::detachInterrupt(uint32_t interruptNum)
{
    gpio_isr_handler_remove(static_cast<gpio_num_t>(interruptNum));
    gpio_intr_disable(static_cast<gpio_num_t>(interruptNum));
}

/* =========================================================================
 * Timing
 * ====================================================================== */

// cppcheck-suppress unusedFunction
void EspIdfHal::yield()
{
    vTaskDelay(pdMS_TO_TICKS(1));
}

// cppcheck-suppress unusedFunction
void EspIdfHal::delay(RadioLibTime_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// cppcheck-suppress unusedFunction
void EspIdfHal::delayMicroseconds(RadioLibTime_t us)
{
    esp_rom_delay_us(static_cast<uint32_t>(us));
}

// cppcheck-suppress unusedFunction
RadioLibTime_t EspIdfHal::millis()
{
    return static_cast<RadioLibTime_t>(esp_timer_get_time() / 1000LL);
}

// cppcheck-suppress unusedFunction
RadioLibTime_t EspIdfHal::micros()
{
    return static_cast<RadioLibTime_t>(esp_timer_get_time());
}

// cppcheck-suppress unusedFunction
long EspIdfHal::pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout)
{
    /*
     * Wait for pin to reach the desired state, then measure how long it
     * stays there.  Used by RadioLib for CAD / channel-activity detection;
     * not exercised by our simple TX-only flow.
     */
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout;
    const auto    gpio     = static_cast<gpio_num_t>(pin);

    /* Wait for the opposite state first (pre-edge sync) */
    while (gpio_get_level(gpio) == (int)state) {
        if (esp_timer_get_time() >= deadline) return 0;
    }
    /* Wait for the target state to start */
    // cppcheck-suppress knownConditionTrueFalse
    while (gpio_get_level(gpio) != (int)state) {
        if (esp_timer_get_time() >= deadline) return 0;
    }
    const int64_t pulse_start = esp_timer_get_time();
    /* Measure how long the target state lasts */
    // cppcheck-suppress knownConditionTrueFalse
    while (gpio_get_level(gpio) == (int)state) {
        if (esp_timer_get_time() >= deadline) return 0;
    }
    return static_cast<long>(esp_timer_get_time() - pulse_start);
}

/* =========================================================================
 * SPI
 *
 * RadioLib drives CS manually via digitalWrite(), so spics_io_num = -1.
 *
 * DMA is disabled: SX1262 register-level SPI transactions are small (a
 * few bytes), and the 1-byte LoRa payload keeps FIFO writes well within
 * SOC_SPI_MAXIMUM_BUFFER_SIZE (64 B on ESP32-S3).  Disabling DMA avoids
 * GDMA channel allocation and alignment requirements entirely.
 *
 * spi_device_acquire_bus / release_bus wrap every transaction because
 * ESP-IDF 5.x+ polling mode requires explicit bus ownership.
 * ====================================================================== */

void EspIdfHal::spiBegin()
{
    /* Zero-init, then explicitly set EVERY gpio field to -1 ("not used").
     * ESP-IDF 6.x validates every data pin field; a zero value is treated
     * as GPIO 0 (BOOT button), causing ESP_ERR_INVALID_ARG. */
    spi_bus_config_t bus = {};
    bus.mosi_io_num     = _mosi;
    bus.miso_io_num     = _miso;
    bus.sclk_io_num     = _sck;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.data4_io_num    = -1;
    bus.data5_io_num    = -1;
    bus.data6_io_num    = -1;
    bus.data7_io_num    = -1;
    bus.max_transfer_sz = 0;

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi_bus_initialize: bus already claimed, proceeding");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return;
    }

    spi_device_interface_config_t dev = {};
    dev.mode           = 0;                 /* SX1262: CPOL=0, CPHA=0       */
    dev.clock_speed_hz = 2 * 1000 * 1000;  /* 2 MHz — conservative start    */
    dev.spics_io_num   = -1;               /* CS driven by RadioLib via GPIO */
    dev.queue_size     = 1;

    err = spi_bus_add_device(SPI2_HOST, &dev, &_spi_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SPI bus ready  MOSI=%d MISO=%d SCK=%d",
                 (int)_mosi, (int)_miso, (int)_sck);
    }
}

// cppcheck-suppress unusedFunction
void EspIdfHal::spiBeginTransaction()
{
    if (_spi_device) {
        spi_device_acquire_bus(_spi_device, portMAX_DELAY);
    }
}

// cppcheck-suppress unusedFunction
void EspIdfHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in)
{
    if (!_spi_device) {
        if (!_spi_error_logged) {
            ESP_LOGE(TAG, "spiTransfer: no device handle (spiBegin failed)");
            _spi_error_logged = true;
        }
        if (in) memset(in, 0xFF, len);
        return;
    }
    if (len == 0) return;

    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = out;
    t.rx_buffer = in;

    esp_err_t err = spi_device_polling_transmit(_spi_device, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_polling_transmit(%u B): %s",
                 (unsigned)len, esp_err_to_name(err));
    }
}

// cppcheck-suppress unusedFunction
void EspIdfHal::spiEndTransaction()
{
    if (_spi_device) {
        spi_device_release_bus(_spi_device);
    }
}

void EspIdfHal::spiEnd()
{
    if (_spi_device) {
        spi_bus_remove_device(_spi_device);
        _spi_device = nullptr;
    }
    spi_bus_free(SPI2_HOST);
}
