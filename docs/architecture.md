# System Architecture

Batear supports two deployment models: **wireless** (a LoRa mesh of detectors, optional relays and one or more gateways) and **wired** (Ethernet/PoE direct to MQTT).

## Wireless: LoRa mesh

```
                               direct, when in range
          ┌─────────────────────────────────────────────────────────────┐
          │                                                             ▼
┌─────────┴────────┐          ┌──────────────────┐          ┌──────────────────────┐
│ DETECTOR (×N)    │          │ RELAY (×0..N)    │          │ GATEWAY (×1..N)      │
│                  │   LoRa   │ optional         │   LoRa   │                      │
│ ICS-43434 mic    │ ───────► │                  │ ───────► │ SX1262 LoRa RX       │
│ FFT detection    │          │ re-transmits     │          │ SSD1306 OLED + LED   │
│ Battery monitor  │          │ others' packets  │          │ Wi-Fi + MQTT         │
│ SX1262 LoRa TX   │          │ up to 3 hops     │          │ relay (optional)     │
└──────────────────┘          └──────────────────┘          └───────────┬──────────┘
                                                                        │ MQTT
                                                                        ▼
                                                            ┌──────────────────────┐
                                                            │ HOME ASSISTANT       │
                                                            │                      │
                                                            │ one device per       │
                                                            │ detector, whichever  │
                                                            │ gateway heard it     │
                                                            └──────────────────────┘

Every LoRa box is a Heltec WiFi LoRa 32 V3/V4 running the same node
firmware; its role comes from capabilities set at flash time.
Packets: AES-128-GCM, 38 bytes.
```

All LoRa boxes run the **same LoRa node firmware**. What a node does is decided at boot from four capabilities in NVS (`node_cfg`), which the web flasher's Detector / Gateway / Relay tabs set for you:

| Capability | Detector | Relay | Gateway |
|:---|:---:|:---:|:---:|
| `mic` — run the audio detector, transmit alarm / clear / heartbeat | ✓ | | |
| `rx` — listen, show received events on OLED + LED | | ✓ | ✓ |
| `relay` — re-transmit other nodes' packets | | ✓ | optional |
| `bridge` — publish to MQTT over Wi-Fi | | | ✓ |

Combinations are allowed — e.g. a mains-powered detector that also relays. See [Configuration → Node capabilities](configuration.md#serial-console).

- **Detectors** each have a unique device ID (0–255). Besides alarm / clear transitions they send a **telemetry heartbeat** every 30 minutes (configurable, ±10% jitter), so Home Assistant sees battery, uptime, firmware version, free heap and TX failures even during silent periods.
- **Relays** forward packets they hear from other nodes (controlled flooding with dedup, TTL 3, slotted backoff, suppression and an hourly airtime budget), so a detector out of range of every gateway still gets through. They listen continuously and should be mains-powered.
- **Gateways** (bridges) decrypt packets and publish them to MQTT. Any number can run at once: detector topics and HA discovery don't depend on which gateway heard a packet, so Home Assistant sees one device per detector and one state change per event, and alarms keep arriving if a gateway goes offline.

See [LoRa Protocol](protocol.md) for the frame format, replay protection and mesh relay, and [Configuration](configuration.md) for MQTT topics, JSON payloads and HA entities.

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

| Build | Core 0 | Core 1 |
|:---|:---|:---|
| LoRa node | LoRaTask — owns the SX1262: encrypt + TX own events, RX → decrypt + replay check + OLED/LED, relay queue | AudioTask — I2S mic + FFT + detection *(if `mic`)*; MqttTask — Wi-Fi + MQTT + HA Discovery *(if `bridge`)* |
| Wired Detector | EthMqttTask — W5500 Ethernet + MQTT publish + HA Discovery | AudioTask — I2S mic + FFT + detection |

LoRaTask waits on one FreeRTOS queue set: `DroneEvent_t` items from AudioTask, and a semaphore given by the SX1262's DIO1 interrupt when a packet arrives. Its timeout is the earlier of the next heartbeat (±10% jitter) and the next queued relay. The radio is half-duplex, so own transmissions and relays briefly pause RX; a node without `rx` keeps the radio in deep sleep between transmissions. Battery voltage is read fresh on every TX — GPIO37 gates the divider so its ~100 µA only flows during a ~2 ms measurement window.

Received events go to MqttTask via a FreeRTOS queue, so LoRa reception is never blocked by network I/O. The wired detector uses `DroneEvent_t` items directly from AudioTask's queue.

## Project Structure

```text
batear/
├── CMakeLists.txt
├── partitions_lora.csv         # LoRa node: 3 MiB app + bootctr (frame boot counter)
├── sdkconfig.defaults          # common ESP-IDF settings
├── sdkconfig.node_v3/_v4       # LoRa node, all caps auto (what CI releases)
├── sdkconfig.detector          # LoRa node, detector preset (local builds)
├── sdkconfig.gateway           # LoRa node, gateway preset (local builds)
├── sdkconfig.wired_detector    # wired detector role + MQTT config
├── main/
│   ├── CMakeLists.txt          # conditional compile by role
│   ├── Kconfig.projbuild       # role / caps / mesh / network / MQTT config
│   ├── main.cpp                # entry point: resolve caps, start tasks
│   ├── pin_config.h            # board-specific GPIO + hardware traits
│   ├── drone_detector.h        # shared DroneEvent_t + queue
│   ├── lora_crypto.h           # frame v2: header + AES-128-GCM (PSA API)
│   ├── lora_provision.c/.h     # network key / device ID / radio params + boot counter
│   ├── EspIdfHal.cpp/.h        # RadioLib HAL for ESP-IDF
│   ├── config_console.c/.h     # serial console (show/set/reboot)
│   ├── audio_processor.c/.h    # [node/wired] ESP-DSP FFT + PSD + harmonic analysis
│   ├── audio_task.c/.h         # [node/wired] I2S mic + detection state machine + mic probe
│   ├── battery.c/.h            # [node]  VBAT ADC + divider gating
│   ├── node_caps.c/.h          # [node]  runtime capabilities (mic / rx / bridge / relay)
│   ├── lora_radio.cpp/.h       # [node]  owns the SX1262: TX, RX, relay
│   ├── node_rx.cpp/.h          # [node]  decrypt + replay check + OLED + LED → MQTT
│   ├── mesh.c/.h               # [node]  dedup ring, slotted backoff, suppression, budget
│   ├── mqtt_task.cpp/.h        # [node]  Wi-Fi + MQTT + bridge-independent HA Discovery
│   ├── oled.c/.h               # [node]  SSD1306 128x64 driver
│   ├── eth_mqtt_task.cpp/.h    # [wired] W5500 Ethernet + MQTT + HA Discovery
│   ├── http_api.cpp/.h         # [wired] REST API + OTA
│   ├── tf_recorder.c/.h        # [wired] microSD WAV capture
│   └── idf_component.yml       # RadioLib + ESP-DSP + W5500 dependencies
└── test/host/                  # host unit tests (run in CI)
```
