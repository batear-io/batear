/*
 * pin_config.h — board-specific GPIO and hardware trait definitions
 *
 * All board-specific constants live here, selected by BATEAR_BOARD Kconfig.
 * No other source file should hard-code pin numbers or board constants.
 *
 * To add a new board:
 *   1. Kconfig.projbuild  — add a config entry under BATEAR_BOARD
 *   2. This file           — add an #elif block with all PIN_* and BOARD_* macros
 *   3. sdkconfig.detector / sdkconfig.gateway — set board + flash size
 *   4. Build               — use `set-target <BOARD_IDF_TARGET>` for the chip
 *   See README.md "Adding a New Board" for the full guide.
 */
#pragma once

/* =====================================================================
 * Board: Heltec WiFi LoRa 32 V3 / V4  (ESP32-S3 + SX1262 + SSD1306)
 * =====================================================================
 *
 * V4 is pin-compatible with V3 (adds GC1109 FEM, 16 MB flash, solar
 * input).  The same GPIO map works for both boards.
 *
 * Occupied by on-board hardware — do NOT use for I2S:
 *   GPIO 8..14  — SX1262 SPI + control
 *   GPIO 17,18,21 — OLED I2C + RST
 *   GPIO 19,20  — USB
 *   GPIO 35     — LED
 *   GPIO 36     — Vext power
 *   GPIO 43,44  — UART0
 *
 * Free pins for I2S mic:
 *   GPIO 2..7, 26, 33..34, 37..42, 45..48
 * ===================================================================== */
#if defined(CONFIG_BATEAR_BOARD_HELTEC_V3) || defined(CONFIG_BATEAR_BOARD_HELTEC_V4)

#define BOARD_IDF_TARGET    "esp32s3"
#define BOARD_FLASH_SIZE    "8MB"

/* I2S microphone (ICS-43434) */
#define PIN_I2S_BCLK     4
#define PIN_I2S_WS       5
#define PIN_I2S_DIN      6

/* LoRa SX1262 SPI — fixed on-board wiring */
#define PIN_LORA_SCK     9
#define PIN_LORA_MISO   11
#define PIN_LORA_MOSI   10
#define PIN_LORA_CS      8
#define PIN_LORA_DIO1   14
#define PIN_LORA_RST    12
#define PIN_LORA_BUSY   13

/* Gateway peripherals */
#define PIN_OLED_SDA    17
#define PIN_OLED_SCL    18
#define PIN_OLED_RST    21
#define PIN_LED         35
#define PIN_VEXT        36
#define BOARD_HAS_VEXT   1
#define BOARD_HAS_OLED   1

/* No on-board SD card on Heltec V3/V4 */
#define BOARD_HAS_SDMMC  0

/* LoRa RF traits */
#define BOARD_LORA_TCXO_V       1.8f
#define BOARD_LORA_DIO2_AS_RF   true

/* Battery monitor — Heltec V3/V4 route VBAT through a resistor divider to GPIO1.
 * The divider is gated by GPIO37 (ADC_Ctrl): drive LOW to read, HIGH to disconnect
 * (prevents the divider from idling ~100 µA off the cell). Ratio measured at 4.9×.
 */
#define PIN_VBAT_ADC              1
#define PIN_VBAT_CTRL            37
#define BOARD_HAS_VBAT            1
#define BOARD_VBAT_DIVIDER_RATIO  4.9f

/* =====================================================================
 * Board: LILYGO T-ETH-Lite S3  (ESP32-S3 + W5500 Ethernet)
 * =====================================================================
 *
 * Occupied by on-board hardware — do NOT use for I2S:
 *   GPIO 9..14  — W5500 Ethernet SPI + control
 *   GPIO 5,6,7,42 — SD card SPI
 *   GPIO 19,20  — USB
 *   GPIO 43,44  — UART0
 *
 * Free pins for I2S mic (on extension headers):
 *   GPIO 0..4, 8, 15..18, 21, 38..41, 45..48
 * ===================================================================== */
#elif defined(CONFIG_BATEAR_BOARD_LILYGO_T_ETH_LITE_S3)

#define BOARD_IDF_TARGET    "esp32s3"
#define BOARD_FLASH_SIZE    "16MB"

/* I2S microphone (ICS-43434) — wired to extension headers */
#define PIN_I2S_BCLK    38
#define PIN_I2S_WS      39
#define PIN_I2S_DIN     40

/* W5500 Ethernet SPI — fixed on-board wiring */
#define PIN_ETH_SCLK    10
#define PIN_ETH_MOSI    12
#define PIN_ETH_MISO    11
#define PIN_ETH_CS       9
#define PIN_ETH_INT     13
#define PIN_ETH_RST     14
#define PIN_ETH_ADDR     1

/* On-board microSD slot — SDMMC 1-bit mode (CLK / CMD / D0 only).
 * The fourth historical SD pin (GPIO 42) is the D3/CS line in 4-bit / SPI mode
 * and is left unconfigured in 1-bit mode so it doesn't fight the slot's
 * internal pull-up. SDMMC uses its own dedicated controller (not SPI), so
 * there is no bus conflict with the W5500 on SPI2.
 */
#define PIN_SD_CLK      6
#define PIN_SD_CMD      5
#define PIN_SD_D0       7
#define BOARD_HAS_SDMMC 1

/* No gateway peripherals on this board */
#define BOARD_HAS_VEXT   0
#define BOARD_HAS_OLED   0
#define BOARD_HAS_ETH    1

/* No LoRa radio — dummy values to satisfy shared headers */
#define BOARD_LORA_TCXO_V       0.0f
#define BOARD_LORA_DIO2_AS_RF   false

/* No battery monitor on this board (USB/PoE powered) */
#define BOARD_HAS_VBAT           0

/* ===================================================================== */

#else
#error "No board selected — set BATEAR_BOARD in menuconfig or sdkconfig"
#endif

/* -------------------------------------------------------------------
 * LoRa network config (board-independent)
 *
 * Only the Detector and Gateway roles encrypt over LoRa; CONFIG_BATEAR_NET_KEY
 * is gated to those roles in Kconfig, so the BATEAR_NET_KEY hex-array macro
 * is only emitted when that string is present in sdkconfig.
 * ----------------------------------------------------------------- */

#define _HEX_NIBBLE(c) \
    (((c) >= '0' && (c) <= '9') ? ((c) - '0') : \
     ((c) >= 'A' && (c) <= 'F') ? ((c) - 'A' + 10) : \
     ((c) >= 'a' && (c) <= 'f') ? ((c) - 'a' + 10) : 0)

#define _HEX_BYTE(s, i) \
    (uint8_t)((_HEX_NIBBLE((s)[2*(i)]) << 4) | _HEX_NIBBLE((s)[2*(i)+1]))

#ifdef CONFIG_BATEAR_NET_KEY
#define BATEAR_NET_KEY { \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 0), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 1), \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 2), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 3), \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 4), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 5), \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 6), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 7), \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 8), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 9), \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 10), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 11), \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 12), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 13), \
    _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 14), _HEX_BYTE(CONFIG_BATEAR_NET_KEY, 15), \
}
#endif
