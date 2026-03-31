<div align="center">
  <img src="icon.png" alt="Batear Logo" width="200"/>
  
  <h1>Batear</h1>
  <p><strong>A ultra-low-cost, edge-only acoustic drone detector on ESP32-S3 with encrypted LoRa alerting.</strong></p>

<p align="center">
  <a href="https://hackaday.com/2026/03/23/acoustic-drone-detection-on-the-cheap-with-esp32/"><img src="https://img.shields.io/badge/Featured%20on-Hackaday-black?logo=hackaday" alt="Featured on Hackaday" style="display:inline-block;"></a><a href="https://github.com/TN666/batear/stargazers"><img src="https://img.shields.io/github/stars/TN666/batear?style=flat-square" alt="Stars" style="display:inline-block;"></a><a href="https://github.com/TN666/batear/blob/main/LICENSE"><img src="https://img.shields.io/github/license/TN666/batear?style=flat-square" alt="License" style="display:inline-block;"></a>
</p>

  <br><br>
  <p><em>"Built for defense, hoping it becomes unnecessary. We believe in a world where no one needs to fear the sky."</em></p>
</div>
<br><br>

<div align="center">
  <a href="https://youtu.be/_OXP_MpExm8?si=sbgMLhAHPr7uCMk2">
    <img src="https://img.youtube.com/vi/_OXP_MpExm8/0.jpg" alt="Batear Demo Video" width="600">
  </a>
  <br><br>
  <em>▶️ Click to watch the bench test demo</em>
</div>
<br>

---

Drones are an increasing threat to homes, farms, and communities — and effective detection has traditionally required expensive radar or camera systems. **Batear changes that.**

For ultra-low-cost hardware, Batear turns a tiny ESP32-S3 microcontroller and a MEMS microphone into an always-on acoustic drone detector. It runs entirely at the edge — **no cloud subscription, no internet connection, no ongoing cost.** Deploy one at a window, a fence line, or a rooftop and it will alert you the moment drone rotor harmonics are detected nearby.

The same codebase builds as a **Detector** (mic + LoRa TX) or a **Gateway** (LoRa RX + OLED + LED), selectable at build time.

---

## 🧠 How Detection Works: FFT Harmonic Signature

Batear reads audio from an ICS-43434 I2S MEMS microphone at **16 kHz** and runs a **1024-point real FFT** (via ESP-DSP SIMD-accelerated routines) to produce a power spectral density (PSD) with **15.625 Hz/bin** resolution.

Multi-rotor drones produce a characteristic acoustic signature: a strong **fundamental frequency** (f₀, tied to motor/propeller RPM) plus clearly visible **harmonics at 2×f₀ and 3×f₀**. Batear exploits this by scanning the PSD for peaks that exhibit this harmonic ladder pattern.

* **Harmonic Analysis:** For each candidate f₀ (180–2400 Hz), the detector checks whether energy peaks also exist near 2×f₀ and 3×f₀ relative to the noise floor.
* **Confidence Scoring:** A 0–1 heuristic combining SNR, h2/h3 ratios, and exponential moving average smoothing reduces false alarms from transient sounds.
* **Hysteresis:** Alarm requires **2 consecutive** positive frames; clearing requires **8 consecutive** negative frames — eliminating flicker.
* **ESP-DSP Accelerated:** The ESP32-S3's SIMD instructions keep the full FFT + harmonic scan well under 10 ms per frame.

When a drone signature is confirmed, an **AES-128-GCM encrypted** alert is transmitted over LoRa.

---

## 🏗️ System Architecture

```
┌──────────────────────┐        LoRa 915 MHz         ┌──────────────────────┐
│    DETECTOR (×N)     │ ───────────────────────────► │     GATEWAY (×1)     │
│                      │  AES-128-GCM encrypted       │                      │
│  ICS-43434 mic       │  28-byte packets             │  SSD1306 OLED display│
│  FFT harmonic detect │                              │  LED alarm indicator │
│  SX1262 LoRa TX      │                              │  SX1262 LoRa RX      │
└──────────────────────┘                              └──────────────────────┘
   Heltec WiFi LoRa 32 V3                               Heltec WiFi LoRa 32 V3
```

