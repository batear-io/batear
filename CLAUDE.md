# batear

ESP32-S3 acoustic drone detection with encrypted LoRa alerting or direct Ethernet/MQTT. ESP-IDF 6.x.

Same codebase builds as **Detector**, **Gateway**, or **Wired Detector** via sdkconfig files.

## Board → Target

| Board | `set-target` | Flash |
|---|---|---|
| Heltec WiFi LoRa 32 V3 | `esp32s3` | 8 MB |
| Heltec WiFi LoRa 32 V4 | `esp32s3` | 16 MB |
| LILYGO T-ETH-Lite S3 | `esp32s3` | 16 MB |

## Build & Flash

`set-target` depends on board chip — see table above and `BOARD_IDF_TARGET` in `pin_config.h`.

```bash
# First time — set target (once per build directory)
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig \
 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.detector" set-target esp32s3
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig \
 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.gateway" set-target esp32s3
idf.py -B build_wired_detector -DSDKCONFIG=build_wired_detector/sdkconfig \
 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.wired_detector" set-target esp32s3

# Build
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig build
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig build
idf.py -B build_wired_detector -DSDKCONFIG=build_wired_detector/sdkconfig build

# Flash
idf.py -B build_detector -DSDKCONFIG=build_detector/sdkconfig -p PORT flash monitor
idf.py -B build_gateway -DSDKCONFIG=build_gateway/sdkconfig -p PORT flash monitor
idf.py -B build_wired_detector -DSDKCONFIG=build_wired_detector/sdkconfig -p PORT flash monitor
```

## Configuration Files

| File | Purpose |
|---|---|
| `sdkconfig.defaults` | Common ESP-IDF settings (CPU freq, logging, stack) |
| `sdkconfig.detector` | Board, flash size, role, device ID, network config |
| `sdkconfig.gateway` | Board, flash size, role, network config |
| `sdkconfig.wired_detector` | Board, flash size, role, MQTT config (Ethernet) |

Key parameters in sdkconfig role files:

| Config | Description |
|---|---|
| `CONFIG_BATEAR_BOARD_*` | Board selection (determines GPIO mapping) |
| `CONFIG_ESPTOOLPY_FLASHSIZE_*` | Flash size (must match board hardware) |
| `CONFIG_BATEAR_NET_KEY` | 32-char hex AES-128 key. Must match on all devices. (overridden by NVS) |
| `CONFIG_BATEAR_LORA_FREQ` | LoRa frequency in kHz (detector/gateway only). (overridden by NVS) |
| `CONFIG_BATEAR_LORA_SYNC_WORD` | LoRa network isolation (detector/gateway only). (overridden by NVS) |
| `CONFIG_BATEAR_DEVICE_ID` | Detector/wired detector only, 0–255. (overridden by NVS) |
| `CONFIG_BATEAR_WIFI_SSID` | Gateway Wi-Fi SSID (overridden by NVS) |
| `CONFIG_BATEAR_WIFI_PASS` | Gateway Wi-Fi password (overridden by NVS) |
| `CONFIG_BATEAR_MQTT_BROKER_URL` | MQTT broker URI, e.g. `mqtt://ha.local:1883` |
| `CONFIG_BATEAR_MQTT_USER` | MQTT username (overridden by NVS) |
| `CONFIG_BATEAR_MQTT_PASS` | MQTT password (overridden by NVS) |
| `CONFIG_BATEAR_GW_DEVICE_ID` | Gateway ID for MQTT topics (overridden by NVS) |
| `CONFIG_BATEAR_WIRED_DEVICE_ID` | Wired detector ID for MQTT topics (overridden by NVS) |
| `CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN` | Detector only, 1–60. Silent-period telemetry interval in minutes (default 30). Jittered ±10% in firmware. |

## Project Structure

