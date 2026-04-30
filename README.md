<div align="center">
  <img src="icon.png" alt="Batear Logo" width="200"/>
  
  <h1>Batear</h1>
  <p><strong>An ultra-low-cost, edge-only acoustic drone detector on ESP32-S3 with encrypted LoRa or wired Ethernet/PoE alerting.</strong></p>

<p align="center">
  <a href="https://opencollective.com/batear"><img src="https://img.shields.io/opencollective/all/batear?label=Sponsors&color=blue" alt="Sponsors"></a>
  <a href="https://opencollective.com/batear"><img src="https://img.shields.io/badge/501(c)(3)-Tax%20Deductible-brightgreen" alt="Tax Deductible"></a>
  <a href="https://hackaday.com/2026/03/23/acoustic-drone-detection-on-the-cheap-with-esp32/"><img src="https://img.shields.io/badge/Featured%20on-Hackaday-black?logo=hackaday" alt="Featured on Hackaday"></a>
  <a href="https://github.com/batear-io/batear/stargazers"><img src="https://img.shields.io/github/stars/batear-io/batear?style=flat-square" alt="Stars"></a>
</p>

<p align="center">
  <a href="https://github.com/batear-io/batear/actions/workflows/esp-idf-build.yml"><img src="https://github.com/batear-io/batear/actions/workflows/esp-idf-build.yml/badge.svg" alt="Firmware Build"></a>
  <a href="https://github.com/batear-io/batear/actions/workflows/cppcheck.yml"><img src="https://github.com/batear-io/batear/actions/workflows/cppcheck.yml/badge.svg" alt="Static Analysis"></a>
</p>

  <br>
  <p><em>"Built for defense, hoping it becomes unnecessary. We believe in a world where no one needs to fear the sky."</em></p>
</div>

---

## 🛡️ Support the Mission (501(c)(3) Tax Deductible)

Batear is officially fiscally hosted by the **Open Source Collective (OSC)**. Your contribution directly supports the deployment of civil defense technology where it is needed most.

