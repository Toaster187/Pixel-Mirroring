// Minimal ZIP writer, used only to build fixtures for the tests.
//
// The point of the reader in public/js/zip.js is that it never touches anything
// but the one entry it was asked for. Proving that needs an archive with several
// entries, one of them deliberately unreadable, and it must not be somebody's
// real Instagram export - so the tests build their own.

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let value = i;
    for (let bit = 0; bit < 8; bit++) {
      value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    }
    table[i] = value >>> 0;
  }
  return table;
})();

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

async function deflateRaw(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

function concat(chunks) {
  const total = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const out = new Uint8Array(total);
  let cursor = 0;
  for (const chunk of chunks) {
    out.set(chunk, cursor);
    cursor += chunk.length;
  }
  return out;
}

function header(fields) {
  const bytes = new Uint8Array(fields.reduce((sum, [size]) => sum + size, 0));
  const dv = new DataView(bytes.buffer);
  let offset = 0;
  for (const [size, value] of fields) {
    if (size === 2) dv.setUint16(offset, value, true);
    if (size === 4) dv.setUint32(offset, value, true);
    offset += size;
  }
  return bytes;
}

/**
 * Builds a ZIP archive.
 * @param {Array<{name: string, content: string|Uint8Array, store?: boolean}>} files
 * @returns {Promise<Uint8Array>}
 */
export async function makeZip(files) {
  const encoder = new TextEncoder();
  const parts = [];
  const directory = [];
  let offset = 0;

  for (const file of files) {
    const name = encoder.encode(file.name);
    const raw = typeof file.content === 'string' ? encoder.encode(file.content) : file.content;
    const method = file.store ? 0 : 8;
    const data = method === 0 ? raw : await deflateRaw(raw);
    const crc = crc32(raw);

    const local = header([
      [4, 0x04034b50], [2, 20], [2, 0], [2, method], [2, 0], [2, 0],
      [4, crc], [4, data.length], [4, raw.length], [2, name.length], [2, 0],
    ]);
    parts.push(local, name, data);

    directory.push(concat([
      header([
        [4, 0x02014b50], [2, 20], [2, 20], [2, 0], [2, method], [2, 0], [2, 0],
        [4, crc], [4, data.length], [4, raw.length],
        [2, name.length], [2, 0], [2, 0], [2, 0], [2, 0], [4, 0], [4, offset],
      ]),
      name,
    ]));
    offset += local.length + name.length + data.length;
  }

  const central = concat(directory);
  const eocd = header([
    [4, 0x06054b50], [2, 0], [2, 0], [2, files.length], [2, files.length],
    [4, central.length], [4, offset], [2, 0],
  ]);
  return concat([...parts, central, eocd]);
}