Multiple detectors can report to one gateway. Each detector has a unique device ID (0–255).

---

## 🛠️ Hardware

### Detector

| Component | Notes |
|:---|:---|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 + SX1262 on-board |
| ICS-43434 I2S MEMS microphone | 3.3 V, L/R → GND (left channel) |

**Wiring (ICS-43434 → Heltec V3):**

| ICS-43434 | GPIO | Function |
|:---|:---|:---|
| VDD | 3.3V | Power |
| GND | GND | Ground |
| SCK | 4 | I2S bit clock (BCLK) |
| WS | 5 | I2S word select (LRCLK) |
| SD | 6 | I2S data input (DIN) |
| L/R | GND | Left channel select |

### Gateway

| Component | Notes |
|:---|:---|
| Heltec WiFi LoRa 32 V3 | Uses on-board SX1262, SSD1306 OLED, and LED |

No external wiring needed — everything is on-board.

---

## 💻 Prerequisites

- **ESP-IDF v6.x** — follow the [official installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Source the environment before building: `. $IDF_PATH/export.sh`

---

## 🔨 Build & Flash

The same codebase builds either role. Each role has its own build directory so both can coexist.

`set-target` depends on your board's chip:

| Board | Chip | `set-target` | Flash |
|:---|:---|:---|:---|
| Heltec WiFi LoRa 32 V3 | ESP32-S3 | `esp32s3` | 8 MB |

### 1. Clone & find your serial port

```bash
git clone https://github.com/TN666/batear.git
cd batear
ls /dev/cu.usb*          # note your port, e.g. /dev/cu.usbserial-3
```

### 2. Build & flash the Detector

```bash
# First time only — set chip target (esp32s3 for Heltec V3, see table above)
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.detector" \
  set-target esp32s3

# Build
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  build

# Flash & monitor (replace PORT)
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  -p PORT flash monitor
```

### 3. Build & flash the Gateway

```bash
# First time only — set chip target (esp32s3 for Heltec V3, see table above)
idf.py -B build_gateway \
  -DSDKCONFIG=build_gateway/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.gateway" \
  set-target esp32s3

# Build
idf.py -B build_gateway \
  -DSDKCONFIG=build_gateway/sdkconfig \
  build

# Flash & monitor (replace PORT)
idf.py -B build_gateway \
  -DSDKCONFIG=build_gateway/sdkconfig \
  -p PORT flash monitor
```

### After first setup

Code changes only need rebuild + flash (no `set-target`):

```bash
# Detector
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig build
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig -p PORT flash monitor

# Gateway
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig build
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig -p PORT flash monitor
```

> Replace `PORT` with your actual serial port (e.g. `/dev/cu.usbserial-3` on macOS, `/dev/ttyUSB0` on Linux).

---

## ⚙️ Configuration

All network and role settings live in two small files — no `menuconfig` needed:

**`sdkconfig.detector`**

```ini
# Board
CONFIG_BATEAR_BOARD_HELTEC_V3=y
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y

# Role
CONFIG_BATEAR_ROLE_DETECTOR=y
CONFIG_BATEAR_DEVICE_ID=1

# Network
CONFIG_BATEAR_NET_KEY="DEADBEEFCAFEBABE13374200F00DAA55"
CONFIG_BATEAR_LORA_FREQ=915000
CONFIG_BATEAR_LORA_SYNC_WORD=0x12
```

**`sdkconfig.gateway`**

```ini
# Board
CONFIG_BATEAR_BOARD_HELTEC_V3=y
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y

# Role
CONFIG_BATEAR_ROLE_GATEWAY=y

# Network
CONFIG_BATEAR_NET_KEY="DEADBEEFCAFEBABE13374200F00DAA55"
CONFIG_BATEAR_LORA_FREQ=915000
CONFIG_BATEAR_LORA_SYNC_WORD=0x12
```

| Parameter | Description |
|:---|:---|
| `CONFIG_BATEAR_BOARD_*` | Board selection — determines GPIO mapping and `set-target` chip. |
| `CONFIG_ESPTOOLPY_FLASHSIZE_*` | Flash size — must match your board's flash chip. |
| `CONFIG_BATEAR_NET_KEY` | 128-bit AES-GCM key (32 hex chars). **Must match** between all devices. |
| `CONFIG_BATEAR_LORA_FREQ` | Centre frequency in kHz: `915000` (US/TW), `868000` (EU), `923000` (AS). |
| `CONFIG_BATEAR_LORA_SYNC_WORD` | Network isolation byte. Different values = invisible to each other. |
| `CONFIG_BATEAR_DEVICE_ID` | Detector only. Unique ID (0–255) shown on gateway display. |

**Generate a new key:**

```bash
python3 -c "import os; print(os.urandom(16).hex().upper())"
```

---

## 🔐 LoRa Packet Protocol

AES-128-GCM encrypted, 28 bytes on the wire:

```
[4B nonce] [8B ciphertext] [16B GCM auth tag]
```

Plaintext (8 bytes):

```
[2B seq] [1B device_id] [1B event_type] [1B f0_bin] [1B rms_db] [2B reserved]
```

- **Sequence counter** prevents replay attacks
- **GCM auth tag** ensures data integrity and authenticity
- **Sync word** provides channel isolation at the RF level

---

## 🎛️ Calibration

When an alarm is active, serial output prints:

```
cal: f0=XXX.X Hz h2=X.XX h3=X.XX snr=XX.X nf=X.XXeXX conf_ema=X.XX rms=X.XXXXX
```

Record these values with and without a drone present (or play rotor audio through a speaker). Key tuning parameters in `main/audio_task.c` and `main/audio_processor.c`:

| Symbol | Default | File | Description |
|:---|:---|:---|:---|
| `SAMPLE_RATE_HZ` | `16000` | `audio_processor.h` | Sample rate (Hz) |
| `FRAME_SAMPLES` | `1024` | `audio_processor.h` | FFT size (samples per frame) |
| `HOP_MS` | `100` | `audio_task.c` | Frame hop interval (ms) |
| `HARM_F0_MIN_HZ` | `180` | `audio_task.c` | Fundamental search lower bound (Hz) |
| `HARM_F0_MAX_HZ` | `2400` | `audio_task.c` | Fundamental search upper bound (Hz); 3×f₀ must stay < Nyquist |
| `CONF_ON` | `0.30` | `audio_task.c` | Smoothed confidence threshold to trigger alarm |
| `CONF_OFF` | `0.18` | `audio_task.c` | Smoothed confidence threshold to clear alarm |
| `SUSTAIN_FRAMES_ON` | `2` | `audio_task.c` | Consecutive frames above CONF_ON to trigger |
| `SUSTAIN_FRAMES_OFF` | `8` | `audio_task.c` | Consecutive frames below CONF_OFF to clear |
| `RMS_MIN` | `0.0004` | `audio_task.c` | Minimum windowed RMS — frames below this are skipped |
| `EMA_ALPHA` | `0.25` | `audio_task.c` | Exponential moving average factor for confidence |
| `AUDIO_PROC_HARM_PEAK_MIN_SNR` | `4.0` | `audio_processor.c` | Minimum f₀ peak SNR vs noise floor |
| `AUDIO_PROC_HARM_MIN_H2` | `0.07` | `audio_processor.c` | Minimum P(2f₀)/P(f₀) ratio |
| `AUDIO_PROC_HARM_MIN_H3` | `0.035` | `audio_processor.c` | Minimum P(3f₀)/P(f₀) ratio |

---

## 📁 Project Structure

```text
batear/
├── CMakeLists.txt
├── sdkconfig.defaults          # common ESP-IDF settings
├── sdkconfig.detector          # detector role + device ID + network
├── sdkconfig.gateway           # gateway role + network
├── main/
│   ├── CMakeLists.txt          # conditional compile by role
│   ├── Kconfig.projbuild       # role / device ID / network / debug config
│   ├── main.cpp                # entry point (role switch)
│   ├── pin_config.h            # board-specific GPIO + hardware traits
│   ├── drone_detector.h        # shared DroneEvent_t + queue
│   ├── lora_crypto.h           # AES-128-GCM packet protocol (PSA API)
│   ├── EspIdfHal.cpp/.h        # RadioLib HAL for ESP-IDF
│   ├── audio_processor.c/.h    # [detector] ESP-DSP FFT + PSD + harmonic analysis
│   ├── audio_task.c/.h         # [detector] I2S mic + detection state machine
│   ├── lora_task.cpp/.h        # [detector] LoRa TX
│   ├── gateway_task.cpp/.h     # [gateway]  LoRa RX + OLED + LED
│   ├── oled.c/.h               # [gateway]  SSD1306 128x64 driver
│   └── idf_component.yml       # RadioLib + ESP-DSP dependencies
```

---

## 🔧 Adding a New Board

Batear uses a board abstraction layer — all board-specific GPIO and hardware traits are defined in one place (`main/pin_config.h`), selected by a Kconfig option. Here's how to port to a new board.

### Step 1: Kconfig — register the board

Add an entry under `BATEAR_BOARD` in `main/Kconfig.projbuild`:

```kconfig
config BATEAR_BOARD_MY_NEW_BOARD
    bool "My New Board (ESP32 + SX1276)"
```

### Step 2: pin_config.h — define all hardware constants

Add an `#elif` block in `main/pin_config.h`. Every board must define **all** of these macros:

```c
#elif defined(CONFIG_BATEAR_BOARD_MY_NEW_BOARD)

#define BOARD_IDF_TARGET    "esp32"     // chip for set-target
#define BOARD_FLASH_SIZE    "4MB"       // for documentation

// I2S microphone
#define PIN_I2S_BCLK     26
#define PIN_I2S_WS       25
#define PIN_I2S_DIN      34

// LoRa SPI
#define PIN_LORA_SCK      5
#define PIN_LORA_MISO    19
#define PIN_LORA_MOSI    27
#define PIN_LORA_CS      18
#define PIN_LORA_DIO1    26
#define PIN_LORA_RST     23
#define PIN_LORA_BUSY    -1            // SX1276 has no BUSY pin

// Gateway peripherals
#define PIN_OLED_SDA      4
#define PIN_OLED_SCL     15
#define PIN_OLED_RST     16
#define PIN_LED           2
#define PIN_VEXT         -1
#define BOARD_HAS_VEXT    0            // no Vext power control
#define BOARD_HAS_OLED    1

// LoRa RF traits
#define BOARD_LORA_TCXO_V       0.0f   // no TCXO
#define BOARD_LORA_DIO2_AS_RF   false  // no DIO2 RF switch
```

### Step 3: sdkconfig files — board + flash size

Copy `sdkconfig.detector` and `sdkconfig.gateway`, change the board and flash lines:

```ini
CONFIG_BATEAR_BOARD_MY_NEW_BOARD=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
```

### Step 4: Build — use the correct chip target

`set-target` must match `BOARD_IDF_TARGET` (ESP-IDF only accepts chip names, not board names):

```bash
idf.py -B build_detector \
  -DSDKCONFIG=build_detector/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.detector" \
  set-target esp32          # ← matches BOARD_IDF_TARGET
```

### Step 5: If the LoRa chip is different

If the board uses SX1276/SX1278 instead of SX1262, you'll need to update `lora_task.cpp` and `gateway_task.cpp` to instantiate the correct RadioLib class. Use `#ifdef` on your board macro for conditional compilation.

### Checklist

- [ ] `Kconfig.projbuild` — board entry added
- [ ] `pin_config.h` — `#elif` block with all `PIN_*` and `BOARD_*` macros
- [ ] `sdkconfig.detector` / `sdkconfig.gateway` — board + flash size set
- [ ] `set-target` matches the chip (`esp32`, `esp32s3`, `esp32c3`, ...)
- [ ] Board row added to the build table in this README
- [ ] Build + flash tested on hardware

---

## 🚀 Current Status & Call for Contributors

**Batear is functioning as a flashable baseline and has been successfully tested against prerecorded drone audio.** We need real-world testing! Acoustic drone detection in real environments depends heavily on distance, wind, background noise, and drone type.

If you have a micro drone, a soldering iron, and some free time, we would love your help:

- Real-world threshold calibration per drone type
- Noise filtering improvements
- ML-based detection (ESP-NN / TFLite Micro)
- Multi-gateway mesh networking
- Battery/solar power optimizations