* 🇺🇦 **Field Testing in Ukraine**: We are partnering with local experts to verify Batear in active EW (Electronic Warfare) environments.
* 🔬 **Hardware R&D**: Funding allows us to procure high-precision sensors (ICS-43434) and PoE-capable hardware for professional-grade reliability.
* 🏢 **Corporate Matching**: Since we are a **501(c)(3)** non-profit project, check if your employer matches your donation!
* 💎 **100% Transparent**: View every single receipt and transaction on our [Public Ledger](https://opencollective.com/batear#category-BUDGET).

[**💖 Become a Sponsor on Open Collective**](https://opencollective.com/batear)

---

<div align="center">
  <a href="https://youtu.be/_OXP_MpExm8?si=sbgMLhAHPr7uCMk2">
    <img src="https://img.youtube.com/vi/_OXP_MpExm8/0.jpg" alt="Batear Demo Video" width="600">
  </a>
  <br><br>
  <em>▶️ Click to watch the bench test demo</em>
</div>
<br>

Drones are an increasing threat to homes, farms, and communities — and effective detection has traditionally required expensive radar or camera systems. **Batear changes that.**

For ultra-low-cost hardware, Batear turns a tiny ESP32-S3 microcontroller and a MEMS microphone into an always-on acoustic drone detector. It runs entirely at the edge — **no cloud subscription, no internet connection, no ongoing cost.** Deploy one at a window, a fence line, or a rooftop and it will alert you the moment drone rotor harmonics are detected nearby.

The same codebase builds as a **Detector** (mic + LoRa TX), a **Gateway** (LoRa RX + OLED + LED + MQTT), or a **Wired Detector** (mic + Ethernet/PoE + MQTT), selectable at build time. The gateway forwards alerts to **Home Assistant** via MQTT with automatic device discovery; the wired detector publishes directly over Ethernet — no LoRa or gateway required, ideal for permanent installations.

---

## 🌐 Web Flasher — Zero Install

Flash firmware directly from your browser — no toolchain needed:

**[Open Web Flasher](https://docs.batear.io/flasher/)**

> Requires Chrome or Edge on desktop. Connect a Heltec V3/V4 (Detector or Gateway) or a LILYGO T-ETH-Lite S3 (Wired Detector) via USB-C and click Install.

---

## 🏠 Smart Home Integration (Home Assistant)

Stop checking serial monitors. Batear brings drone detection directly to your dashboard.

<img width="1851" height="1047" alt="image" src="https://github.com/user-attachments/assets/d57e30c3-9cc5-46db-8cb8-4a87c0076ae3" />

### Features:
- **Plug & Play**: Auto-Discovery via MQTT.
- **Rich Diagnostics**: Monitor signal strength (RSSI/SNR) in real-time.
- **Automations Ready**: Trigger your smart lights, alarms, or notifications when a drone is detected.
- **Historical Logs**: Analyze drone activity patterns in your area.

> "Batear bridges the gap between complex signal processing and simple home automation."

---

## 📖 Documentation

Full documentation is available at **[batear.io](https://docs.batear.io)**.

| | |
|:---|:---|
| [**Getting Started**](https://docs.batear.io/getting-started/) | Prerequisites and supported boards |
| [**Hardware**](https://docs.batear.io/hardware/) | Parts list, wiring diagrams, pin map |
| [**Build & Flash**](https://docs.batear.io/build-flash/) | Compile and flash the firmware |
| [**Configuration**](https://docs.batear.io/configuration/) | Encryption keys, frequencies, MQTT, device IDs |
| [**How It Works**](https://docs.batear.io/how-it-works/) | FFT harmonic detection algorithm |
| [**Calibration**](https://docs.batear.io/calibration/) | Tuning detection thresholds |
| [**Adding a Board**](https://docs.batear.io/adding-boards/) | Porting to new hardware |

---

## 🏗️ System Architecture

Batear supports two deployment models — pick whichever fits the site:

### Wireless (LoRa Detector → Gateway → MQTT)

```
┌──────────────────────┐        LoRa 915 MHz          ┌──────────────────────┐
│    DETECTOR (×N)     │ ───────────────────────────► │     GATEWAY (×1)     │
│                      │  AES-128-GCM encrypted       │                      │
│  ICS-43434 mic       │  36-byte packets             │  SSD1306 OLED display│
│  FFT harmonic detect │                              │  LED alarm indicator │
│  SX1262 LoRa TX      │                              │  SX1262 LoRa RX      │
└──────────────────────┘                              │  WiFi + MQTT TX      │
   Heltec WiFi LoRa 32 V3/V4                          └──────────┬───────────┘
                                                        Heltec WiFi LoRa 32 V3/V4
                                                                  │ MQTT
                                                                  ▼
                                                      ┌──────────────────────┐
                                                      │   HOME ASSISTANT     │
                                                      │   (auto-discovery)   │
                                                      └──────────────────────┘
```

### Wired (Ethernet/PoE Detector → MQTT, no gateway)

```
┌──────────────────────┐
│  WIRED DETECTOR (×N) │       Ethernet (PoE)
│                      │ ───────────────────────────► ┌──────────────────────┐
│  ICS-43434 mic       │       MQTT / JSON            │   HOME ASSISTANT     │
│  FFT harmonic detect │       + REST API / OTA       │   (auto-discovery)   │
│  W5500 Ethernet      │                              └──────────────────────┘
└──────────────────────┘
   LILYGO T-ETH-Lite S3
```

---

## ⚡ Quick Start (Build from Source)

```bash
# Clone
git clone https://github.com/batear-io/batear.git && cd batear

# Build detector  (swap sdkconfig.detector → sdkconfig.gateway or
# sdkconfig.wired_detector for the other roles)
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.detector" \
  set-target esp32s3
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig build

# Flash (replace PORT)
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig -p PORT flash monitor
```

See the [full build guide](https://docs.batear.io/build-flash/) for gateway and wired-detector setup and detailed instructions.

---

## 👤 Maintainers

<a href="https://github.com/TN666">
  <img src="https://images.weserv.nl/?url=https://github.com/TN666.png?v=4&w=200&h=200&fit=cover&mask=circle&maxage=7d" width="100">
</a>

<p><strong>Batear is a community-driven project. We welcome contributions, whether through code, field data, or financial sponsorship.</strong></p>

