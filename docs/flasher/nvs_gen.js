/**
 * nvs_gen.js — Browser-side ESP32 NVS partition image generator
 *
 * Generates a 4 KB (one page) NVS binary image containing:
 *   namespace "lora_cfg"
 *     blob "dev_eui"   (8 bytes)
 *     blob "app_key"   (16 bytes)
 *     u8   "device_id" (1 byte, detector only)
 *
 *   namespace "gateway_cfg"  (gateway only)
 *     str  "wifi_ssid"
 *     str  "wifi_pass"
 *     str  "mqtt_url"
 *     str  "mqtt_user"
 *     str  "mqtt_pass"
 *     str  "device_id"
 *
 * The output can be flashed to the NVS partition offset (0x9000 for
 * default single-app partition table) via esptool / esp-web-tools.
 *
 * NVS format reference: ESP-IDF components/nvs_flash
 */

const NVS_PAGE_SIZE  = 4096;
const ENTRY_SIZE     = 32;
const HEADER_SIZE    = 32;
const BITMAP_SIZE    = 32;
const ENTRIES_OFFSET = HEADER_SIZE + BITMAP_SIZE;  // 0x40

const PAGE_STATE_ACTIVE = 0xFFFFFFFE;
const NVS_VERSION       = 0xFE;

// NVS item types (from ESP-IDF nvs.h / nvs_handle.hpp)
const TYPE_U8         = 0x01;
const TYPE_STR        = 0x21;  // NVS_TYPE_STR
const TYPE_BLOB_DATA  = 0x42;  // NVS_TYPE_BLOB
const TYPE_BLOB_IDX   = 0x48;

// Entry states (2 bits per entry)
const ES_WRITTEN = 0x02;  // 10b
const ES_EMPTY   = 0x03;  // 11b

/* ------------------------------------------------------------------ */
/*  CRC-32 (LE, polynomial 0xEDB88320 — matches ESP-IDF crc32_le)    */
/* ------------------------------------------------------------------ */

let _crc32Table = null;

function crc32Table() {
  if (_crc32Table) return _crc32Table;
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let j = 0; j < 8; j++) {
      c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    }
    t[i] = c;
  }
  _crc32Table = t;
  return t;
}

