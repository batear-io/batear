# System Architecture

Batear supports two deployment models: **wireless** (LoRa detectors + gateway) and **wired** (Ethernet/PoE direct to MQTT).

## Wireless: LoRa Detector + Gateway

```
┌──────────────────────┐        LoRa 915 MHz          ┌──────────────────────┐
│    DETECTOR (×N)     │ ───────────────────────────► │     GATEWAY (×1)     │
│                      │  AES-128-GCM encrypted       │                      │
│  ICS-43434 mic       │  36-byte packets             │  SSD1306 OLED display│
│  FFT harmonic detect │  alarm / clear / heartbeat   │  LED alarm indicator │
│  Battery monitor     │                              │  SX1262 LoRa RX      │
│  SX1262 LoRa TX      │                              │  WiFi + MQTT TX      │
└──────────────────────┘                              └──────────┬───────────┘
   Heltec WiFi LoRa 32 V3/V4                           Heltec WiFi LoRa 32 V3/V4
                                                                  │
                                                           WiFi / MQTT
                                                                  │
                                                                  ▼
                                                      ┌──────────────────────┐
                                                      │   HOME ASSISTANT     │
                                                      │                      │
                                                      │  Mosquitto broker    │
                                                      │  MQTT Discovery      │
                                                      │  per-detector device │
                                                      │  + diagnostics       │
                                                      └──────────────────────┘
```

Multiple detectors can report to one gateway. Each detector has a unique device ID (0–255) and appears as an individual Home Assistant device with its own diagnostics (battery, uptime, firmware version, free heap, TX failure counter).

Alongside alarm / clear state transitions, each detector emits a **telemetry heartbeat** every 30 minutes (configurable, ±10% jitter) so Home Assistant sees live health data even during silent periods. Heartbeats and alarm/clear events share the same encrypted wire format; the `event_type` byte distinguishes them.

See [LoRa Protocol](protocol.md) for packet layout, retry behaviour, and the no-ACK design rationale, and [Configuration](configuration.md) for MQTT topics, JSON payload schema, and HA entity mapping.

## Wired: Ethernet/PoE Detector

```
┌──────────────────────┐
│  WIRED DETECTOR (×N) │
│                      │       Ethernet (PoE)         ┌──────────────────────┐
│  ICS-43434 mic       │ ───────────────────────────► │   HOME ASSISTANT     │
│  FFT harmonic detect │       MQTT / JSON            │                      │
│  W5500 Ethernet      │                              │  Mosquitto broker    │
│  MQTT TX             │                              │  MQTT Discovery      │
└──────────────────────┘                              │  binary_sensor +     │
   LILYGO T-ETH-Lite S3                               │  confidence sensor   │
                                                      └──────────────────────┘
```

The wired detector bypasses LoRa and the gateway entirely. It connects directly to your network via Ethernet (optionally powered by PoE) and publishes detection events straight to an MQTT broker. This is ideal for fixed installations where Ethernet is available.

## Task Layout (dual-core)

| Role | Core 0 | Core 1 |
|:---|:---|:---|
| Detector | LoRaTask — encrypt + SX1262 TX; wakes on alarm/clear or heartbeat timeout | AudioTask — I2S mic + FFT + detection state machine |
| Gateway | GatewayTask — LoRa RX + decrypt + replay check + OLED + LED | MqttTask — Wi-Fi + MQTT publish + per-detector HA Discovery |
| Wired Detector | EthMqttTask — W5500 Ethernet + MQTT publish + HA Discovery | AudioTask — I2S mic + FFT + detection |

AudioTask pushes `DroneEvent_t` to the LoRaTask over a FreeRTOS queue. LoRaTask blocks on that queue with a heartbeat-interval timeout (±10% jitter); on timeout it synthesises a `DRONE_EVENT_TELEMETRY` and walks the same TX path, so alarm/clear and heartbeat share one pipeline. Battery voltage is read fresh on every TX via the `battery` module — GPIO37 gates the divider so the 100 µA divider current only flows during a ~2 ms measurement window.

GatewayTask sends `MqttEvent_t` items to MqttTask via a FreeRTOS queue, so LoRa reception is never blocked by network I/O. The wired detector uses `DroneEvent_t` items directly from AudioTask's queue.

## Project Structure

```text
batear/
├── CMakeLists.txt
├── sdkconfig.defaults          # common ESP-IDF settings
├── sdkconfig.detector          # detector role + device ID + network
├── sdkconfig.gateway           # gateway role + network + MQTT/WiFi
├── sdkconfig.wired_detector    # wired detector role + MQTT config
├── main/
│   ├── CMakeLists.txt          # conditional compile by role
│   ├── Kconfig.projbuild       # role / device ID / network / MQTT config
│   ├── main.cpp                # entry point (role switch)
│   ├── pin_config.h            # board-specific GPIO + hardware traits
│   ├── drone_detector.h        # shared DroneEvent_t + queue
│   ├── lora_crypto.h           # AES-128-GCM packet protocol (PSA API)
│   ├── EspIdfHal.cpp/.h        # RadioLib HAL for ESP-IDF
│   ├── audio_processor.c/.h    # [detector/wired] ESP-DSP FFT + PSD + harmonic analysis
│   ├── audio_task.c/.h         # [detector/wired] I2S mic + detection state machine
│   ├── battery.c/.h            # [detector] VBAT ADC + divider gating
│   ├── lora_task.cpp/.h        # [detector] LoRa TX (alarm/clear/heartbeat + local retry)
│   ├── eth_mqtt_task.cpp/.h    # [wired]    W5500 Ethernet + MQTT + HA Discovery
│   ├── gateway_task.cpp/.h     # [gateway]  LoRa RX + OLED + LED
│   ├── mqtt_task.cpp/.h        # [gateway]  WiFi + MQTT + HA Discovery
│   ├── oled.c/.h               # [gateway]  SSD1306 128x64 driver
│   └── idf_component.yml       # RadioLib + ESP-DSP + W5500 dependencies
```
