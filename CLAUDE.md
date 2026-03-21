# batear

ESP32-S3 + ICS-43434 acoustic drone detection project using ESP-IDF 5.x.
Target board: **LilyGo T-Display-S3**.

## Build & Flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Project Structure

```
project_batear/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   └── main.c        # All application logic lives here
```

## Key Parameters (main/main.c)

| Symbol | Value | Notes |
|---|---|---|
| `I2S_MIC_BCLK_GPIO` | 43 | |
| `I2S_MIC_WS_GPIO` | 44 | |
| `I2S_MIC_DIN_GPIO` | 1 | |
| `SAMPLE_RATE_HZ` | 16000 | |
| `FRAME_SAMPLES` | 512 | |
| `HOP_MS` | 100 | |
| `k_target_hz[]` | 200, 400, 800, 1200, 2400, 4000 Hz | Calibrate per drone type |
| `FREQ_RATIO_ON` | 0.008 | Calibrate per environment |
| `FREQ_RATIO_OFF` | 0.004 | Calibrate per environment |

## Calibration

Serial output prints `cal: active=N/6 [...ratios...] rms=... alarm=0` every second. Record ratio range with and without drone present, then adjust `FREQ_RATIO_ON`/`FREQ_RATIO_OFF` and `k_target_hz[]` accordingly.
