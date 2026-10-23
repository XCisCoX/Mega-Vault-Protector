/* Legacy .mvpk v1 codec. Cryptography is exclusively vendored libsodium.
 * Load /vendor/sodium.js, /vendor/sodium-wrappers.js, then this script.
 * No network, persistence, password normalization, or unauthenticated output.
 */
(function (root) {
  'use strict';
  const MAX_FILE_BYTES = 128 * 1024 * 1024;
  const CHUNK = 1024 * 1024;
  const MAGIC = new Uint8Array([77, 86, 80, 75, 167, 1, 195, 0]);
  const encoder = new TextEncoder();
  const fail = message => { throw new Error(message); };
  const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  function join(parts) {
    const result = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let offset = 0;
    for (const part of parts) { result.set(part, offset); offset += part.length; }
    return result;
  }
  function pathBytes(name) {
    if (typeof name !== 'string') fail('Invalid package path.');
    const bytes = encoder.encode(name);
    if (!bytes.length || bytes.length > 1024 || name.split('/').some(part =>
      !part || part === '.' || part === '..' || encoder.encode(part).length > 120 ||
      part.startsWith(' ') || /[ .]$/.test(part) || /[\x00-\x1f\x7f\\:*?"<>|]/.test(part)))
      fail('Unsafe package path.');
    return bytes;
  }
  function passwordBytes(password) {
    if (typeof password !== 'string') fail('Password must be a string.');
    const bytes = encoder.encode(password);
    if (!bytes.length || bytes.length > 4096) { bytes.fill(0); fail('Password must contain 1–4096 UTF-8 bytes.'); }
    return bytes;
  }
  async function sodium() {
    if (!root.sodium || !root.sodium.ready) fail('Self-hosted libsodium is not loaded.');
    await root.sodium.ready;
    return root.sodium;
  }
  function derive(s, password, salt) {
    return s.crypto_pwhash(32, password, salt, 3, 65536 * 1024, s.crypto_pwhash_ALG_ARGON2ID13);
  }
  function release(s, state) {
    // Upstream wrappers allocate a 52-byte secretstream state without a free API.
    const module = s.libsodium;
    if (state && module) { module.HEAPU8.fill(0, state, state + 52); module._free(state); }
  }
  async function encrypt(file, password, onProgress) {
    const size = file && file.size;
    if (!file || !Number.isSafeInteger(size) || size < 0 || size > MAX_FILE_BYTES)
      fail('File exceeds the 128 MiB browser limit or is not a Blob.');
    if (!(file instanceof Blob)) fail('Input must be a File or Blob.');
    const name = pathBytes(file.name || 'file.bin');
    const pass = passwordBytes(password);
    const progress = n => { if (typeof onProgress === 'function') onProgress(n); };
    let s, key, state;
    try {
      progress(0);
      s = await sodium();
      const salt = s.randombytes_buf(16);
      key = derive(s, pass, salt);
      const stream = s.crypto_secretstream_xchacha20poly1305_init_push(key);
      state = stream.state;
      const header = new Uint8Array(64);
      header.set(MAGIC); header[8] = 1;
      const dv = view(header);
      dv.setUint32(12, 65536, true); dv.setUint32(16, 3, true); dv.setUint32(20, 1, true);
      header.set(salt, 24); header.set(stream.header, 40);
      const parts = [header];
      function push(plain, tag = s.crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) {
        try {
          const cipher = s.crypto_secretstream_xchacha20poly1305_push(state, plain, null, tag);
          const length = new Uint8Array(4); view(length).setUint32(0, cipher.length, true);
          parts.push(length, cipher);
        } finally { plain.fill(0); }
      }
      const metadata = new Uint8Array(3 + name.length + 8);
      metadata[0] = 1; view(metadata).setUint16(1, name.length, true); metadata.set(name, 3);
      view(metadata).setBigUint64(3 + name.length, BigInt(size), true);
      push(metadata);
      for (let offset = 0; offset < size; offset += CHUNK) {
        const expected = Math.min(CHUNK, size - offset);
        const bytes = new Uint8Array(await file.slice(offset, offset + expected).arrayBuffer());
        if (bytes.length !== expected) { bytes.fill(0); fail('File changed or ended while reading.'); }
        const record = new Uint8Array(6 + bytes.length);
        record[0] = 2; view(record).setUint32(2, bytes.length, true); record.set(bytes, 6); bytes.fill(0);
        push(record);
        progress((offset + expected) / (size + 1));
      }
      push(new Uint8Array([4]), s.crypto_secretstream_xchacha20poly1305_TAG_FINAL);
      const result = join(parts);
      progress(1);
      return result;
    } finally { pass.fill(0); if (key) key.fill(0); if (s) release(s, state); }
  }
  const MAX_FILES = 256;
  const MAX_DIRECTORIES = 256;
  const MAX_RECORDS = 4096;
  const MAX_PACKAGE_BYTES = MAX_FILE_BYTES + 2 * CHUNK;
  // zlib compressBound(1 MiB) + native allowance + secretstream ABYTES.
  const MAX_CIPHER_BYTES = CHUNK + (CHUNK >>> 12) + (CHUNK >>> 14) + (CHUNK >>> 25) + 13 + 64 + 17;
  async function inflate(stored, expected) {
    if (typeof DecompressionStream !== 'function') fail('This browser does not support zlib DecompressionStream.');
    const output = new Uint8Array(expected);
    // Do not hand a whole compressed frame (or Blob's 64 KiB chunks) to the
    // transform: an adversarial DEFLATE block could expand before backpressure
    // reaches the output reader. Small input pulls bound transient expansion.
    let offset = 0;
    const compressed = new ReadableStream({
      pull(controller) {
        if (offset === stored.length) { controller.close(); return; }
        const end = Math.min(offset + 256, stored.length);
        controller.enqueue(stored.subarray(offset, end)); offset = end;
      }
    });
    const reader = compressed.pipeThrough(new DecompressionStream('deflate')).getReader();
    let used = 0;
    try {
      for (;;) {
        const {done, value} = await reader.read();
        if (done) break;
        if (value.length > expected - used) fail('Decompressed chunk exceeds its declared size.');
        output.set(value, used); used += value.length;
      }
      if (used !== expected) fail('Decompressed chunk has the wrong size.');
      return output;
    } catch (error) {
      output.fill(0);
      try { await reader.cancel(); } catch (_) { /* preserve the original error */ }
      throw new Error('Invalid compressed package chunk: ' + error.message);
    } finally { reader.releaseLock(); }
  }
  async function decrypt(input, password, onProgress) {
    let source;
    if (input instanceof Uint8Array) source = input;
    else if (input instanceof ArrayBuffer) source = new Uint8Array(input);
    else fail('Package must be a Uint8Array or ArrayBuffer.');
    if (source.length < 64 || source.length > MAX_PACKAGE_BYTES) fail('Package exceeds browser size limits or is incomplete.');
    // Own the input while awaiting decompression: caller mutation cannot bypass validation.
    const bytes = new Uint8Array(source);
    const dv = view(bytes);
    if (!MAGIC.every((v, i) => bytes[i] === v)) fail('Not an .mvpk package.');
    if (bytes[8] !== 1 || bytes[9] > 1 || bytes[10] || bytes[11]) fail('Unsupported package version or flags.');
    // Reject every unsupported dimension before awaiting sodium or running the KDF.
    if (dv.getUint32(12, true) !== 65536 || dv.getUint32(16, true) !== 3 || dv.getUint32(20, true) !== 1)
      fail('Unsupported package KDF: requires Argon2id 65536 KiB, 3 iterations, parallelism 1.');
    const pass = passwordBytes(password);
    const progress = n => { if (typeof onProgress === 'function') onProgress(n); };
    const files = [], paths = new Map();
    let s, key, state, active = null, total = 0, dirs = 0, records = 0, offset = 64, success = false;
    try {
      progress(0);
      s = await sodium(); key = derive(s, pass, bytes.subarray(24, 40));
      state = s.crypto_secretstream_xchacha20poly1305_init_pull(bytes.subarray(40, 64), key);
      if (!state) fail('Invalid package stream header.');
      for (;;) {
        if (++records > MAX_RECORDS) fail('Package record count exceeds browser limits.');
        if (bytes.length - offset < 4) fail('Incomplete package frame.');
        const size = dv.getUint32(offset, true); offset += 4;
        if (size < 18 || size > MAX_CIPHER_BYTES) fail('Invalid package frame length.');
        if (size > bytes.length - offset) fail('Truncated package frame.');
        const result = s.crypto_secretstream_xchacha20poly1305_pull(state, bytes.subarray(offset, offset + size), null);
        if (!result) fail('Package authentication failed: wrong password or damaged package.');
        offset += size;
        const plain = result.message;
        try {
          const kind = plain[0], tag = result.tag;
          if (tag !== s.crypto_secretstream_xchacha20poly1305_TAG_MESSAGE && tag !== s.crypto_secretstream_xchacha20poly1305_TAG_FINAL)
            fail('Unsupported package stream tag.');
          if (kind !== 4 && tag === s.crypto_secretstream_xchacha20poly1305_TAG_FINAL) fail('Premature final package tag.');
          if (kind === 1 || kind === 3 || kind === 4) {
            if (active && active.used !== active.bytes.length) fail('Package file size is incomplete.');
            active = null;
          }
          if (kind === 1 || kind === 3) {
            const tail = kind === 1 ? 8 : 0;
            if (plain.length < 3 + tail) fail('Invalid package path record.');
            const pd = view(plain), length = pd.getUint16(1, true);
            if (plain.length !== 3 + length + tail) fail('Invalid package path length.');
            const name = new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}).decode(plain.subarray(3, 3 + length));
            pathBytes(name);
            const canonical = name.toLowerCase();
            if (paths.has(canonical)) fail('Duplicate package path.');
            const segments = canonical.split('/');
            for (let i = 1; i < segments.length; i++) {
              if (paths.get(segments.slice(0, i).join('/')) === 1) fail('Package file conflicts with directory path.');
            }
            if (kind === 1 && [...paths.keys()].some(p => p.startsWith(canonical + '/')))
              fail('Package file conflicts with directory path.');
            paths.set(canonical, kind);
            if (kind === 3) {
              if (++dirs > MAX_DIRECTORIES) fail('Package directory count exceeds browser limits.');
            } else {
              const declared = pd.getBigUint64(3 + length, true);
              if (declared > BigInt(MAX_FILE_BYTES) || declared > BigInt(MAX_FILE_BYTES - total))
                fail('Package plaintext exceeds the 128 MiB browser limit.');
              if (files.length >= MAX_FILES) fail('Package file count exceeds browser limits.');
              const size = Number(declared); total += size;
              active = {name, bytes: new Uint8Array(size), used: 0}; files.push(active);
            }
          } else if (kind === 2) {
            if (!active || plain.length < 6 || plain[1] > 1) fail('Invalid package chunk.');
            const full = view(plain).getUint32(2, true);
            if (!full || full > CHUNK || full > active.bytes.length - active.used) fail('Package chunk exceeds file size or chunk limit.');
            const stored = plain.subarray(6);
            if (!plain[1] && stored.length !== full) fail('Package chunk has the wrong size.');
            const raw = plain[1] ? await inflate(stored, full) : stored;
            try { active.bytes.set(raw, active.used); active.used += full; }
            finally { if (plain[1]) raw.fill(0); }
          } else if (kind === 4) {
            if (plain.length !== 1 || tag !== s.crypto_secretstream_xchacha20poly1305_TAG_FINAL || offset !== bytes.length)
              fail('Package final record is invalid or contains trailing data.');
            progress(1);
            success = true;
            return files.map(({name, bytes}) => ({name, bytes}));
          } else fail('Unknown package record type.');
        } finally { plain.fill(0); }
        progress(offset / (bytes.length + 1));
      }
    } finally {
      pass.fill(0); if (key) key.fill(0); if (s) release(s, state);
      if (!success) for (const file of files) file.bytes.fill(0);
    }
  }
  root.MvpPackage = Object.freeze({encrypt, decrypt, MAX_FILE_BYTES, MAX_PACKAGE_BYTES, MAX_FILES, MAX_DIRECTORIES, MAX_RECORDS});
})(typeof window !== 'undefined' ? window : globalThis);
