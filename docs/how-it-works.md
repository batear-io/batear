# Detection Algorithm

## FFT Harmonic Signature

Batear reads audio from an ICS-43434 I2S MEMS microphone at **16 kHz** and runs a **1024-point real FFT** (via ESP-DSP SIMD-accelerated routines) to produce a power spectral density (PSD) with **15.625 Hz/bin** resolution.

Multi-rotor drones produce a characteristic acoustic signature: a strong **fundamental frequency** (f₀, tied to motor/propeller RPM) plus clearly visible **harmonics at 2×f₀ and 3×f₀**. Batear exploits this by scanning the PSD for peaks that exhibit this harmonic ladder pattern.

## Detection Pipeline

### Harmonic Analysis

For each candidate f₀ (180–2400 Hz), the detector checks whether energy peaks also exist near 2×f₀ and 3×f₀ relative to the noise floor.

### Confidence Scoring

A 0–1 heuristic combining SNR, h2/h3 ratios, and exponential moving average smoothing reduces false alarms from transient sounds.

### Hysteresis

Alarm requires **2 consecutive** positive frames; clearing requires **8 consecutive** negative frames — eliminating flicker.

### ESP-DSP Accelerated

The ESP32-S3's SIMD instructions keep the full FFT + harmonic scan well under 10 ms per frame.

## Alert Transmission

When a drone signature is confirmed, the detector sends an alert. The path depends on the deployment model:

- **LoRa Detector** — An **AES-128-GCM encrypted** alert is transmitted over LoRa to the gateway. See the [LoRa Protocol](protocol.md) page for packet format details.
- **Wired Detector** — The `DroneEvent_t` is published directly as JSON to the MQTT broker over Ethernet. No encryption overhead is needed since the network is wired.

## Gateway → Home Assistant

The gateway decrypts the LoRa packet and pushes a `MqttEvent_t` into a FreeRTOS queue. On a separate core, MqttTask picks up the event and publishes a JSON payload over MQTT to the broker.

## Wired Detector → Home Assistant

The wired detector's EthMqttTask receives `DroneEvent_t` items directly from AudioTask via a FreeRTOS queue and publishes them as JSON over MQTT. No gateway is needed — the detector connects directly to the MQTT broker over Ethernet (optionally via PoE for single-cable deployment).

Home Assistant discovers both device types automatically via MQTT Discovery — no manual YAML needed. See [Configuration → MQTT / Home Assistant](configuration.md#mqtt-home-assistant-integration) for topics, payloads, and setup.
