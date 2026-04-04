/**
 * nvs_gen.js — Browser-side ESP32 NVS partition image generator
 *
 * Generates a 4 KB (one page) NVS binary image containing:
 *   namespace "lora_cfg"
 *     blob "dev_eui"   (8 bytes)
 *     blob "app_key"   (16 bytes)
 *     u8   "device_id" (1 byte, detector only)
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
