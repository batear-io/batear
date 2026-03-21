# batear

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Drones are an increasing threat to homes, farms, and communities — and effective detection has traditionally required expensive radar or camera systems. **batear** changes that.

For under **$15 in hardware**, batear turns a tiny ESP32-S3 microcontroller and a MEMS microphone into an always-on acoustic drone detector. It runs entirely at the edge — no cloud subscription, no internet connection, no ongoing cost. Deploy one at a window, a fence line, or a rooftop and it will alert you the moment drone rotor harmonics are detected nearby.

It reads audio from an ICS-43434 I2S MEMS microphone, uses multi-frequency **Goertzel** filters to measure tonal energy at drone rotor harmonics, and triggers an alarm when the tonal/broadband energy ratio exceeds a threshold. The Goertzel algorithm is O(N) per frequency bin, fits entirely within the ESP32-S3's 512 KB SRAM, and consumes negligible power — making it practical for battery-powered or solar deployments.

> *Built for defense, hoping it becomes unnecessary. We believe in a world where no one needs to fear the sky.*

> **Note:** Acoustic drone detection in real environments depends on distance, wind, background noise, and drone type. This project is a **flashable baseline** — thresholds must be calibrated per environment. Higher accuracy can be achieved with ESP-NN / TensorFlow Lite Micro models.

## License

MIT — see [LICENSE](LICENSE)

## Hardware Wiring (ICS-43434)

| ICS-43434 | T-Display-S3 |
|-----------|--------------|
| VDD | 3.3V |
| GND | GND |
| SCK | `GPIO43` (BCLK) |
| WS | `GPIO44` (LRCLK / WS) |
| SD | `GPIO1` (DIN) |
| L/R | **GND** (left channel, matches `I2S_STD_SLOT_LEFT` in code) |

- Power at **3.3V only**. I2S data line runs SD → MCU DIN (mic output → MCU input).
- If there is no audio or the clock is unstable, try changing Philips to `I2S_STD_MSB_SLOT_DEFAULT_CONFIG` in `main.c`.

## Environment

- **ESP-IDF**: `~/esp/esp-idf` (v5.3.2)
- Load environment in a new terminal: `get_idf` (alias in `~/.bashrc`), or run `. ~/esp/esp-idf/export.sh`
- **CMake / Ninja**: installed via `pip install --user` at `~/.local/bin` (added to `PATH` in `~/.bashrc`)

## Build & Flash

```bash
get_idf
idf.py set-target esp32s3
idf.py build flash monitor
```

## Calibration

1. After boot, serial output prints `cal: active=N/6 [...ratios...] rms=... alarm=0` every second.
2. Record the ratio values with and without a drone present (or play rotor audio through a speaker).
3. Adjust `FREQ_RATIO_ON` / `FREQ_RATIO_OFF` and `k_target_hz[]` in `main/main.c` accordingly. Rotor harmonics typically fall between a few hundred Hz and a few kHz depending on drone type.

## Key Parameters

| Symbol | Default | Description |
|---|---|---|
| `SAMPLE_RATE_HZ` | 16000 | Sample rate (Hz) |
| `FRAME_SAMPLES` | 512 | Samples per analysis frame |
| `HOP_MS` | 100 | Frame hop interval (ms) |
| `k_target_hz[]` | 200, 400, 800, 1200, 2400, 4000 | Goertzel target frequencies (Hz) |
| `FREQ_RATIO_ON` | 0.008 | Alarm-on threshold (tonal/broadband ratio) |
| `FREQ_RATIO_OFF` | 0.004 | Alarm-off threshold |
| `EMA_ALPHA` | 0.25 | EMA smoothing factor (higher = faster response) |
| `FREQS_NEEDED` | 1 | Minimum active frequencies required to trigger |
| `SUSTAIN_FRAMES_ON` | 2 | Consecutive frames to set alarm |
| `SUSTAIN_FRAMES_OFF` | 8 | Consecutive frames to clear alarm |
| `RMS_MIN` | 0.0003 | Minimum RMS — frames below this are skipped |

## Project Structure

```
project_batear/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
└── main/
    ├── CMakeLists.txt
    └── main.c
```
