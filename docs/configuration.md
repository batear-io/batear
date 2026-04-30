# Configuration

All network and role settings live in small sdkconfig files — no `menuconfig` needed.

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

# Network key (shared with LoRa devices if co-deployed)
CONFIG_BATEAR_NET_KEY="DEADBEEFCAFEBABE13374200F00DAA55"

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
```

!!! note
    The wired detector does not use LoRa, Wi-Fi, or a gateway. It connects directly to the MQTT broker over Ethernet.

!!! tip
    To use a static IP, set `CONFIG_BATEAR_ETH_STATIC_IP` at build time or use `set eth_ip 192.168.1.50` via the serial console.  Leave it blank for DHCP (the default).

## Parameter Reference

| Parameter | Description |
|:---|:---|
| `CONFIG_BATEAR_BOARD_*` | Board selection — determines GPIO mapping and `set-target` chip. |
| `CONFIG_ESPTOOLPY_FLASHSIZE_*` | Flash size — must match your board's flash chip. |
| `CONFIG_BATEAR_NET_KEY` | 128-bit AES-GCM key (32 hex chars). **Must match** between all devices. Overridden by NVS. |
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

**All roles:**

| Key | Format | Example | NVS namespace |
|:---|:---|:---|:---|
| `net_key` | 32 hex chars | `set net_key A1B2C3D4E5F6A7B8C9D0E1F2A3B4C5D6` | `lora_cfg` |

**Detector and gateway only (LoRa):**

| Key | Format | Example | NVS namespace |
|:---|:---|:---|:---|
| `lora_freq` | kHz integer | `set lora_freq 868000` | `lora_cfg` |
| `sync_word` | 2 hex chars | `set sync_word 34` | `lora_cfg` |

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
| `lora_cfg` | `app_key` | `CONFIG_BATEAR_NET_KEY` | All | AES-128 network key (16-byte blob) |
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

The wired detector runs an HTTP server on port **8080** (configurable via `CONFIG_BATEAR_HTTP_PORT`) that exposes device info, detection status, OTA firmware updates, NVS configuration, and reboot.

### Endpoints

| Method | Path | Auth | Description |
|:---|:---|:---|:---|
| `GET` | `/api/info` | No | Device metadata (version, uptime, free heap, partition) |
| `GET` | `/api/status` | No | Current detection state (drone_detected, confidence, rms_db) |
| `POST` | `/api/ota` | Bearer | Upload firmware binary for OTA update |
| `POST` | `/api/config` | Bearer | Update NVS config keys (JSON body) |
| `POST` | `/api/reboot` | Bearer | Reboot the device |

### Authentication

POST endpoints optionally require a Bearer token. Set it via:

- **Kconfig**: `CONFIG_BATEAR_HTTP_AUTH_TOKEN`
- **NVS**: `set http_token MyS3cretToken` via serial console
- **API**: `POST /api/config` with `{"http_token":"newtoken"}`

If the token is empty (default), POST endpoints are accessible without authentication.

Include the token in requests:

```bash
curl -H "Authorization: Bearer MyS3cretToken" -X POST http://<ip>:8080/api/reboot
```

### GET /api/info

Returns device metadata:

```json
{
  "app_name": "batear",
  "version": "v1.2.0",
  "idf_version": "v6.0",
  "compile_date": "Apr  7 2026 12:00:00",
  "partition": "ota_0",
  "free_heap": 245760,
  "uptime_s": 3600
}
```

### GET /api/status

Returns the latest detection state:

```json
{
  "drone_detected": true,
  "detector_id": 1,
  "rms_db": 45,
  "f0_bin": 12,
  "confidence": 0.8500,
  "timestamp": 3600
}
```

### POST /api/ota

Upload a firmware binary to perform an over-the-air update. The device uses a two-OTA-partition layout with automatic rollback protection.

```bash
curl -X POST --data-binary @firmware.bin \
  -H "Authorization: Bearer MyS3cretToken" \
  http://<ip>:8080/api/ota
```

Response:

```json
{"status":"ok","bytes":1234567,"message":"rebooting in 1s"}
```

The device reboots after 1 second. On next boot, the new firmware calls `esp_ota_mark_app_valid_cancel_rollback()` to confirm the update. If the new firmware crashes before reaching that point, the bootloader automatically rolls back to the previous version.

### POST /api/config

Update NVS keys using a flat JSON object. The same keys available via the serial console are supported:

```bash
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Bearer MyS3cretToken" \
  -d '{"mqtt_url":"mqtt://192.168.1.100:1883","eth_ip":"192.168.1.50"}' \
  http://<ip>:8080/api/config
```

Response:

```json
{"status":"ok","keys_written":2,"note":"reboot to apply"}
```

Valid keys: `mqtt_url`, `mqtt_user`, `mqtt_pass`, `device_id`, `eth_ip`, `eth_gw`, `eth_mask`, `eth_dns`, `http_token`.

!!! warning
    Config changes take effect after reboot. Use `POST /api/reboot` or the serial console `reboot` command.

### POST /api/reboot

Reboots the device after responding:

```bash
curl -X POST -H "Authorization: Bearer MyS3cretToken" http://<ip>:8080/api/reboot
```

```json
{"status":"ok","message":"rebooting in 1s"}
```