function crc32(buf, init = 0) {
  const t = crc32Table();
  let crc = init ^ 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) {
    crc = t[(crc ^ buf[i]) & 0xFF] ^ (crc >>> 8);
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

/* ------------------------------------------------------------------ */
/*  Little-endian helpers                                              */
/* ------------------------------------------------------------------ */

function writeU16(buf, offset, val) {
  buf[offset]     = val & 0xFF;
  buf[offset + 1] = (val >>> 8) & 0xFF;
}

function writeU32(buf, offset, val) {
  buf[offset]     = val & 0xFF;
  buf[offset + 1] = (val >>> 8) & 0xFF;
  buf[offset + 2] = (val >>> 16) & 0xFF;
  buf[offset + 3] = (val >>> 24) & 0xFF;
}

function writeStr(buf, offset, str, maxLen) {
  for (let i = 0; i < maxLen; i++) {
    buf[offset + i] = i < str.length ? str.charCodeAt(i) : 0;
  }
}

/* ------------------------------------------------------------------ */
/*  Entry bitmap                                                       */
/* ------------------------------------------------------------------ */

function setEntryState(bitmap, index, state) {
  const byteIdx = Math.floor(index / 4);
  const bitIdx  = (index % 4) * 2;
  bitmap[byteIdx] &= ~(0x03 << bitIdx);
  bitmap[byteIdx] |= (state << bitIdx);
}

/* ------------------------------------------------------------------ */
/*  Entry-level CRC                                                    */
/*  Covers entry[0:4] (ns,type,span,chunk) + entry[8:32] (key+data)   */
/*  = 28 bytes, init = 0xFFFFFFFF  (matches ESP-IDF set_crc_header)    */
/* ------------------------------------------------------------------ */

function entryItemCrc(page, entryOffset) {
  const crcBuf = new Uint8Array(28);
  crcBuf.set(page.subarray(entryOffset, entryOffset + 4));         // [0:4]
  crcBuf.set(page.subarray(entryOffset + 8, entryOffset + 32), 4); // [8:32]
  return crc32(crcBuf, 0xFFFFFFFF);
}

/* ------------------------------------------------------------------ */
/*  Build a single NVS entry (header portion)                          */
/* ------------------------------------------------------------------ */

function writeEntryHeader(page, entryIdx, ns, type, span, chunkIdx, key) {
  const off = ENTRIES_OFFSET + entryIdx * ENTRY_SIZE;
  page[off]     = ns;
  page[off + 1] = type;
  page[off + 2] = span;
  page[off + 3] = chunkIdx;
  // CRC placeholder at off+4 (filled later)
  writeStr(page, off + 8, key, 16);
  return off;
}

/* ------------------------------------------------------------------ */
/*  Write a blob (NVS v2: blob_data + blob_index entries)              */
/* ------------------------------------------------------------------ */

function writeBlobEntries(page, startIdx, nsIdx, key, blobData) {
  const dataSpan = 1 + Math.ceil(blobData.length / ENTRY_SIZE);

  // --- Blob Data entry (TYPE_BLOB_DATA) ---
  const off = writeEntryHeader(page, startIdx, nsIdx, TYPE_BLOB_DATA, dataSpan, 0, key);

  // Data field: dataSize(u16) + reserved(u16, 0xFF) + dataCRC(u32)
  writeU16(page, off + 24, blobData.length);
  // bytes 26-27 stay 0xFF from page.fill()
  writeU32(page, off + 28, crc32(blobData, 0xFFFFFFFF));

  // Raw blob bytes in subsequent entry slots
  const rawOff = off + ENTRY_SIZE;
  page.set(blobData, rawOff);

  // Compute and write entry CRC (covers header only, not extra spans)
  writeU32(page, off + 4, entryItemCrc(page, off));

  // Mark bitmap: dataSpan entries starting at startIdx
  for (let i = 0; i < dataSpan; i++) {
    setEntryState(page.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), startIdx + i, ES_WRITTEN);
  }

  // --- Blob Index entry (TYPE_BLOB_IDX) ---
  const idxEntryIdx = startIdx + dataSpan;
  const idxOff = writeEntryHeader(page, idxEntryIdx, nsIdx, TYPE_BLOB_IDX, 1, 0xFF, key);

  // Data field: dataSize(u32) + chunkCount(u8) + chunkStart(u8) + reserved(u16=0xFFFF)
  writeU32(page, idxOff + 24, blobData.length);
  page[idxOff + 28] = 1;   // chunkCount
  page[idxOff + 29] = 0;   // chunkStart
  // bytes 30-31 stay 0xFF from page.fill()

  writeU32(page, idxOff + 4, entryItemCrc(page, idxOff));
  setEntryState(page.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), idxEntryIdx, ES_WRITTEN);

  return idxEntryIdx + 1;  // next free entry index
}

/* ------------------------------------------------------------------ */
/*  Write a U8 primitive entry                                         */
/* ------------------------------------------------------------------ */

function writeU8Entry(page, entryIdx, nsIdx, key, value) {
  const off = writeEntryHeader(page, entryIdx, nsIdx, TYPE_U8, 1, 0xFF, key);
  page[off + 24] = value & 0xFF;
  // bytes 25-31 stay 0xFF from page.fill()
  writeU32(page, off + 4, entryItemCrc(page, off));
  setEntryState(page.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), entryIdx, ES_WRITTEN);
  return entryIdx + 1;
}

/* ------------------------------------------------------------------ */
/*  Write a NVS string entry (TYPE_STR = 0x21)                         */
/*  Stored as multi-span entry: header + ceil((len+1)/32) data spans.  */
/*  No blob-index entry (strings use the legacy single-entry format).  */
/* ------------------------------------------------------------------ */

