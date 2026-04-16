# System Architecture

```
┌──────────────────────┐        LoRa 915 MHz          ┌──────────────────────┐
│    DETECTOR (×N)     │ ───────────────────────────► │     GATEWAY (×1)     │
│                      │  AES-128-GCM encrypted       │                      │
│  ICS-43434 mic       │  28-byte packets             │  SSD1306 OLED display│
│  FFT harmonic detect │                              │  LED alarm indicator │
│  SX1262 LoRa TX      │                              │  SX1262 LoRa RX      │
└──────────────────────┘                              │  WiFi + MQTT TX      │
   Heltec WiFi LoRa 32 V3/V4                          └──────────┬───────────┘
                                                        Heltec WiFi LoRa 32 V3/V4
                                                                  │
                                                           WiFi / MQTT
                                                                  │
                                                                  ▼
                                                      ┌──────────────────────┐
                                                      │   HOME ASSISTANT     │
                                                      │                      │
                                                      │  Mosquitto broker    │
                                                      │  MQTT Discovery      │
                                                      │  binary_sensor +     │
                                                      │  RSSI / SNR sensors  │
                                                      └──────────────────────┘
```

Multiple detectors can report to one gateway. Each detector has a unique device ID (0–255).
The gateway forwards detection events to Home Assistant via MQTT over Wi-Fi (see [Configuration](configuration.md) for setup).

## Task Layout (dual-core)

| Role | Core 0 | Core 1 |
|:---|:---|:---|
| Detector | LoRaTask — encrypt + SX1262 TX | AudioTask — I2S mic + FFT + detection |
| Gateway | GatewayTask — LoRa RX + decrypt + OLED + LED | MqttTask — Wi-Fi + MQTT publish + HA Discovery |

GatewayTask sends `MqttEvent_t` items to MqttTask via a FreeRTOS queue, so LoRa reception is never blocked by network I/O.

## Project Structure

```text
batear/
├── CMakeLists.txt
├── sdkconfig.defaults          # common ESP-IDF settings
├── sdkconfig.detector          # detector role + device ID + network
├── sdkconfig.gateway           # gateway role + network + MQTT/WiFi
├── main/
│   ├── CMakeLists.txt          # conditional compile by role
│   ├── Kconfig.projbuild       # role / device ID / network / MQTT config
│   ├── main.cpp                # entry point (role switch)
│   ├── pin_config.h            # board-specific GPIO + hardware traits
│   ├── drone_detector.h        # shared DroneEvent_t + queue
│   ├── lora_crypto.h           # AES-128-GCM packet protocol (PSA API)
│   ├── EspIdfHal.cpp/.h        # RadioLib HAL for ESP-IDF
│   ├── audio_processor.c/.h    # [detector] ESP-DSP FFT + PSD + harmonic analysis
│   ├── audio_task.c/.h         # [detector] I2S mic + detection state machine
│   ├── lora_task.cpp/.h        # [detector] LoRa TX
│   ├── gateway_task.cpp/.h     # [gateway]  LoRa RX + OLED + LED
│   ├── mqtt_task.cpp/.h        # [gateway]  WiFi + MQTT + HA Discovery
│   ├── oled.c/.h               # [gateway]  SSD1306 128x64 driver
│   └── idf_component.yml       # RadioLib + ESP-DSP dependencies
```
