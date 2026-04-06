# Configuration

All network and role settings live in two small files — no `menuconfig` needed.

## Detector config

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

## Gateway config

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

# MQTT / Home Assistant (override via NVS "gateway_cfg" namespace)
CONFIG_BATEAR_WIFI_SSID=""
CONFIG_BATEAR_WIFI_PASS=""
CONFIG_BATEAR_MQTT_BROKER_URL="mqtt://{BROKER_IP}:1883"
CONFIG_BATEAR_MQTT_USER=""
CONFIG_BATEAR_MQTT_PASS=""
CONFIG_BATEAR_GW_DEVICE_ID="gw01"
```

## Parameter Reference

| Parameter | Description |
|:---|:---|
| `CONFIG_BATEAR_BOARD_*` | Board selection — determines GPIO mapping and `set-target` chip. |
| `CONFIG_ESPTOOLPY_FLASHSIZE_*` | Flash size — must match your board's flash chip. |
| `CONFIG_BATEAR_NET_KEY` | 128-bit AES-GCM key (32 hex chars). **Must match** between all devices. Overridden by NVS. |
| `CONFIG_BATEAR_LORA_FREQ` | Centre frequency in kHz: `915000` (US/TW), `868000` (EU), `923000` (AS). Overridden by NVS. |
| `CONFIG_BATEAR_LORA_SYNC_WORD` | Network isolation byte. Different values = invisible to each other. Overridden by NVS. |
| `CONFIG_BATEAR_DEVICE_ID` | Detector only. Unique ID (0–255) shown on gateway display. Overridden by NVS. |
| `CONFIG_BATEAR_WIFI_SSID` | Gateway Wi-Fi SSID. Overridden by NVS. |
| `CONFIG_BATEAR_WIFI_PASS` | Gateway Wi-Fi password. Overridden by NVS. |
| `CONFIG_BATEAR_MQTT_BROKER_URL` | MQTT broker URI, e.g. `mqtt://192.168.1.100:1883`. |
| `CONFIG_BATEAR_MQTT_USER` | MQTT username. Overridden by NVS. |
| `CONFIG_BATEAR_MQTT_PASS` | MQTT password. Overridden by NVS. |
| `CONFIG_BATEAR_GW_DEVICE_ID` | Gateway ID used in MQTT topics. Overridden by NVS. |

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

**Both roles** (detector and gateway):

| Key | Format | Example | NVS namespace |
|:---|:---|:---|:---|
| `net_key` | 32 hex chars | `set net_key A1B2C3D4E5F6A7B8C9D0E1F2A3B4C5D6` | `lora_cfg` |
| `lora_freq` | kHz integer | `set lora_freq 868000` | `lora_cfg` |
| `sync_word` | 2 hex chars | `set sync_word 34` | `lora_cfg` |

**Detector only:**

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
| `device_id` | string | `set device_id gw01` | `gateway_cfg` |

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

The gateway connects to Wi-Fi and publishes detection events to an MQTT broker. Home Assistant discovers the gateway automatically via [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery).

### Credential Priority

Credentials are loaded with **NVS-first, Kconfig-fallback** priority:

1. At boot, the firmware reads from NVS namespaces `lora_cfg` and `gateway_cfg`.
2. If a key is missing (or the namespace doesn't exist), the Kconfig value from `sdkconfig.detector` / `sdkconfig.gateway` is used.

This lets you set defaults at compile time and override per-device via the [serial console](#serial-console), the [Web Flasher](https://docs.batear.io/flasher/), or `nvs_partition_gen.py`.

| NVS namespace | NVS key | Kconfig fallback | Description |
|:---|:---|:---|:---|
| `lora_cfg` | `app_key` | `CONFIG_BATEAR_NET_KEY` | AES-128 network key (16-byte blob) |
| `lora_cfg` | `lora_freq` | `CONFIG_BATEAR_LORA_FREQ` | LoRa frequency in kHz |
| `lora_cfg` | `sync_word` | `CONFIG_BATEAR_LORA_SYNC_WORD` | LoRa sync word |
| `lora_cfg` | `device_id` | `CONFIG_BATEAR_DEVICE_ID` | Detector ID, 0–255 (detector only) |
| `gateway_cfg` | `wifi_ssid` | `CONFIG_BATEAR_WIFI_SSID` | Wi-Fi network name |
| `gateway_cfg` | `wifi_pass` | `CONFIG_BATEAR_WIFI_PASS` | Wi-Fi password |
| `gateway_cfg` | `mqtt_url` | `CONFIG_BATEAR_MQTT_BROKER_URL` | Broker URI (`mqtt://` or `mqtts://`) |
| `gateway_cfg` | `mqtt_user` | `CONFIG_BATEAR_MQTT_USER` | Broker username |
| `gateway_cfg` | `mqtt_pass` | `CONFIG_BATEAR_MQTT_PASS` | Broker password |
| `gateway_cfg` | `device_id` | `CONFIG_BATEAR_GW_DEVICE_ID` | Gateway ID for MQTT topics |

### MQTT Topics

| Topic | QoS | Retained | Description |
|:---|:---|:---|:---|
| `batear/nodes/<id>/status` | 1 | No | Detection events (JSON) |
| `batear/nodes/<id>/det/<XX>/status` | 1 | No | Per-detector events (`XX` = hex detector ID) |
| `batear/nodes/<id>/availability` | 1 | Yes | `online` / `offline` (LWT) |

### Status Payload (JSON)

```json
{
  "drone_detected": true,
  "detector_id": 1,
  "rssi": -90,
  "snr": 5.2,
  "rms_db": 45,
  "f0_bin": 12,
  "seq": 42,
  "timestamp": 1234567
}
```

### Home Assistant Discovery

On MQTT connect, the gateway publishes retained config messages to HA's discovery prefix:

| Entity | Discovery topic | Type |
|:---|:---|:---|
| Drone Detected | `homeassistant/binary_sensor/batear_<id>/drone/config` | `binary_sensor` (`safety`) |
| RSSI | `homeassistant/sensor/batear_<id>/rssi/config` | `sensor` (`signal_strength`, dBm) |
| SNR | `homeassistant/sensor/batear_<id>/snr/config` | `sensor` (dB) |

All entities are grouped under a single HA device: **Batear Gateway &lt;id&gt;**.

!!! tip
    Make sure your Home Assistant MQTT integration has **discovery enabled** (the default). The gateway entities will appear automatically under **Settings → Devices & Services → MQTT**.