function writeStringEntry(page, entryIdx, nsIdx, key, str) {
  const enc = new TextEncoder();
  const strBytes = enc.encode(str);
  const dataLen = strBytes.length + 1;  // includes null terminator
  const dataSpan = 1 + Math.ceil(dataLen / ENTRY_SIZE);

  const off = writeEntryHeader(page, entryIdx, nsIdx, TYPE_STR, dataSpan, 0xFF, key);

  writeU16(page, off + 24, dataLen);
  // bytes 26-27 stay 0xFF from page.fill()

  // raw string bytes (with null terminator) in subsequent entry slots
  const rawOff = off + ENTRY_SIZE;
  page.set(strBytes, rawOff);
  page[rawOff + strBytes.length] = 0;  // explicit null terminator

  // CRC over the string data (including null)
  const dataBuf = page.subarray(rawOff, rawOff + dataLen);
  writeU32(page, off + 28, crc32(dataBuf, 0xFFFFFFFF));

  writeU32(page, off + 4, entryItemCrc(page, off));

  for (let i = 0; i < dataSpan; i++) {
    setEntryState(page.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), entryIdx + i, ES_WRITTEN);
  }

  return entryIdx + dataSpan;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * Generate a 4 KB NVS partition image.
 *
 * @param {Uint8Array} devEui     - 8-byte DevEUI
 * @param {Uint8Array} appKey     - 16-byte AppKey / network key
 * @param {number|null} deviceId  - Detector device ID (0-255), null to omit
 * @returns {Uint8Array} 4096-byte NVS page image
 */
export function generateNvsImage(devEui, appKey, deviceId = null) {
  if (devEui.length !== 8)  throw new Error("dev_eui must be 8 bytes");
  if (appKey.length !== 16) throw new Error("app_key must be 16 bytes");

  const page = new Uint8Array(NVS_PAGE_SIZE);
  page.fill(0xFF);

  // --- Page header ---
  writeU32(page, 0, PAGE_STATE_ACTIVE);
  writeU32(page, 4, 0);          // sequence number
  page[8] = NVS_VERSION;
  // bytes 9-27: 0xFF (already filled)
  // Header CRC covers bytes 4-27 (excludes page state at 0-3)
  writeU32(page, 28, crc32(page.subarray(4, 28), 0xFFFFFFFF));

  // --- Entry bitmap: start all empty (0xFF already) ---

  // --- Entry 0: Namespace "lora_cfg" ---
  const nsOff = writeEntryHeader(page, 0, 0, TYPE_U8, 1, 0xFF, "lora_cfg");
  page[nsOff + 24] = 1;  // namespace index = 1
  // bytes 25-31 stay 0xFF from page.fill()
  writeU32(page, nsOff + 4, entryItemCrc(page, nsOff));
  setEntryState(page.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), 0, ES_WRITTEN);

  // --- Entries 1+: dev_eui blob ---
  let nextIdx = writeBlobEntries(page, 1, 1, "dev_eui", devEui);

  // --- Entries N+: app_key blob ---
  nextIdx = writeBlobEntries(page, nextIdx, 1, "app_key", appKey);

  // --- Optional: device_id (U8) ---
  if (deviceId !== null && deviceId !== undefined) {
    writeU8Entry(page, nextIdx, 1, "device_id", deviceId);
  }

  return page;
}

/**
 * Parse a hex string into a Uint8Array.
 * Accepts with or without separators (colons, spaces, dashes).
 *
 * @param {string} hex - e.g. "AABBCC" or "AA:BB:CC"
 * @returns {Uint8Array}
 */
export function hexToBytes(hex) {
  const clean = hex.replace(/[:\s-]/g, "");
  if (clean.length % 2 !== 0) throw new Error("Hex string must have even length");
  if (!/^[0-9a-fA-F]+$/.test(clean)) throw new Error("Invalid hex characters");
  const bytes = new Uint8Array(clean.length / 2);
  for (let i = 0; i < bytes.length; i++) {
    bytes[i] = parseInt(clean.substring(i * 2, i * 2 + 2), 16);
  }
  return bytes;
}

/**
 * Generate a cryptographically random hex key string.
 *
 * @param {number} bytes - number of bytes (default 16 for AES-128)
 * @returns {string} uppercase hex string
 */
export function generateRandomKey(bytes = 16) {
  const arr = new Uint8Array(bytes);
  crypto.getRandomValues(arr);
  return Array.from(arr, b => b.toString(16).toUpperCase().padStart(2, "0")).join("");
}

