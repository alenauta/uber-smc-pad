/**
 * Client-side SMC-PAD firmware image assembler (runs 100% locally in the browser).
 *
 * Combines the user's own official SMC-PAD_005.fwsc container (stock SDK v1.0.0 bootloader,
 * BLE RCSP updater ota.bin, container seed table, and trailer) with the clean-room compiled
 * custom application payload (docs/payload/smcpad_925.bin).
 *
 * Nothing is uploaded anywhere; all hashing, CRC16 checks, and container framing happen in RAM.
 */
(function (root) {
  "use strict";

  const FWSC_BLK = 0x30;
  const FWSC_DATA = 0x2f;
  const FWSC_N = 20;
  const BUNDLE_MAGIC = "SMCP925\x01";

  function hexFromBytes(bytes) {
    let out = "";
    for (let i = 0; i < bytes.length; i++) {
      out += bytes[i].toString(16).padStart(2, "0");
    }
    return out;
  }

  function bytesFromHex(hex) {
    if (hex.length % 2 !== 0) {
      throw new Error("Invalid hex length");
    }
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; i++) {
      out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
    }
    return out;
  }

  async function sha256Hex(uint8) {
    const subtle =
      typeof globalThis !== "undefined" &&
      globalThis.crypto &&
      globalThis.crypto.subtle
        ? globalThis.crypto.subtle
        : null;
    if (!subtle) {
      throw new Error("WebCrypto SubtleCrypto SHA-256 is unavailable in this environment.");
    }
    const view =
      uint8.byteOffset === 0 && uint8.byteLength === uint8.buffer.byteLength
        ? uint8.buffer
        : uint8.slice().buffer;
    const digest = await subtle.digest("SHA-256", view);
    return hexFromBytes(new Uint8Array(digest));
  }

  function jlCrc16(buf, off = 0, len = buf.length - off, init = 0) {
    let crc = init & 0xffff;
    const end = off + len;
    for (let i = off; i < end; i++) {
      crc ^= buf[i] << 8;
      for (let b = 0; b < 8; b++) {
        if (crc & 0x8000) {
          crc = ((crc << 1) ^ 0x1021) & 0xffff;
        } else {
          crc = (crc << 1) & 0xffff;
        }
      }
    }
    return crc;
  }

  function jlLfsrXor(buf, off, size, seed = 0xffff) {
    let k = seed & 0xffff;
    for (let i = 0; i < size; i++) {
      buf[off + i] ^= k & 0xff;
      k = ((k << 1) ^ (k & 0x8000 ? 0x1021 : 0)) & 0xffff;
    }
    return k;
  }

  function jlSfcXor(buf, off, size, base, seed, blockSize = 32) {
    for (let i = 0; i < size; i += blockSize) {
      const chunk = Math.min(size - i, blockSize);
      const blkSeed = (seed ^ ((off + i - base) >>> 2)) & 0xffff;
      jlLfsrXor(buf, off + i, chunk, blkSeed);
    }
  }

  function readContainerSeed(table32) {
    if (table32.length < 32) {
      throw new Error("Container seed table is truncated.");
    }
    let sum = 0;
    for (let i = 0; i < 16; i++) {
      sum = (sum + table32[i]) & 0xff;
    }
    if (sum >= 0xe0) {
      sum = 0xaa;
    } else if (sum <= 0x10) {
      sum = 0x55;
    }
    let seed = 0;
    for (let i = 0; i < 16; i++) {
      if ((table32[16 + i] ^ table32[15 - i]) < sum) {
        seed |= 1 << i;
      }
    }
    return seed;
  }

  function encodeFwscTag(name) {
    if (name.length > FWSC_N) {
      throw new Error("FWSC tag name exceeds 20 characters.");
    }
    const tag = new Uint8Array(FWSC_N);
    tag.fill(0x40);
    for (let i = 0; i < name.length; i++) {
      tag[i] = (name.charCodeAt(i) + i + 1) & 0xff;
    }
    return tag;
  }

  function decodeFwscTag(tag) {
    let s = "";
    for (let i = 0; i < tag.length; i++) {
      s += String.fromCharCode((tag[i] - i - 1) & 0xff);
    }
    const m = s.match(/^(.+?_\d{3})/);
    return m ? m[1] : s;
  }

  function unwrapFwsc(fwscBytes) {
    if (fwscBytes.length < FWSC_N * FWSC_BLK + 0x400) {
      throw new Error("Input file is too small to be a valid .fwsc firmware container.");
    }
    const ufw = new Uint8Array(fwscBytes.length - FWSC_N);
    const tag = new Uint8Array(FWSC_N);
    let dst = 0;
    for (let i = 0; i < FWSC_N; i++) {
      const blkOff = i * FWSC_BLK;
      ufw.set(fwscBytes.subarray(blkOff, blkOff + FWSC_DATA), dst);
      dst += FWSC_DATA;
      tag[i] = fwscBytes[blkOff + FWSC_DATA];
    }
    ufw.set(fwscBytes.subarray(FWSC_N * FWSC_BLK), dst);
    return { ufw, tag, tagName: decodeFwscTag(tag) };
  }

  function wrapFwsc(ufwBytes, name) {
    if (ufwBytes.length < FWSC_N * FWSC_DATA) {
      throw new Error("UFW image too short to wrap.");
    }
    const tag = encodeFwscTag(name);
    const out = new Uint8Array(ufwBytes.length + FWSC_N);
    let dst = 0;
    for (let i = 0; i < FWSC_N; i++) {
      const srcOff = i * FWSC_DATA;
      out.set(ufwBytes.subarray(srcOff, srcOff + FWSC_DATA), dst);
      dst += FWSC_DATA;
      out[dst++] = tag[i];
    }
    out.set(ufwBytes.subarray(FWSC_N * FWSC_DATA), dst);
    return out;
  }

  function parseUfwSections(ufwBytes) {
    const hdr = ufwBytes.slice(0, 0x400);
    jlLfsrXor(hdr, 0, 0x40, 0xffff);
    const dv = new DataView(hdr.buffer, hdr.byteOffset, hdr.byteLength);
    const hdrCrc = dv.getUint16(0, true);
    const listCrc = dv.getUint16(2, true);
    const numEnts = dv.getUint16(8, true);

    if (hdrCrc !== jlCrc16(hdr, 2, 0x3e)) {
      throw new Error("Stock UFW outer header CRC check failed.");
    }
    if (listCrc !== jlCrc16(ufwBytes, 0x40, numEnts * 0x50)) {
      throw new Error("Stock UFW section table CRC check failed.");
    }

    const sections = {};
    for (let i = 0; i < numEnts; i++) {
      const off = 0x40 + i * 0x50;
      jlLfsrXor(hdr, off, 0x50, 0xffff);
      const eOff = dv.getUint32(off + 8, true);
      const eSz = dv.getUint32(off + 12, true);
      let name = "";
      for (let c = 0; c < 16; c++) {
        const ch = hdr[off + 64 + c];
        if (ch === 0) break;
        name += String.fromCharCode(ch);
      }
      sections[name] = ufwBytes.subarray(eOff, eOff + eSz);
    }
    return sections;
  }

  function parsePayloadBundle(bundleBytes) {
    if (bundleBytes.length < 16) {
      throw new Error("Custom payload bundle is truncated.");
    }
    let magic = "";
    for (let i = 0; i < 8; i++) {
      magic += String.fromCharCode(bundleBytes[i]);
    }
    if (magic !== BUNDLE_MAGIC) {
      throw new Error("Unsupported custom payload bundle header.");
    }
    const dv = new DataView(bundleBytes.buffer, bundleBytes.byteOffset, bundleBytes.byteLength);
    const manifestLen = dv.getUint32(8, true);
    const jsonStart = 12;
    const jsonEnd = jsonStart + manifestLen;
    if (jsonEnd > bundleBytes.length) {
      throw new Error("Custom payload manifest exceeds bundle size.");
    }
    const manifestText = new TextDecoder("utf-8").decode(
      bundleBytes.subarray(jsonStart, jsonEnd)
    );
    const manifest = JSON.parse(manifestText);
    const dataBase = jsonEnd;
    const chunks = {};
    for (const [name, info] of Object.entries(manifest.chunks)) {
      const start = dataBase + info.offset;
      const end = start + info.size;
      if (end > bundleBytes.length) {
        throw new Error(`Payload chunk ${name} is out of bounds.`);
      }
      chunks[name] = bundleBytes.subarray(start, end);
    }
    return { manifest, chunks };
  }

  async function patchFirmware(stockFwscBytes, bundleBytes) {
    const { manifest, chunks } = parsePayloadBundle(bundleBytes);

    if (stockFwscBytes.length !== manifest.stock_size) {
      throw new Error(
        `Invalid stock file size (${stockFwscBytes.length.toLocaleString()} bytes). ` +
          `Please select the unmodified official ${manifest.stock_name} (${manifest.stock_size.toLocaleString()} bytes).`
      );
    }

    const stockSha256 = await sha256Hex(stockFwscBytes);
    if (stockSha256 !== manifest.stock_sha256) {
      const unwrappedCheck = unwrapFwsc(stockFwscBytes);
      throw new Error(
        `SHA-256 mismatch for input file (tag "${unwrappedCheck.tagName}", SHA-256 ${stockSha256.slice(0, 16)}…). ` +
          `Only the official unmodified ${manifest.stock_name} (SHA-256 ${manifest.stock_sha256.slice(0, 16)}…) is supported.`
      );
    }

    const { ufw: stockUfw, tagName } = unwrapFwsc(stockFwscBytes);
    if (tagName !== "SMC-PAD_005") {
      throw new Error(`Unexpected firmware tag "${tagName}". Expected "SMC-PAD_005".`);
    }

    const stockSections = parseUfwSections(stockUfw);
    const stockFlash1 = stockSections["flash.bin"];
    const stockFlash2 = stockSections["flash2.bin"];
    const stockOta = stockSections["ota.bin"];
    const stockTail = stockSections["tail.bin"];
    if (!stockFlash1 || !stockFlash2 || !stockOta || !stockTail) {
      throw new Error("Stock firmware container is missing required sections.");
    }

    const otaSha256 = await sha256Hex(stockOta);
    if (otaSha256 !== manifest.expected_ota_sha256) {
      throw new Error("Stock RCSP updater (ota.bin) digest check failed.");
    }

    // 1. Read container seed dynamically from user's stock flash.bin[0xEEC..0xF0C]
    const seed = readContainerSeed(stockFlash1.subarray(0xeec, 0xf0c));

    // 2. Scramble custom application area (0x1000+) with container seed
    const appEnc = chunks["plain_app"].slice();
    jlSfcXor(appEnc, 0, appEnc.length, 0x1000, seed);

    // 3. Assemble flash2.bin (4 KB stock JLFS/bootloader header + scrambled custom app)
    const rebuiltFlash2 = new Uint8Array(manifest.flash2_size);
    rebuiltFlash2.set(stockFlash2.subarray(0, 0x1000), 0);
    rebuiltFlash2.set(appEnc, 0x1000);

    const flash2Sha256 = await sha256Hex(rebuiltFlash2);
    if (flash2Sha256 !== manifest.expected_flash2_sha256) {
      throw new Error(
        "Internal verification error: assembled flash2.bin SHA-256 did not match expected reference digest."
      );
    }

    // 4. Assemble flash.bin (preserving stock uboot.boot [0xA0..0xEEC] and stock seed table [0xEEC..0xF0E])
    const rebuiltFlash1 = new Uint8Array(manifest.flash1_size);
    rebuiltFlash1.fill(0xff);
    rebuiltFlash1.set(chunks["jlfs_top"], 0);
    rebuiltFlash1.set(stockFlash1.subarray(0xa0, 0xf0e), 0xa0);
    rebuiltFlash1.set(chunks["ini_tail"], 0xf0e);
    rebuiltFlash1.set(appEnc, 0x1000);

    // Update JLFS entry @0x40 (isd_config.ini, offset 0xEEC, length 0x63) data_crc & hdr_crc
    const ent40 = rebuiltFlash1.slice(0x40, 0x60);
    jlLfsrXor(ent40, 0, 32, 0xffff);
    const ent40Dv = new DataView(ent40.buffer, ent40.byteOffset, ent40.byteLength);
    const iniDataCrc = jlCrc16(rebuiltFlash1, 0xeec, 0x63);
    ent40Dv.setUint16(2, iniDataCrc, true);
    ent40Dv.setUint16(0, jlCrc16(ent40, 2, 30), true);
    jlLfsrXor(ent40, 0, 32, 0xffff);
    rebuiltFlash1.set(ent40, 0x40);

    // 5. Assemble outer UFW container & recompute CRCs
    const sectionMap = {
      "flash.bin": rebuiltFlash1,
      "info.log": new Uint8Array(0),
      "isd_config.ini": chunks["isd_config.ini"],
      "ota.bin": stockOta,
      "script.ver": chunks["script.ver"],
      "flash2.bin": rebuiltFlash2,
      "blimit.bin": chunks["blimit.bin"],
      "tail.bin": stockTail,
    };

    const rebuiltUfw = new Uint8Array(manifest.ufw_size);
    if (manifest.ufw_trailer && manifest.ufw_trailer.hex) {
      rebuiltUfw.set(bytesFromHex(manifest.ufw_trailer.hex), manifest.ufw_trailer.offset);
    }

    const hdrBuf = new Uint8Array(0x400);
    const recomputeSet = new Set(["flash.bin", "info.log", "ota.bin", "flash2.bin", "tail.bin"]);

    for (let i = 0; i < manifest.entries.length; i++) {
      const e = manifest.entries[i];
      const secBytes = sectionMap[e.name];
      if (!secBytes || secBytes.length !== e.size) {
        throw new Error(`Section ${e.name} size mismatch.`);
      }
      rebuiltUfw.set(secBytes, e.offset);
      const dcrc = recomputeSet.has(e.name) ? jlCrc16(secBytes) : e.dcrc;

      const eRaw = new Uint8Array(0x50);
      const edv = new DataView(eRaw.buffer);
      edv.setUint16(0, e.type, true);
      edv.setUint16(2, e.index, true);
      edv.setUint16(4, dcrc, true);
      edv.setUint16(6, e.wa1, true);
      edv.setUint32(8, e.offset, true);
      edv.setUint32(12, e.size, true);
      edv.setUint32(16, e.size2, true);
      eRaw.set(bytesFromHex(e.wa2_hex), 20);
      eRaw.set(bytesFromHex(e.raw_name_hex), 64);
      jlLfsrXor(eRaw, 0, 0x50, 0xffff);
      hdrBuf.set(eRaw, 0x40 + i * 0x50);
    }

    const listCrc = jlCrc16(hdrBuf, 0x40, manifest.entries.length * 0x50);
    const hdv = new DataView(hdrBuf.buffer);
    hdv.setUint16(0, 0, true);
    hdv.setUint16(2, listCrc, true);
    hdv.setUint32(4, manifest.ufw_header.imgsize, true);
    hdv.setUint16(8, manifest.entries.length, true);
    hdv.setUint16(10, manifest.ufw_header.wa3, true);
    hdv.setUint32(12, manifest.ufw_header.wa4, true);
    hdrBuf.set(bytesFromHex(manifest.ufw_header.chipname_hex), 16);
    hdv.setUint16(0, jlCrc16(hdrBuf, 2, 0x3e), true);
    jlLfsrXor(hdrBuf, 0, 0x40, 0xffff);
    rebuiltUfw.set(hdrBuf, 0);

    // 6. Wrap UFW into SincoOTA .fwsc format
    const outFwsc = wrapFwsc(rebuiltUfw, manifest.build_name);
    const outSha256 = await sha256Hex(outFwsc);

    return {
      fileName: `${manifest.build_name}.fwsc`,
      buildName: manifest.build_name,
      bytes: outFwsc,
      outputSha256: outSha256,
      stockSha256,
      flash2Sha256,
    };
  }

  const api = {
    patchFirmware,
    parsePayloadBundle,
    unwrapFwsc,
    wrapFwsc,
    sha256Hex,
    jlCrc16,
  };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
  }
  if (root) {
    root.SmcPadPatcher = api;
  }
})(typeof globalThis !== "undefined" ? globalThis : this);
