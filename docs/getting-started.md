# Prerequisites

!!! note "Don't want to install ESP-IDF?"
    Use the [Web Flasher](https://docs.batear.io/flasher/){ target="_blank" } to flash pre-built firmware directly from your browser. The prerequisites below are only needed if you want to build from source or customize configuration.

## Requirements

- **ESP-IDF v6.x** — follow the [official installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Source the environment before building:

```bash
. $IDF_PATH/export.sh
```

## Supported Boards

| Board | Chip | `set-target` | Flash | Roles |
|:---|:---|:---|:---|:---|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | `esp32s3` | 8 MB | Detector, Gateway |
| Heltec WiFi LoRa 32 V4 | ESP32-S3 | `esp32s3` | 16 MB | Detector, Gateway |
| LILYGO T-ETH-Lite S3 | ESP32-S3 | `esp32s3` | 16 MB | Wired Detector |

## Next Steps

Once you have ESP-IDF installed and your board ready, head to [Build & Flash](build-flash.md) to compile and upload the firmware.