/**
 * Generate a multi-page NVS image with lora_cfg and wired_cfg.
 *
 * Page 0: "lora_cfg"  — dev_eui, app_key, device_id (u8)
 * Page 1: "wired_cfg" — mqtt_url, mqtt_user, mqtt_pass, device_id (string),
 *                        eth_ip, eth_gw, eth_mask, eth_dns (all strings,
 *                        only non-empty values written)
 *
 * @param {Uint8Array}    devEui    - 8-byte DevEUI
 * @param {Uint8Array}    appKey    - 16-byte AppKey / network key
 * @param {number|null}   deviceId  - Detector device ID (0-255), null to omit
 * @param {Object}        wiredCfg  - Wired detector config strings
 * @param {string}        [wiredCfg.mqttUrl]
 * @param {string}        [wiredCfg.mqttUser]
 * @param {string}        [wiredCfg.mqttPass]
 * @param {string}        [wiredCfg.deviceId]
 * @param {string}        [wiredCfg.ethIp]
 * @param {string}        [wiredCfg.ethGw]
 * @param {string}        [wiredCfg.ethMask]
 * @param {string}        [wiredCfg.ethDns]
 * @param {string}        [wiredCfg.httpToken]
 * @returns {Uint8Array} 8192-byte NVS image (2 pages)
 */
export function generateWiredNvsImage(devEui, appKey, deviceId, wiredCfg) {
  if (devEui.length !== 8)  throw new Error("dev_eui must be 8 bytes");
  if (appKey.length !== 16) throw new Error("app_key must be 16 bytes");

  const image = new Uint8Array(NVS_PAGE_SIZE * 2);
  image.fill(0xFF);

  // ---- Page 0: lora_cfg ----
  const p0 = image.subarray(0, NVS_PAGE_SIZE);

  writeU32(p0, 0, PAGE_STATE_ACTIVE);
  writeU32(p0, 4, 0);
  p0[8] = NVS_VERSION;
  writeU32(p0, 28, crc32(p0.subarray(4, 28), 0xFFFFFFFF));

  const ns0Off = writeEntryHeader(p0, 0, 0, TYPE_U8, 1, 0xFF, "lora_cfg");
  p0[ns0Off + 24] = 1;
  writeU32(p0, ns0Off + 4, entryItemCrc(p0, ns0Off));
  setEntryState(p0.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), 0, ES_WRITTEN);

  let idx = writeBlobEntries(p0, 1, 1, "dev_eui", devEui);
  idx = writeBlobEntries(p0, idx, 1, "app_key", appKey);
  if (deviceId !== null && deviceId !== undefined) {
    writeU8Entry(p0, idx, 1, "device_id", deviceId);
  }

  // ---- Page 1: wired_cfg ----
  const p1 = image.subarray(NVS_PAGE_SIZE, NVS_PAGE_SIZE * 2);

  writeU32(p1, 0, PAGE_STATE_ACTIVE);
  writeU32(p1, 4, 1);
  p1[8] = NVS_VERSION;
  writeU32(p1, 28, crc32(p1.subarray(4, 28), 0xFFFFFFFF));

  const ns1Off = writeEntryHeader(p1, 0, 0, TYPE_U8, 1, 0xFF, "wired_cfg");
  p1[ns1Off + 24] = 2;
  writeU32(p1, ns1Off + 4, entryItemCrc(p1, ns1Off));
  setEntryState(p1.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), 0, ES_WRITTEN);

  idx = 1;
  if (wiredCfg.mqttUrl)  idx = writeStringEntry(p1, idx, 2, "mqtt_url",  wiredCfg.mqttUrl);
  if (wiredCfg.mqttUser) idx = writeStringEntry(p1, idx, 2, "mqtt_user", wiredCfg.mqttUser);
  if (wiredCfg.mqttPass) idx = writeStringEntry(p1, idx, 2, "mqtt_pass", wiredCfg.mqttPass);
  if (wiredCfg.deviceId) idx = writeStringEntry(p1, idx, 2, "device_id", wiredCfg.deviceId);
  if (wiredCfg.ethIp)    idx = writeStringEntry(p1, idx, 2, "eth_ip",    wiredCfg.ethIp);
  if (wiredCfg.ethGw)    idx = writeStringEntry(p1, idx, 2, "eth_gw",    wiredCfg.ethGw);
  if (wiredCfg.ethMask)  idx = writeStringEntry(p1, idx, 2, "eth_mask",  wiredCfg.ethMask);
  if (wiredCfg.ethDns)    idx = writeStringEntry(p1, idx, 2, "eth_dns",    wiredCfg.ethDns);
  if (wiredCfg.httpToken) idx = writeStringEntry(p1, idx, 2, "http_token", wiredCfg.httpToken);

  return image;
}

