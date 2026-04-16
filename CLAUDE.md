# batear

ESP32-S3 (Heltec WiFi LoRa 32 V3 / V4) acoustic drone detection with encrypted LoRa alerting. ESP-IDF 6.x.

Same codebase builds as **Detector** or **Gateway** via sdkconfig files.

## Board → Target

| Board | `set-target` | Flash |
|---|---|---|
| Heltec WiFi LoRa 32 V3 | `esp32s3` | 8 MB |
| Heltec WiFi LoRa 32 V4 | `esp32s3` | 16 MB |

## Build & Flash

`set-target` depends on board chip — see table above and `BOARD_IDF_TARGET` in `pin_config.h`.

```bash
# First time — set target (once per build directory)
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.detector" set-target esp32s3
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.gateway" set-target esp32s3

# Build
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig build
idf.py -B build_gateway  -DSDKCONFIG=build_gateway/sdkconfig  build

# Flash
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig -p PORT flash monitor
idf.py -B build_gateway  -DSDKCONFIG=build_gateway/sdkconfig  -p PORT flash monitor
```

## Configuration Files

| File | Purpose |
|---|---|
| `sdkconfig.defaults` | Common ESP-IDF settings (CPU freq, logging, stack) |
| `sdkconfig.detector` | Board, flash size, role, device ID, network config |
| `sdkconfig.gateway` | Board, flash size, role, network config |

Key parameters in `sdkconfig.detector` / `sdkconfig.gateway`:

| Config | Description |
|---|---|
| `CONFIG_BATEAR_BOARD_*` | Board selection (determines GPIO mapping) |
| `CONFIG_ESPTOOLPY_FLASHSIZE_*` | Flash size (must match board hardware) |
| `CONFIG_BATEAR_NET_KEY` | 32-char hex AES-128 key. Must match on all devices. (overridden by NVS) |
| `CONFIG_BATEAR_LORA_FREQ` | Frequency in kHz (915000 / 868000 / 923000). (overridden by NVS) |
| `CONFIG_BATEAR_LORA_SYNC_WORD` | Network isolation (0x12 = private). (overridden by NVS) |
| `CONFIG_BATEAR_DEVICE_ID` | Detector only, 0–255. (overridden by NVS) |
| `CONFIG_BATEAR_WIFI_SSID` | Gateway Wi-Fi SSID (overridden by NVS) |
| `CONFIG_BATEAR_WIFI_PASS` | Gateway Wi-Fi password (overridden by NVS) |
| `CONFIG_BATEAR_MQTT_BROKER_URL` | MQTT broker URI, e.g. `mqtt://ha.local:1883` |
| `CONFIG_BATEAR_MQTT_USER` | MQTT username (overridden by NVS) |
| `CONFIG_BATEAR_MQTT_PASS` | MQTT password (overridden by NVS) |
| `CONFIG_BATEAR_GW_DEVICE_ID` | Gateway ID for MQTT topics (overridden by NVS) |

## Project Structure

```
batear/
├── CMakeLists.txt
├── sdkconfig.defaults
├── sdkconfig.detector
├── sdkconfig.gateway
├── main/
│   ├── CMakeLists.txt          # conditional compile by role
│   ├── Kconfig.projbuild       # role / device ID / network / debug config
│   ├── main.cpp                # entry point (role switch)
│   ├── pin_config.h            # board-specific GPIO + hardware traits
│   ├── drone_detector.h        # shared DroneEvent_t + queue
│   ├── lora_crypto.h           # AES-128-GCM packet protocol (PSA API)
│   ├── EspIdfHal.cpp/.h        # RadioLib HAL for ESP-IDF
│   ├── config_console.c/.h     # serial console (show/set/reboot)
│   ├── audio_processor.c/.h    # [detector] ESP-DSP FFT + PSD + harmonic analysis
│   ├── audio_task.c/.h         # [detector] I2S mic + detection state machine
│   ├── lora_task.cpp/.h        # [detector] LoRa TX
│   ├── gateway_task.cpp/.h     # [gateway]  LoRa RX + OLED + LED
│   ├── mqtt_task.cpp/.h        # [gateway]  WiFi + MQTT + HA Discovery
│   ├── oled.c/.h               # [gateway]  SSD1306 128x64 driver
│   └── idf_component.yml       # RadioLib + ESP-DSP dependencies
```

## Pin Map (pin_config.h, Heltec V3 / V4)

| Function | GPIO | Notes |
|---|---|---|
| I2S BCLK | 4 | ICS-43434 SCK (detector only) |
| I2S WS | 5 | ICS-43434 LRCLK |
| I2S DIN | 6 | ICS-43434 SD |
| LoRa SCK | 9 | SX1262, on-board |
| LoRa MOSI | 10 | on-board |
| LoRa MISO | 11 | on-board |
| LoRa CS | 8 | SX1262 NSS |
| LoRa RST | 12 | |
| LoRa BUSY | 13 | |
| LoRa DIO1 | 14 | |
| OLED SDA | 17 | gateway only |
| OLED SCL | 18 | gateway only |
| OLED RST | 21 | gateway only |
| LED | 35 | gateway only |
| Vext | 36 | 3.3V power control (active low) |

## Calibration

When alarm is active, serial prints:
`cal: f0=XXX.X Hz h2=X.XX h3=X.XX snr=XX.X nf=X.XXeXX conf_ema=X.XX rms=X.XXXXX`

Tune detection in `audio_task.c` (`HARM_F0_MIN/MAX_HZ`, `CONF_ON/OFF`, `SUSTAIN_FRAMES_*`, `RMS_MIN`, `EMA_ALPHA`)
and `audio_processor.c` (`AUDIO_PROC_HARM_PEAK_MIN_SNR`, `AUDIO_PROC_HARM_MIN_H2/H3`).
Enable `BATEAR_AUDIO_PERF_LOG` in menuconfig for per-frame DSP timing.

## Release Tagging

Tags MUST be **annotated** (`git tag -a`), never lightweight. Format: `v<MAJOR>.<MINOR>.<PATCH>`.

### Tag annotation format

```
v1.3.2: Short summary under 60 chars

Bug fixes:
- Fix I2S read tight loop that could spin-lock Core 1

Hardware:
- Add Heltec WiFi LoRa 32 V4 as supported board

CI:
- Parallelize detector and gateway builds
```

- Title line: `v<VERSION>: <summary>`
- Group changes under: `Bug fixes`, `Features`, `Hardware`, `CI`, `Docs`, `Breaking changes` (omit empty categories)
- Each bullet starts with a verb: Fix, Add, Remove, Update, Refactor
- Cover ALL non-merge commits since previous tag: `git log --oneline --no-merges <prev_tag>..HEAD`

### Semver

| Bump | When |
|------|------|
| PATCH | Bug fixes, docs, CI — no new features |
| MINOR | New features, new board support, new config options |
| MAJOR | Breaking changes (protocol, packet format, NVS schema) |

### CI integration

`.github/workflows/esp-idf-build.yml` reacts to tags:

- **`v*` tag push** → build → GitHub Release with tag annotation as body, marked `latest`
- **`main` push** → build → update `firmware-latest` pre-release

Tag on `main` or on a branch **after** merging, so `firmware-latest` and the versioned release stay in sync.

### Checklist

```bash
git tag -l 'v*' --sort=-v:refname | head -1        # previous tag
git log --oneline --no-merges <prev_tag>..HEAD      # changes to document
git tag -a v<X.Y.Z> -m "<annotation>"               # create
git tag -l -n99 v<X.Y.Z>                            # verify
git push origin v<X.Y.Z>                            # push
```