```
batear/
├── CMakeLists.txt
├── sdkconfig.defaults
├── sdkconfig.detector
├── sdkconfig.gateway
├── sdkconfig.wired_detector
├── main/
│   ├── CMakeLists.txt          # conditional compile by role
│   ├── Kconfig.projbuild       # role / device ID / network / debug config
│   ├── main.cpp                # entry point (role switch)
│   ├── pin_config.h            # board-specific GPIO + hardware traits
│   ├── drone_detector.h        # shared DroneEvent_t + queue
│   ├── lora_crypto.h           # AES-128-GCM packet protocol (PSA API)
│   ├── EspIdfHal.cpp/.h        # RadioLib HAL for ESP-IDF
│   ├── config_console.c/.h     # serial console (show/set/reboot)
│   ├── audio_processor.c/.h    # [detector/wired] ESP-DSP FFT + PSD + harmonic analysis
│   ├── audio_task.c/.h         # [detector/wired] I2S mic + detection state machine
│   ├── battery.c/.h            # [detector] VBAT ADC + divider gating
│   ├── lora_task.cpp/.h        # [detector] LoRa TX (event + heartbeat, jittered)
│   ├── eth_mqtt_task.cpp/.h    # [wired]    W5500 Ethernet + MQTT + HA Discovery
│   ├── gateway_task.cpp/.h     # [gateway]  LoRa RX + OLED + LED
│   ├── mqtt_task.cpp/.h        # [gateway]  WiFi + MQTT + HA Discovery
│   ├── oled.c/.h               # [gateway]  SSD1306 128x64 driver
│   └── idf_component.yml       # RadioLib + ESP-DSP + W5500 dependencies
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
| VBAT ADC | 1 | detector only, ADC1_CH0, read through ~4.9× divider |
| VBAT ADC Ctrl | 37 | detector only, active low to enable the divider |

## Pin Map (pin_config.h, LILYGO T-ETH-Lite S3)

| Function | GPIO | Notes |
|---|---|---|
| I2S BCLK | 38 | ICS-43434 SCK (extension header) |
| I2S WS | 39 | ICS-43434 LRCLK |
| I2S DIN | 40 | ICS-43434 SD |
| ETH SCLK | 10 | W5500, on-board |
| ETH MOSI | 12 | on-board |
| ETH MISO | 11 | on-board |
| ETH CS | 9 | W5500 chip select |
| ETH INT | 13 | W5500 interrupt |
| ETH RST | 14 | W5500 reset |

## Calibration

When alarm is active, serial prints:
`cal: f0=XXX.X Hz h2=X.XX h3=X.XX snr=XX.X nf=X.XXeXX conf_ema=X.XX rms=X.XXXXX`

Tune detection in `audio_task.c` (`HARM_F0_MIN/MAX_HZ`, `CONF_ON/OFF`, `SUSTAIN_FRAMES_*`, `RMS_MIN`, `EMA_ALPHA`)
and `audio_processor.c` (`AUDIO_PROC_HARM_PEAK_MIN_SNR`, `AUDIO_PROC_HARM_MIN_H2/H3`).
Enable `BATEAR_AUDIO_PERF_LOG` in menuconfig for per-frame DSP timing.

## Telemetry & LoRa protocol

The detector transmits AES-128-GCM encrypted packets to the gateway on three triggers:

| Event type | Byte | Trigger |
|---|---|---|
| `CLEAR` | `0x00` | audio state machine ALARM → SAFE transition |
| `ALARM` | `0x01` | audio state machine SAFE → ALARM transition |
| `TELEMETRY` | `0x02` | silent-period heartbeat (`BATEAR_TELEMETRY_HEARTBEAT_MIN`, ±10% jitter) |

Every packet carries the same telemetry fields (battery voltage, firmware version, uptime, free heap, cumulative TX failure counter, flags) so alarm/clear events piggyback diagnostics without extra airtime.

Wire format is 36 bytes: `[4B nonce][16B ciphertext][16B GCM tag]`. See `main/lora_crypto.h` for the plaintext struct. **Packet format is not backward compatible** with the pre-2.x 28-byte layout — detectors and gateways must be upgraded together.

Alarm/clear events get **one local retry** with a 150–300 ms randomised backoff if `SX1262->transmit()` reports a local error. Heartbeats are fire-and-forget (the next interval refreshes the same snapshot). There is **no ACK** — the gateway uses `pt.seq <= dev->last_seq` as a replay counter to dedup duplicate RX from retries.

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
