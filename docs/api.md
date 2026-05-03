# REST API (Wired Detector only)

The wired detector runs an HTTP server on port **8080** (configurable via `CONFIG_BATEAR_HTTP_PORT`) that exposes device info, detection status, OTA firmware updates, NVS configuration, and reboot.

## Endpoints

| Method | Path | Auth | Description |
|:---|:---|:---|:---|
| `GET` | `/api/info` | No | Device metadata (version, uptime, free heap, partition) |
| `GET` | `/api/status` | No | Current detection state (drone_detected, confidence, rms_db) |
| `GET` | `/api/recordings` | No | List captured WAV files (TF recorder only). |
| `GET` | `/api/recordings/storage` | No | TF storage stats (mounted, used/free MiB, drops). |
| `GET` | `/api/recordings/<file>` | No | Stream a specific WAV (chunked, `audio/wav`). |
| `DELETE` | `/api/recordings/<file>` | Bearer | Delete a specific WAV (TF recorder only). |
| `POST` | `/api/ota` | Bearer | Upload firmware binary for OTA update |
| `POST` | `/api/config` | Bearer | Update NVS config keys (JSON body) |
| `POST` | `/api/reboot` | Bearer | Reboot the device |

## Authentication

POST endpoints optionally require a Bearer token. Set it via:

- **Kconfig**: `CONFIG_BATEAR_HTTP_AUTH_TOKEN`
- **NVS**: `set http_token MyS3cretToken` via serial console
- **API**: `POST /api/config` with `{"http_token":"newtoken"}`

If the token is empty (default), POST endpoints are accessible without authentication.

Include the token in requests:

```bash
curl -H "Authorization: Bearer MyS3cretToken" -X POST http://<ip>:8080/api/reboot
```

## GET /api/info

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

## GET /api/status

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

## POST /api/ota

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

## POST /api/config

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

Valid keys: `mqtt_url`, `mqtt_user`, `mqtt_pass`, `device_id`, `eth_ip`, `eth_gw`, `eth_mask`, `eth_dns`, `http_token`, `ntp_server`.

!!! warning
    Config changes take effect after reboot. Use `POST /api/reboot` or the serial console `reboot` command.

## POST /api/reboot

Reboots the device after responding:

```bash
curl -X POST -H "Authorization: Bearer MyS3cretToken" http://<ip>:8080/api/reboot
```

```json
{"status":"ok","message":"rebooting in 1s"}
```

## Recordings endpoints

```bash
# List all recordings
curl http://<ip>:8080/api/recordings

# Storage stats
curl http://<ip>:8080/api/recordings/storage

# Download one
curl -o sample.wav http://<ip>:8080/api/recordings/20260501-073000_00012_alarm.wav

# Delete one (Bearer required)
curl -X DELETE -H "Authorization: Bearer MyS3cretToken" \
  http://<ip>:8080/api/recordings/20260501-073000_00012_alarm.wav
```

`<file>` whitelist: `[A-Za-z0-9_.-]+`, no leading dot, no `..`. Returns `503` if the SD card isn't mounted, `400` on a bad name, `404` if missing.

See [TF Card Audio Recording](configuration.md#tf-card-audio-recording-wired-detector) for file format, triggers, and storage semantics.
