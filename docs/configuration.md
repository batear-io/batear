# Configuration

All network and role settings live in small sdkconfig files — no `menuconfig` needed for normal operation. A handful of debug-only switches are exposed via `idf.py menuconfig` (see [Diagnostic switches](#diagnostic-switches)).

## Detector config

**`sdkconfig.detector`**

```ini
# Board (use HELTEC_V4 + FLASHSIZE_16MB for V4)
CONFIG_BATEAR_BOARD_HELTEC_V3=y
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y

# Role
CONFIG_BATEAR_ROLE_DETECTOR=y
CONFIG_BATEAR_DEVICE_ID=1

# Network
CONFIG_BATEAR_NET_KEY="DEADBEEFCAFEBABE13374200F00DAA55"
CONFIG_BATEAR_LORA_FREQ=915000
CONFIG_BATEAR_LORA_SYNC_WORD=0x12

# Telemetry heartbeat (jittered ±10% in firmware)
CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN=30
```

## Gateway config

**`sdkconfig.gateway`**

```ini
# Board (use HELTEC_V4 + FLASHSIZE_16MB for V4)
CONFIG_BATEAR_BOARD_HELTEC_V3=y
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y

# Role
CONFIG_BATEAR_ROLE_GATEWAY=y

# Network
CONFIG_BATEAR_NET_KEY="DEADBEEFCAFEBABE13374200F00DAA55"
CONFIG_BATEAR_LORA_FREQ=915000
CONFIG_BATEAR_LORA_SYNC_WORD=0x12

# MQTT / Home Assistant (override via NVS "gateway_cfg" namespace)
CONFIG_BATEAR_WIFI_SSID=""
CONFIG_BATEAR_WIFI_PASS=""
CONFIG_BATEAR_MQTT_BROKER_URL="mqtt://{BROKER_IP}:1883"
CONFIG_BATEAR_MQTT_USER=""
CONFIG_BATEAR_MQTT_PASS=""
CONFIG_BATEAR_GW_DEVICE_ID="gw01"
```

## Wired Detector config

**`sdkconfig.wired_detector`**

```ini
# Board
CONFIG_BATEAR_BOARD_LILYGO_T_ETH_LITE_S3=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

# Role
CONFIG_BATEAR_ROLE_WIRED_DETECTOR=y
CONFIG_BATEAR_DEVICE_ID=1

# MQTT / Home Assistant (override via NVS "wired_cfg" namespace)
CONFIG_BATEAR_MQTT_BROKER_URL="mqtt://{BROKER_IP}:1883"
CONFIG_BATEAR_MQTT_USER=""
CONFIG_BATEAR_MQTT_PASS=""
CONFIG_BATEAR_WIRED_DEVICE_ID="wd01"

# Ethernet — static IP (leave empty for DHCP)
CONFIG_BATEAR_ETH_STATIC_IP=""
CONFIG_BATEAR_ETH_GATEWAY=""
CONFIG_BATEAR_ETH_NETMASK="255.255.255.0"
CONFIG_BATEAR_ETH_DNS=""

# Optional: TF card audio recording knobs (recording is always compiled in
# for the Wired Detector; runtime-disabled if no SD card is present)
# CONFIG_BATEAR_TF_PREROLL_SEC=5
# CONFIG_BATEAR_TF_POSTROLL_SEC=10
# CONFIG_BATEAR_TF_MAX_MB=1024
# CONFIG_BATEAR_TF_NTP_SERVER="pool.ntp.org"
```

!!! note
    The wired detector does not use LoRa, Wi-Fi, or a gateway. It connects directly to the MQTT broker over Ethernet, so it does **not** require the AES-128 `CONFIG_BATEAR_NET_KEY` (that key only secures the LoRa link between Detectors and the Gateway). For transport encryption to the broker, use a `mqtts://` URL.

!!! tip
    To use a static IP, set `CONFIG_BATEAR_ETH_STATIC_IP` at build time or use `set eth_ip 192.168.1.50` via the serial console.  Leave it blank for DHCP (the default).

## Parameter Reference

| Parameter | Description |
|:---|:---|
| `CONFIG_BATEAR_BOARD_*` | Board selection — determines GPIO mapping and `set-target` chip. |
| `CONFIG_ESPTOOLPY_FLASHSIZE_*` | Flash size — must match your board's flash chip. |
| `CONFIG_BATEAR_NET_KEY` | 128-bit AES-GCM key for the LoRa link (32 hex chars). **Must match** between all Detectors and the Gateway. **Detector / Gateway only** — not used by the Wired Detector. Overridden by NVS. |
| `CONFIG_BATEAR_LORA_FREQ` | Centre frequency in kHz: `915000` (US/TW), `868000` (EU), `923000` (AS). Overridden by NVS. |
| `CONFIG_BATEAR_LORA_SYNC_WORD` | Network isolation byte. Different values = invisible to each other. Overridden by NVS. |
| `CONFIG_BATEAR_DEVICE_ID` | Detector / wired detector. Unique ID (0–255). Overridden by NVS. |
| `CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN` | LoRa detector only. Silent-period telemetry interval in minutes (1–60, default 30). Firmware adds ±10% jitter per cycle to avoid fleet-wide sync. Not NVS-overridable (compile-time). |
| `CONFIG_BATEAR_WIFI_SSID` | Gateway Wi-Fi SSID. Overridden by NVS. |
| `CONFIG_BATEAR_WIFI_PASS` | Gateway Wi-Fi password. Overridden by NVS. |
| `CONFIG_BATEAR_MQTT_BROKER_URL` | MQTT broker URI, e.g. `mqtt://192.168.1.100:1883`. |
| `CONFIG_BATEAR_MQTT_USER` | MQTT username. Overridden by NVS. |
| `CONFIG_BATEAR_MQTT_PASS` | MQTT password. Overridden by NVS. |
| `CONFIG_BATEAR_GW_DEVICE_ID` | Gateway ID used in MQTT topics. Overridden by NVS. |
| `CONFIG_BATEAR_WIRED_DEVICE_ID` | Wired detector ID used in MQTT topics. Overridden by NVS. |
| `CONFIG_BATEAR_ETH_STATIC_IP` | Static IP address for wired detector (empty = DHCP). Overridden by NVS. |
| `CONFIG_BATEAR_ETH_GATEWAY` | Default gateway for static IP. Overridden by NVS. |
| `CONFIG_BATEAR_ETH_NETMASK` | Subnet mask (default `255.255.255.0`). Overridden by NVS. |
| `CONFIG_BATEAR_ETH_DNS` | DNS server. If blank, gateway address is used. Overridden by NVS. |
| `CONFIG_BATEAR_HTTP_PORT` | REST API port (wired detector only, default `8080`). |
| `CONFIG_BATEAR_HTTP_AUTH_TOKEN` | Bearer token for POST endpoints (empty = no auth). Overridden by NVS. |
| `CONFIG_BATEAR_TF_PREROLL_SEC` | Pre-roll seconds buffered in PSRAM (default `5`). |
| `CONFIG_BATEAR_TF_POSTROLL_SEC` | Seconds kept after `DRONE_EVENT_CLEAR` (default `10`). |
| `CONFIG_BATEAR_TF_MAX_MB` | FIFO rotation cap, MiB (default `1024`). |
| `CONFIG_BATEAR_TF_RECORD_ALWAYS` | Always-on debug recording, 60 s segments (default off). |
| `CONFIG_BATEAR_TF_MANUAL_ENABLE` | Enable BOOT-button push-to-talk capture (default on with TF). |
| `CONFIG_BATEAR_TF_MANUAL_SEC` | Safety-net auto-stop for stuck BOOT button (default `30` s). |
| `CONFIG_BATEAR_TF_NTP_SERVER` | NTP host for filename timestamps (default `pool.ntp.org`). Overridden by NVS `wired_cfg`→`ntp_server`. |

## Serial Console

After flashing, you can change any configuration value at runtime via the built-in serial console — no recompile needed.

### Connecting

Open a serial terminal to the device at **115200 baud** (e.g. `idf.py monitor`, PuTTY, or `screen /dev/ttyUSB0 115200`). After boot you will see a prompt:

```
batear>
```

### Commands

| Command | Description |
|:---|:---|
| `help` | List all commands |
| `show` | Display NVS-stored values and Kconfig defaults |
| `set <key> <value>` | Write a value to NVS (reboot to apply) |
| `reboot` | Restart the device |

### Available Keys

**Detector and gateway only (LoRa):**

| Key | Format | Example | NVS namespace |
|:---|:---|:---|:---|
| `net_key` | 32 hex chars | `set net_key A1B2C3D4E5F6A7B8C9D0E1F2A3B4C5D6` | `lora_cfg` |
| `lora_freq` | kHz integer | `set lora_freq 868000` | `lora_cfg` |
| `sync_word` | 2 hex chars | `set sync_word 34` | `lora_cfg` |

!!! note
    `net_key` is the AES-128 LoRa pre-shared key. The Wired Detector role does not transmit over LoRa, so it does not expose `net_key` from its serial console — use `mqtts://` if you need transport encryption to the broker.

**Detector and wired detector:**

| Key | Format | Example | NVS namespace |
|:---|:---|:---|:---|
| `device_id` | 0–255 | `set device_id 2` | `lora_cfg` |

**Gateway only:**

| Key | Format | Example | NVS namespace |
|:---|:---|:---|:---|
| `wifi_ssid` | string | `set wifi_ssid MyNetwork` | `gateway_cfg` |
| `wifi_pass` | string | `set wifi_pass s3cretP@ss` | `gateway_cfg` |
| `mqtt_url` | URI | `set mqtt_url mqtt://192.168.1.100:1883` | `gateway_cfg` |
| `mqtt_user` | string | `set mqtt_user ha_user` | `gateway_cfg` |
| `mqtt_pass` | string | `set mqtt_pass ha_pass` | `gateway_cfg` |
| `mqtt_device_id` | string | `set mqtt_device_id gw01` | `gateway_cfg` |

**Wired detector only:**

| Key | Format | Example | NVS namespace |
|:---|:---|:---|:---|
| `mqtt_url` | URI | `set mqtt_url mqtt://192.168.1.100:1883` | `wired_cfg` |
| `mqtt_user` | string | `set mqtt_user ha_user` | `wired_cfg` |
| `mqtt_pass` | string | `set mqtt_pass ha_pass` | `wired_cfg` |
| `mqtt_device_id` | string | `set mqtt_device_id wd01` | `wired_cfg` |
| `eth_ip` | IP address | `set eth_ip 192.168.1.50` (empty = DHCP) | `wired_cfg` |
| `eth_gw` | IP address | `set eth_gw 192.168.1.1` | `wired_cfg` |
| `eth_mask` | IP address | `set eth_mask 255.255.255.0` | `wired_cfg` |
| `eth_dns` | IP address | `set eth_dns 8.8.8.8` (empty = use gateway) | `wired_cfg` |
| `http_token` | string | `set http_token MyS3cretToken` | `wired_cfg` |

### Example Session

```
batear> show

--- Batear Configuration ---

[lora_cfg]  (shared network settings)
  net_key      = (not set)
  lora_freq    = (not set)
  sync_word    = (not set)
  Kconfig defaults:
    net_key    = DEADBEEFCAFEBABE13374200F00DAA55
    lora_freq  = 915000 kHz
    sync_word  = 0x12

[gateway_cfg]  (WiFi / MQTT)
  wifi_ssid    = (not set)
  ...

batear> set wifi_ssid MyHomeWiFi
OK: gateway_cfg:wifi_ssid = "MyHomeWiFi" (reboot to apply)

batear> set wifi_pass s3cretP@ss
OK: gateway_cfg:wifi_pass = "s3cretP@ss" (reboot to apply)

batear> set lora_freq 868000
OK: lora_cfg:lora_freq = 868000 (reboot to apply)

batear> reboot
Rebooting...
```

!!! warning
    `net_key`, `lora_freq`, and `sync_word` must be **identical** on all detectors and the gateway. If any device has a mismatch, packets will fail decryption or be invisible.

!!! note
    NVS values persist across firmware updates. To reset all NVS data, run `idf.py erase-flash` before re-flashing.

## Generate a new encryption key

```bash
python3 -c "import os; print(os.urandom(16).hex().upper())"
```

!!! warning
    The `CONFIG_BATEAR_NET_KEY` must be identical on **all** detectors and gateways in your network. If they don't match, packets will fail decryption silently.

## MQTT / Home Assistant Integration

The gateway connects to Wi-Fi and publishes detection events to an MQTT broker. The wired detector publishes directly over Ethernet. Home Assistant discovers both device types automatically via [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery).

### Credential Priority

Credentials are loaded with **NVS-first, Kconfig-fallback** priority:

1. At boot, the firmware reads from NVS namespaces (`lora_cfg`, `gateway_cfg`, or `wired_cfg` depending on role).
2. If a key is missing (or the namespace doesn't exist), the Kconfig value from the sdkconfig file is used.

This lets you set defaults at compile time and override per-device via the [serial console](#serial-console), the [Web Flasher](https://docs.batear.io/flasher/), or `nvs_partition_gen.py`.

| NVS namespace | NVS key | Kconfig fallback | Roles | Description |
|:---|:---|:---|:---|:---|
| `lora_cfg` | `app_key` | `CONFIG_BATEAR_NET_KEY` | Detector, Gateway | AES-128 LoRa network key (16-byte blob). Wired Detector ignores it. |
| `lora_cfg` | `lora_freq` | `CONFIG_BATEAR_LORA_FREQ` | Detector, Gateway | LoRa frequency in kHz |
| `lora_cfg` | `sync_word` | `CONFIG_BATEAR_LORA_SYNC_WORD` | Detector, Gateway | LoRa sync word |
| `lora_cfg` | `device_id` | `CONFIG_BATEAR_DEVICE_ID` | Detector, Wired | Detector ID, 0–255 |
| `gateway_cfg` | `wifi_ssid` | `CONFIG_BATEAR_WIFI_SSID` | Gateway | Wi-Fi network name |
| `gateway_cfg` | `wifi_pass` | `CONFIG_BATEAR_WIFI_PASS` | Gateway | Wi-Fi password |
| `gateway_cfg` | `mqtt_url` | `CONFIG_BATEAR_MQTT_BROKER_URL` | Gateway | Broker URI (`mqtt://` or `mqtts://`) |
| `gateway_cfg` | `mqtt_user` | `CONFIG_BATEAR_MQTT_USER` | Gateway | Broker username |
| `gateway_cfg` | `mqtt_pass` | `CONFIG_BATEAR_MQTT_PASS` | Gateway | Broker password |
| `gateway_cfg` | `device_id` | `CONFIG_BATEAR_GW_DEVICE_ID` | Gateway | Gateway ID for MQTT topics |
| `wired_cfg` | `mqtt_url` | `CONFIG_BATEAR_MQTT_BROKER_URL` | Wired | Broker URI (`mqtt://` or `mqtts://`) |
| `wired_cfg` | `mqtt_user` | `CONFIG_BATEAR_MQTT_USER` | Wired | Broker username |
| `wired_cfg` | `mqtt_pass` | `CONFIG_BATEAR_MQTT_PASS` | Wired | Broker password |
| `wired_cfg` | `device_id` | `CONFIG_BATEAR_WIRED_DEVICE_ID` | Wired | Wired detector ID for MQTT topics |
| `wired_cfg` | `eth_ip` | `CONFIG_BATEAR_ETH_STATIC_IP` | Wired | Static IP (empty = DHCP) |
| `wired_cfg` | `eth_gw` | `CONFIG_BATEAR_ETH_GATEWAY` | Wired | Default gateway |
| `wired_cfg` | `eth_mask` | `CONFIG_BATEAR_ETH_NETMASK` | Wired | Subnet mask |
| `wired_cfg` | `eth_dns` | `CONFIG_BATEAR_ETH_DNS` | Wired | DNS server (empty = gateway) |
| `wired_cfg` | `http_token` | `CONFIG_BATEAR_HTTP_AUTH_TOKEN` | Wired | Bearer auth token for REST API POST endpoints |

### MQTT Topics

Both the gateway and the wired detector use the same topic structure:

| Topic | QoS | Retained | Description |
|:---|:---|:---|:---|
| `batear/nodes/<id>/status` | 1 | No | Gateway- or wired-detector-wide event stream (JSON). `<id>` is the gateway's or wired detector's own `device_id`. |
| `batear/nodes/<id>/det/<XX>/status` | 1 | Yes | Per-detector events (gateway only, `XX` = hex detector ID). Retained so HA picks up the last known state on reconnect. |
| `batear/nodes/<id>/availability` | 1 | Yes | `online` / `offline` (LWT) |

### Status Payload (JSON)

**Gateway** (relayed from LoRa detector). Every published event — `alarm`, `clear`, or `heartbeat` — carries the same schema. Telemetry fields are piggybacked on alarm/clear events at no extra airtime.

```json
{
  "drone_detected": true,
  "event": "alarm",
  "detector_id": 1,
  "rssi": -90,
  "snr": 5.2,
  "rms_db": 45,
  "f0_bin": 12,
  "seq": 42,
  "battery_v": 3.82,
  "fw_version": "2.0.0",
  "uptime_min": 123,
  "free_heap_kb": 187,
  "tx_fails": 0,
  "flags": 0,
  "timestamp": 1234567
}
```

| Field | Description |
|:---|:---|
| `drone_detected` | `true` for ALARM, `false` otherwise. Heartbeats preserve the last known alarm state, so a detector that armed an alarm and then silently sends heartbeats keeps `drone_detected: true` until a CLEAR event arrives. |
| `event` | `"alarm"`, `"clear"`, or `"heartbeat"` — the raw packet type. |
| `rssi` / `snr` | Received-signal quality at the gateway for this specific packet (dBm / dB). |
| `rms_db` / `f0_bin` | Acoustic signature fields; zero on heartbeat. |
| `seq` | Monotonic packet counter per detector. |
| `battery_v` | Detector battery voltage decoded from `vbat_cv` (0.01 V resolution). |
| `fw_version` | Detector firmware version parsed from its `esp_app_desc` git tag. |
| `uptime_min` / `free_heap_kb` / `tx_fails` / `flags` | Detector health metrics. See [LoRa Protocol](protocol.md) for the full field map. |
| `timestamp` | Gateway wall-clock seconds when the event was published. |

**Wired detector** (published directly):

```json
{
  "drone_detected": true,
  "detector_id": 1,
  "rms_db": 45,
  "f0_bin": 12,
  "seq": 42,
  "confidence": 0.85,
  "timestamp": 1234567
}
```

!!! note
    The wired detector payload includes `confidence` (0–1 harmonic score) instead of `rssi` / `snr` / LoRa telemetry fields, since there is no LoRa link.

### Home Assistant Discovery

Both the gateway and wired detector publish retained HA MQTT Discovery config messages. The gateway publishes in two layers:

**Gateway-wide (at MQTT connect)** — for backwards compatibility with earlier setups:

| Entity | Discovery topic | Type |
|:---|:---|:---|
| Drone Detected | `homeassistant/binary_sensor/batear_<gw>/drone/config` | `binary_sensor` (`safety`) |
| RSSI | `homeassistant/sensor/batear_<gw>/rssi/config` | `sensor` (`signal_strength`, dBm) |
| SNR | `homeassistant/sensor/batear_<gw>/snr/config` | `sensor` (dB) |

**Per-detector (lazy, on first packet from each detector ID)** — each detector appears as a distinct HA device (`batear_<gw>_det_<XX>`) linked back to the gateway via `via_device`:

| Entity | Topic suffix | HA device class / unit |
|:---|:---|:---|
| Drone Detected | `.../drone/config` | `binary_sensor` (`safety`) |
| Battery | `.../battery_v/config` | `voltage`, V |
| Firmware Version | `.../fw_version/config` | diagnostic string |
| Uptime | `.../uptime_min/config` | `duration`, min |
| Free Heap | `.../free_heap_kb/config` | kB, measurement |
| TX Failures | `.../tx_fails/config` | total_increasing |
| RSSI | `.../rssi/config` | `signal_strength`, dBm |
| SNR | `.../snr/config` | dB, measurement |

On MQTT reconnect, the gateway clears its internal "already discovered" cache and republishes the per-detector configs, so a broker that purged retained messages (or a freshly started HA) recovers automatically.

**Wired detector entities:**

| Entity | Discovery topic | Type |
|:---|:---|:---|
| Drone Detected | `homeassistant/binary_sensor/batear_<id>/drone/config` | `binary_sensor` (`safety`) |
| Confidence | `homeassistant/sensor/batear_<id>/confidence/config` | `sensor` (%) |

All entities are grouped under a single HA device: **Batear Wired Detector &lt;id&gt;**.

!!! tip
    Make sure your Home Assistant MQTT integration has **discovery enabled** (the default). Gateway entities appear under a device named `Batear Gateway <id>`; each detector appears under its own device `Batear Detector <XX>` with the gateway set as its parent in HA's device registry.

## REST API (Wired Detector only)

The wired detector exposes an HTTP server for device info, detection status, OTA firmware updates, NVS configuration, reboot, and TF-card recording management. See the dedicated [REST API reference](api.md) for endpoints, authentication, and example requests.

## TF Card Audio Recording (Wired Detector)

microSD audio capture on the **LILYGO T-ETH-Lite S3** (which has an on-board TF slot wired to SPI3 in SDSPI mode). Always compiled in for the Wired Detector role; if no card is inserted, the recorder marks itself unmounted at boot and quietly no-ops without affecting detection / MQTT / HTTP.

### File format

16-bit signed mono PCM, 16 kHz WAV, written to `/sdcard/rec/<wired_id>/<timestamp>_<seq>_<suffix>.wav`. Verify with `ffprobe`:

```text
Stream #0:0: Audio: pcm_s16le, 16000 Hz, mono, s16, 256 kb/s
```

### Triggers

| Source | Filename | Stops when |
|:---|:---|:---|
| `DRONE_EVENT_ALARM` (auto) | `…_alarm.wav` | `DRONE_EVENT_CLEAR` + `BATEAR_TF_POSTROLL_SEC` |
| BOOT button held (push-to-talk) | `…_manual.wav` | BOOT released, OR `BATEAR_TF_MANUAL_SEC` safety timeout |
| Always-on (`BATEAR_TF_RECORD_ALWAYS=y`) | `…_always.wav` | every 60 s a new segment is opened |

All paths flush a `BATEAR_TF_PREROLL_SEC` PSRAM ring into the file head so audio just before the trigger is captured.

The WAV header `data_size` field is rewritten and `fsync()`'d every 10 s during a recording, so a power loss / yanked card mid-recording leaves a parseable file with up to the previous 10 s boundary playable. **For a clean close, wait for the `REC stop: …_alarm.wav dur=…s bytes=…` log line before pulling the card** — only then does the FATFS directory entry get the final size.

### REST endpoints

Recordings are listed, streamed, and deleted over HTTP. See [REST API → Recordings endpoints](api.md#recordings-endpoints) for curl examples and the filename whitelist.

### Storage management

When total recordings size exceeds `BATEAR_TF_MAX_MB`, the oldest WAV is deleted before a new one is opened (FIFO). Disk space is also reported on `batear/nodes/<id>/status` as `tf_used_mb`, `tf_free_mb`, `last_recording`, exposed via HA Discovery as diagnostic sensors.

### Hot-plug and failure handling

- **No card at boot**: `tf_recorder_init()` logs `SD mount failed: …` once and marks the recorder unmounted. Detector, MQTT, HTTP API all continue normally; HTTP recording endpoints return `503`.
- **Card yanked while running**: the first I/O error (`EIO` / `ENODEV` / `ENXIO`) flips the recorder into the same unmounted state with a single `SD I/O error — card removed?` warning. No retry storms — diskio errors stop after one line. Any open WAV is `fclose()`'d (truncated to the last 10 s `fsync` boundary).
- **Re-insert**: not detected at runtime. RST / power-cycle the board to re-mount.
- **Mic latch-up after yank**: the ICS-43434 occasionally stops sampling after the 3.3 V transient of a card yank. Audio task tries one `i2s_channel_disable/enable` after 30 s of silence, then leaves it alone — the mic recovers on its own in 1–3 minutes when its internal brown-out logic releases. RST the board if you can't wait.

### Caveats

- The BOOT button (GPIO 0) is a strap pin — recording configures it as input **after** Ethernet comes up, so `idf.py flash` is unaffected even if you hold BOOT during reset.
- Until the first NTP sync, filenames use a `bootNNNNNNNNN` fallback derived from uptime.
- LILYGO T-ETH-Lite S3 wires the SD slot to a **SPI** footprint (not SDMMC). The recorder uses `esp_vfs_fat_sdspi_mount` on `SPI3_HOST` so it doesn't conflict with W5500 on `SPI2_HOST`. See `docs/hardware.md` for pinout.

## Diagnostic switches

These are off by default and only useful for bring-up / tuning. Enable via `idf.py menuconfig` → **Batear config**.

| Symbol | What it does |
|:---|:---|
| `BATEAR_AUDIO_PERF_LOG` | Every ~10 s, log min/avg/max microseconds for FFT-PSD and harmonic analysis. Use to confirm DSP fits in the 100 ms hop budget. |
| `BATEAR_AUDIO_DEBUG_LOG` | Every ~1 s, log per-frame `rms`, `harm_ok`, `conf_ema`, `f0`, `h2`, `h3`, `snr`, `alarm` flag. Use to diagnose why the state machine isn't entering ALARM with a known sound source. |
| `BATEAR_I2S_MIC_SLOT_RIGHT` | Take the RIGHT I2S slot instead of LEFT. Set this if your ICS-43434's `L/R` pin is tied to 3.3 V instead of GND. |
