# Hardware

## Detector

| Component | Notes |
|:---|:---|
| Heltec WiFi LoRa 32 V3 / V4 | ESP32-S3 + SX1262 on-board |
| ICS-43434 I2S MEMS microphone | 3.3 V, L/R → GND (left channel) |

### Wiring (ICS-43434 → Heltec V3 / V4)

| ICS-43434 | GPIO | Function |
|:---|:---|:---|
| VDD | 3.3V | Power |
| GND | GND | Ground |
| SCK | 4 | I2S bit clock (BCLK) |
| WS | 5 | I2S word select (LRCLK) |
| SD | 6 | I2S data input (DIN) |
| L/R | GND | Left channel select |

### Battery monitoring (optional)

Heltec V3 / V4 ship with an on-board LiPo jack, charger, and a resistor divider that taps VBAT onto **GPIO1 (ADC1_CH0)**, gated by **GPIO37 (ADC_Ctrl, active low)**. The firmware drives the gate low only during a ~2 ms measurement window on every LoRa TX, so the divider's ~100 µA current drain is effectively eliminated between reads.

No extra wiring is required — plug a 1S LiPo into the JST connector and the detector reports `battery_v` (plus a low-battery flag below 3.4 V) in every telemetry packet. Boards without this divider can simply leave `BOARD_HAS_VBAT` at `0` in `pin_config.h` and the battery module compiles down to no-ops (readings are reported as 0 V).

## Wired Detector (Ethernet/PoE)

| Component | Notes |
|:---|:---|
| LILYGO T-ETH-Lite S3 | ESP32-S3 + W5500 Ethernet on-board, optional PoE expansion |
| ICS-43434 I2S MEMS microphone | 3.3 V, L/R → GND (left channel) |

### Wiring (ICS-43434 → T-ETH-Lite S3)

| ICS-43434 | GPIO | Function |
|:---|:---|:---|
| VDD | 3.3V | Power |
| GND | GND | Ground |
| SCK | 38 | I2S bit clock (BCLK) |
| WS | 39 | I2S word select (LRCLK) |
| SD | 40 | I2S data input (DIN) |
| L/R | GND | Left channel select |

!!! tip
    GPIO 38/39/40 are on the extension headers of the T-ETH-Lite S3. These pins avoid conflicts with the on-board W5500 Ethernet (GPIO 9–14) and SD card (GPIO 5–7, 42).

The wired detector connects directly to your network via Ethernet (RJ45). If you use a PoE expansion board, power and data come through a single cable — ideal for permanent outdoor installations.

!!! note "IP addressing"
    By default the wired detector uses DHCP. For fixed installations you can assign a static IP via the [serial console](configuration.md#serial-console) (`set eth_ip 192.168.1.50`) or at build time in `sdkconfig.wired_detector`. See [Configuration → Ethernet static IP](configuration.md#wired-detector-config) for details.

!!! tip "OTA firmware updates"
    The wired detector includes a built-in REST API (port 8080) that supports over-the-air firmware updates with automatic rollback. Upload a new binary with `curl -X POST --data-binary @firmware.bin http://<ip>:8080/api/ota`. See [Configuration → REST API](configuration.md#rest-api-wired-detector-only) for all endpoints.

## Gateway

| Component | Notes |
|:---|:---|
| Heltec WiFi LoRa 32 V3 / V4 | Uses on-board SX1262, SSD1306 OLED, and LED |

No external wiring needed — everything is on-board.

