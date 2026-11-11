# LoRa Packet Protocol

Frame v2 (firmware v3.0+): AES-128-GCM encrypted, **38 bytes on the wire**:

```
[6B cleartext header] [16B ciphertext] [16B GCM auth tag]
```

## Header (6 bytes, cleartext, authenticated)

| Offset | Size | Field | Notes |
|:---|:---|:---|:---|
| 0 | 1 | `ver_ttl` | High nibble = frame version (`2`), low nibble = mesh TTL (default `3`) |
| 1 | 1 | `orig_id` | Originating node's `device_id` |
| 2 | 2 | `boot_id` | Per-node boot counter, little-endian. Persisted in its own NVS partition (`bootctr`, after the app, so reflashing doesn't reset it), incremented every boot and whenever `seq` would wrap |
| 4 | 2 | `seq` | Per-boot packet counter, little-endian, starts at 0 |

`(orig_id, boot_id, seq)` is unique for every packet a network key ever encrypts. It is used three ways:

- **GCM IV** (12 bytes): `[ver][orig_id][boot_id][seq][6B zero]`.
- **Replay key** on the receiver (see below).
- **Dedup key** for mesh relays — no decryption needed to recognise a duplicate.

The header is passed to GCM as additional authenticated data with the TTL nibble cleared, so a relay can decrement TTL without breaking the tag, while any change to version, origin, boot or seq fails authentication. The plaintext repeats `device_id` and `seq`; the receiver rejects the packet if they don't match the header.

The pre-v3.0 36-byte frame (`[4B seq nonce][16B ct][16B tag]`, IV = `[8B key prefix][nonce]`) reused GCM IVs across detectors and across reboots. It is not accepted by v3.0+ gateways, which log a hint to upgrade the detector.

## Plaintext format (16 bytes)

| Offset | Size | Field | Notes |
|:---|:---|:---|:---|
| 0 | 2 | `seq` | Same as header `seq` |
| 2 | 1 | `device_id` | 0–255, unique per detector |
| 3 | 1 | `event_type` | `0x00` CLEAR, `0x01` ALARM, `0x02` TELEMETRY (heartbeat) |
| 4 | 1 | `f0_bin` | Harmonic fundamental bin index (`0` on heartbeat) |
| 5 | 1 | `rms_db` | Frame RMS, dBFS + 96 offset (`0` on heartbeat) |
| 6 | 1 | `vbat_cv` | Battery voltage: `V = 2.50 + vbat_cv × 0.01`, range 2.50–5.05 V |
| 7 | 1 | `fw_major` | Parsed from `esp_app_get_description()` |
| 8 | 1 | `fw_minor` | |
| 9 | 1 | `fw_patch` | |
| 10 | 2 | `uptime_min` | Minutes since boot, clamped to 65535 (~45 days) |
| 12 | 2 | `free_heap_kb` | `esp_get_free_heap_size() / 1024` |
| 14 | 1 | `tx_fail_count` | Monotonic local TX failure counter, clamped to 255 |
| 15 | 1 | `flags` | Bit 0 = low battery (<3.4 V), bit 1 = USB present (reserved) |

Every packet type (CLEAR / ALARM / TELEMETRY) carries the full 16-byte plaintext. Alarm/clear events piggyback telemetry so Home Assistant gets a fresh battery/heap/uptime reading on every state transition, at zero extra airtime.

## Event delivery

### Alarm and clear events

Sent the instant the audio state machine transitions between SAFE and ALARM. These events are latency-sensitive and get **one local retry** on a randomised 150–300 ms backoff if `SX1262->transmit()` returns a local error (SPI busy, TCXO not settled, etc.). The retry reuses the **same `seq` and the same ciphertext**, so if the first packet actually made it over the air but was mis-reported locally, the gateway's replay counter dedupes the duplicate.

### Telemetry heartbeat

Sent every `CONFIG_BATEAR_TELEMETRY_HEARTBEAT_MIN` minutes (default **30**) during silent periods, with a **±10% uniform jitter** applied per cycle so a fleet of detectors booted together desynchronises instead of colliding on shared airtime.

Heartbeats are **fire-and-forget** — no retry. The next interval carries a fresh snapshot, so retrying would only waste airtime and risk correlated collisions with the main heartbeat wave.

## No ACK, by design

The gateway is a passive receiver. There is no PHY-level acknowledgement:

- `transmit()=OK` only guarantees the bits left the SX1262. The detector cannot know whether the packet was received, decrypted, or dropped in flight.
- Adding ACKs would triple the network's airtime (detector TX → gateway TX ACK → detector RX window), block the gateway from hearing other nodes during its TX burst, and scale poorly past a handful of detectors.
- Instead, **idempotency and reliability come from the `(boot_id, seq)` counter**: the gateway tracks the last pair per detector and accepts a packet only if its `boot_id` is newer (16-bit serial-number arithmetic) or the `boot_id` matches and `seq` is higher. Retried packets that arrive twice are dedup'd, and a rebooted detector is accepted immediately because its `boot_id` advanced. A permanently lost packet is covered by the next state transition (for alarm/clear) or the next heartbeat (for telemetry).

If a detector's flash is fully erased (e.g. the web flasher's "Erase device" option), its `boot_id` restarts from a random value. A normal reflash does not reset it. The gateway may then treat it as older and reject it until the gateway is rebooted; the gateway logs `REPLAY ... (last boot=... seq=...)` when this happens.

## Airtime budget

At SF10 / BW125 / CR4/5 the 38-byte packet is ~494 ms on air (48 payload symbols — the same as the old 36-byte frame). The default 30 min heartbeat puts each detector at ~1 s/hour of TX time (0.028% duty cycle), leaving generous headroom for the EU 868 MHz 1%/hour cap and essentially unlimited room on the US/AS 915 MHz ISM bands.

## Security features

- **Unique IV per packet** from `(orig_id, boot_id, seq)` — GCM's security depends on never reusing an IV under the same key.
- **`(boot_id, seq)` counter** prevents replay attacks and deduplicates retries.
- **Authenticated header** — origin, boot and seq can't be altered in flight; only TTL is mutable.
- **GCM auth tag** ensures integrity and authenticity — a tampered or forged packet fails the tag check and is dropped.
- **Sync word** provides RF-level channel isolation so neighbouring batear networks (or unrelated LoRa traffic with a different sync word) never reach the decrypt stage.