/**
 * Generate a multi-page NVS image with both lora_cfg and gateway_cfg.
 *
 * Page 0: "lora_cfg"    — dev_eui, app_key (same as generateNvsImage)
 * Page 1: "gateway_cfg" — wifi_ssid, wifi_pass, mqtt_url, mqtt_user,
 *                          mqtt_pass, device_id (all strings)
 *
 * @param {Uint8Array}  devEui   - 8-byte DevEUI
 * @param {Uint8Array}  appKey   - 16-byte AppKey / network key
 * @param {Object}      gwCfg    - Gateway config strings
 * @param {string}      gwCfg.wifiSsid
 * @param {string}      gwCfg.wifiPass
 * @param {string}      gwCfg.mqttUrl
 * @param {string}      gwCfg.mqttUser
 * @param {string}      gwCfg.mqttPass
 * @param {string}      gwCfg.deviceId
 * @returns {Uint8Array} 8192-byte NVS image (2 pages)
 */
export function generateGatewayNvsImage(devEui, appKey, gwCfg) {
  if (devEui.length !== 8)  throw new Error("dev_eui must be 8 bytes");
  if (appKey.length !== 16) throw new Error("app_key must be 16 bytes");

  const image = new Uint8Array(NVS_PAGE_SIZE * 2);
  image.fill(0xFF);

  // ---- Page 0: lora_cfg ----
  const p0 = image.subarray(0, NVS_PAGE_SIZE);

  writeU32(p0, 0, PAGE_STATE_ACTIVE);
  writeU32(p0, 4, 0);
  p0[8] = NVS_VERSION;
  writeU32(p0, 28, crc32(p0.subarray(4, 28), 0xFFFFFFFF));

  const ns0Off = writeEntryHeader(p0, 0, 0, TYPE_U8, 1, 0xFF, "lora_cfg");
  p0[ns0Off + 24] = 1;
  writeU32(p0, ns0Off + 4, entryItemCrc(p0, ns0Off));
  setEntryState(p0.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), 0, ES_WRITTEN);

  let idx = writeBlobEntries(p0, 1, 1, "dev_eui", devEui);
  writeBlobEntries(p0, idx, 1, "app_key", appKey);

  // ---- Page 1: gateway_cfg ----
  const p1 = image.subarray(NVS_PAGE_SIZE, NVS_PAGE_SIZE * 2);

  writeU32(p1, 0, PAGE_STATE_ACTIVE);
  writeU32(p1, 4, 1);  // sequence number = 1
  p1[8] = NVS_VERSION;
  writeU32(p1, 28, crc32(p1.subarray(4, 28), 0xFFFFFFFF));

  const ns1Off = writeEntryHeader(p1, 0, 0, TYPE_U8, 1, 0xFF, "gateway_cfg");
  p1[ns1Off + 24] = 2;  // namespace index = 2
  writeU32(p1, ns1Off + 4, entryItemCrc(p1, ns1Off));
  setEntryState(p1.subarray(HEADER_SIZE, HEADER_SIZE + BITMAP_SIZE), 0, ES_WRITTEN);

  idx = 1;
  if (gwCfg.wifiSsid) idx = writeStringEntry(p1, idx, 2, "wifi_ssid", gwCfg.wifiSsid);
  if (gwCfg.wifiPass) idx = writeStringEntry(p1, idx, 2, "wifi_pass", gwCfg.wifiPass);
  if (gwCfg.mqttUrl)  idx = writeStringEntry(p1, idx, 2, "mqtt_url",  gwCfg.mqttUrl);
  if (gwCfg.mqttUser) idx = writeStringEntry(p1, idx, 2, "mqtt_user", gwCfg.mqttUser);
  if (gwCfg.mqttPass) idx = writeStringEntry(p1, idx, 2, "mqtt_pass", gwCfg.mqttPass);
  if (gwCfg.deviceId) idx = writeStringEntry(p1, idx, 2, "device_id", gwCfg.deviceId);

  return image;
}
