<div align="center">
  <img src="icon.png" alt="Batear Logo" width="200"/>
  
  <h1>Batear</h1>
  <p><strong>A sub-$15, edge-only acoustic drone detector on ESP32-S3.</strong></p>

  [![Stars](https://img.shields.io/github/stars/TN666/batear?style=flat-square)](https://github.com/TN666/batear/stargazers)
  [![License](https://img.shields.io/github/license/TN666/batear?style=flat-square)](https://github.com/TN666/batear/blob/main/LICENSE)

  <br><br>
  <p><em>"Built for defense, hoping it becomes unnecessary. We believe in a world where no one needs to fear the sky."</em></p>
</div>

---

Drones are an increasing threat to homes, farms, and communities — and effective detection has traditionally required expensive radar or camera systems. **Batear changes that.**

For under $15 in hardware, Batear turns a tiny ESP32-S3 microcontroller and a MEMS microphone into an always-on acoustic drone detector. It runs entirely at the edge — **no cloud subscription, no internet connection, no ongoing cost.** Deploy one at a window, a fence line, or a rooftop and it will alert you the moment drone rotor harmonics are detected nearby.

---

## 🧠 The "Hack": Why Goertzel over FFT?

In typical audio processing, the default approach is a Fast Fourier Transform (FFT). However, running a high-resolution FFT is memory-hungry and computationally heavy for a microcontroller. 

Batear takes a different approach. It reads audio from an ICS-43434 I2S MEMS microphone and uses multi-frequency **Goertzel filters** to measure tonal energy specifically at drone rotor harmonics. 

* **Highly Efficient:** The Goertzel algorithm is O(N) per frequency bin. It only calculates the exact frequencies we care about.
* **Tiny Footprint:** It fits entirely within the ESP32-S3's 512 KB SRAM.
* **Low Power:** Consumes negligible power — making it practical for battery-powered or solar deployments.

It triggers an alarm when the tonal/broadband energy ratio exceeds a threshold.

---

## 🚀 Current Status & Call for Contributors (Help Needed!)

**Batear is currently functioning as a flashable baseline and has been successfully tested against prerecorded drone audio.** However, **we need real-world testing!** Acoustic drone detection in real environments depends heavily on distance, wind, background noise, and drone type. The current thresholds must be calibrated per environment.

If you have a micro drone, a soldering iron, and some free time, we would love your help to test this outside! 
* Pull Requests for threshold calibration, noise filtering, or achieving higher accuracy with ESP-NN / TensorFlow Lite Micro models are highly welcome.

---

## 🛠️ Hardware Wiring (ICS-43434)

| ICS-43434 | T-Display-S3 (ESP32-S3) |
| :--- | :--- |
| **VDD** | 3.3V |
| **GND** | GND |
| **SCK** | GPIO43 (BCLK) |
| **WS** | GPIO44 (LRCLK / WS) |
| **SD** | GPIO1 (DIN) |
| **L/R** | GND (left channel, matches `I2S_STD_SLOT_LEFT` in code) |

*Note: Power at 3.3V only. I2S data line runs SD → MCU DIN (mic output → MCU input). If there is no audio or the clock is unstable, try changing Philips to `I2S_STD_MSB_SLOT_DEFAULT_CONFIG` in `main.c`.*

---

## 💻 Prerequisites & Build

Requires **ESP-IDF v5.x** — follow the [official installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).

After installing, source the environment before building:

```bash
. $IDF_PATH/export.sh
```

**Build & Flash:**

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

---

## 🎛️ Calibration & Key Parameters

After boot, serial output prints `cal: active=N/6 [...ratios...] rms=... alarm=0` every second. 
Record the ratio values with and without a drone present (or play rotor audio through a speaker). Adjust `FREQ_RATIO_ON` / `FREQ_RATIO_OFF` and `k_target_hz[]` in `main/main.c` accordingly. 

*Rotor harmonics typically fall between a few hundred Hz and a few kHz depending on drone type.*

| Symbol | Default | Description |
| :--- | :--- | :--- |
| `SAMPLE_RATE_HZ` | `16000` | Sample rate (Hz) |
| `FRAME_SAMPLES` | `512` | Samples per analysis frame |
| `HOP_MS` | `100` | Frame hop interval (ms) |
| `k_target_hz[]` | `200, 400, 800, 1200, 2400, 4000` | Goertzel target frequencies (Hz) |
| `FREQ_RATIO_ON` | `0.008` | Alarm-on threshold (tonal/broadband ratio) |
| `FREQ_RATIO_OFF` | `0.004` | Alarm-off threshold |
| `EMA_ALPHA` | `0.25` | EMA smoothing factor (higher = faster response) |
| `FREQS_NEEDED` | `1` | Minimum active frequencies required to trigger |
| `SUSTAIN_FRAMES_ON` | `2` | Consecutive frames to set alarm |
| `SUSTAIN_FRAMES_OFF` | `8` | Consecutive frames to clear alarm |
| `RMS_MIN` | `0.0003` | Minimum RMS — frames below this are skipped |

---

## 📁 Project Structure

```text
batear/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
└── main/
    ├── CMakeLists.txt
    └── main.c
```
