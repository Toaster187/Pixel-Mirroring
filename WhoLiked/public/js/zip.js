// Minimal ZIP reader for the Instagram data export.
//
// Why not JSZip: a ready-made library pulls the whole archive into memory before
// anyone gets to say which entry is wanted. The export contains private messages,
// photos and videos - those must never arrive here in the first place. This
// reader loads the central directory (names and sizes only) and then inflates
// exactly one named entry. Nothing else is ever read, because Blob.slice() never
// touches the rest of the file.
//
// No third-party code and no dependency: inflating goes through
// DecompressionStream, which every current browser and Node 18+ ships.

const SIG_EOCD = 0x06054b50;        // End of Central Directory
const SIG_EOCD64 = 0x06064b50;      // ZIP64 End of Central Directory
const SIG_EOCD64_LOCATOR = 0x07064b50;
const SIG_CENTRAL = 0x02014b50;     // Central Directory File Header
const SIG_LOCAL = 0x04034b50;       // Local File Header

const EOCD_MIN_SIZE = 22;
const EOCD_MAX_COMMENT = 0xffff;
const ZIP64_MARKER_32 = 0xffffffff;
const ZIP64_MARKER_16 = 0xffff;

const METHOD_STORE = 0;
const METHOD_DEFLATE = 8;

export class ZipError extends Error {}

async function sliceBytes(blob, start, end) {
  const buffer = await blob.slice(start, Math.min(end, blob.size)).arrayBuffer();
  return new Uint8Array(buffer);
}

function view(bytes) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

// ZIP stores 64-bit values as two 32-bit halves. Number covers that up to 2^53 -
// beyond it the archive would be nine petabytes.
function readU64(dv, offset) {
  return dv.getUint32(offset + 4, true) * 0x100000000 + dv.getUint32(offset, true);
}

// Scan for the EOCD from the back. The trailing comment may be up to 64 KiB, so
// that window is searched backwards for the signature.
async function findEndOfCentralDirectory(blob) {
  const windowSize = Math.min(blob.size, EOCD_MIN_SIZE + EOCD_MAX_COMMENT);
  const tail = await sliceBytes(blob, blob.size - windowSize, blob.size);
  const dv = view(tail);
  for (let i = tail.length - EOCD_MIN_SIZE; i >= 0; i--) {
    if (dv.getUint32(i, true) !== SIG_EOCD) continue;
    const commentLength = dv.getUint16(i + 20, true);
    // A random byte pattern inside the comment looks exactly like the signature.
    // Only when the comment length reaches precisely the end of the file is this
    // really the EOCD.
    if (i + EOCD_MIN_SIZE + commentLength !== tail.length) continue;
    return {
      absoluteOffset: blob.size - windowSize + i,
      entryCount: dv.getUint16(i + 10, true),
      directorySize: dv.getUint32(i + 12, true),
      directoryOffset: dv.getUint32(i + 16, true),
    };
  }
  throw new ZipError('Keine ZIP-Datei: Das Archiv-Ende wurde nicht gefunden.');
}

// Large exports (>4 GiB or >65535 files) carry the real values only in the ZIP64
// record; the classic EOCD then holds nothing but 0xffff... markers.
async function resolveZip64(blob, eocd) {
  const needsZip64 =
    eocd.entryCount === ZIP64_MARKER_16 ||
    eocd.directorySize === ZIP64_MARKER_32 ||
    eocd.directoryOffset === ZIP64_MARKER_32;
  if (!needsZip64) return eocd;

  const locatorOffset = eocd.absoluteOffset - 20;
  if (locatorOffset < 0) throw new ZipError('ZIP64-Archiv ohne Locator.');
  const locator = view(await sliceBytes(blob, locatorOffset, locatorOffset + 20));
  if (locator.getUint32(0, true) !== SIG_EOCD64_LOCATOR) {
    throw new ZipError('ZIP64-Locator fehlt.');
  }
  const recordOffset = readU64(locator, 8);
  const record = view(await sliceBytes(blob, recordOffset, recordOffset + 56));
  if (record.getUint32(0, true) !== SIG_EOCD64) {
    throw new ZipError('ZIP64-Endsatz fehlt.');
  }
  return {
    ...eocd,
    entryCount: readU64(record, 32),
    directorySize: readU64(record, 40),
    directoryOffset: readU64(record, 48),
  };
}

// The extra field carries 64-bit replacements for exactly those header fields
// marked 0xffffffff, in a fixed order and with no placeholders for the ones that
// were not marked.
function readZip64Extra(extra, entry) {
  const dv = view(extra);
  let offset = 0;
  while (offset + 4 <= extra.length) {
    const headerId = dv.getUint16(offset, true);
    const size = dv.getUint16(offset + 2, true);
    if (headerId === 0x0001) {
      const end = offset + 4 + size;
      let field = offset + 4;
      if (entry.uncompressedSize === ZIP64_MARKER_32 && field + 8 <= end) {
        entry.uncompressedSize = readU64(dv, field);
        field += 8;
      }
      if (entry.compressedSize === ZIP64_MARKER_32 && field + 8 <= end) {
        entry.compressedSize = readU64(dv, field);
        field += 8;
      }
      if (entry.localHeaderOffset === ZIP64_MARKER_32 && field + 8 <= end) {
        entry.localHeaderOffset = readU64(dv, field);
      }
      return;
    }
    offset += 4 + size;
  }
}

/**
 * Reads the archive's table of contents only: names, sizes, positions.
 * Not a single byte of payload is inflated.
 */
export async function readZipIndex(blob) {
  const eocd = await resolveZip64(blob, await findEndOfCentralDirectory(blob));
  const directory = await sliceBytes(
    blob,
    eocd.directoryOffset,
    eocd.directoryOffset + eocd.directorySize,
  );
  const dv = view(directory);
  const utf8 = new TextDecoder('utf-8');
  const entries = [];

  let offset = 0;
  while (offset + 46 <= directory.length) {
    if (dv.getUint32(offset, true) !== SIG_CENTRAL) break;
    const nameLength = dv.getUint16(offset + 28, true);
    const extraLength = dv.getUint16(offset + 30, true);
    const commentLength = dv.getUint16(offset + 32, true);
    const entry = {
      name: utf8.decode(directory.subarray(offset + 46, offset + 46 + nameLength)),
      method: dv.getUint16(offset + 10, true),
      compressedSize: dv.getUint32(offset + 20, true),
      uncompressedSize: dv.getUint32(offset + 24, true),
      localHeaderOffset: dv.getUint32(offset + 42, true),
    };
    const extra = directory.subarray(
      offset + 46 + nameLength,
      offset + 46 + nameLength + extraLength,
    );
    if (extra.length) readZip64Extra(extra, entry);
    entries.push(entry);
    offset += 46 + nameLength + extraLength + commentLength;
  }

  return { entries, entryCount: eocd.entryCount };
}

async function inflateRaw(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  const chunks = [];
  let total = 0;
  for await (const chunk of stream) {
    chunks.push(chunk);
    total += chunk.length;
  }
  const out = new Uint8Array(total);
  let cursor = 0;
  for (const chunk of chunks) {
    out.set(chunk, cursor);
    cursor += chunk.length;
  }
  return out;
}

/**
 * Inflates exactly one entry from the directory. The rest of the archive is left
 * untouched.
 */
export async function extractEntry(blob, entry) {
  // The local header carries its own, often longer extra field than the central
  // directory entry does; the payload starts only behind it.
  const header = view(await sliceBytes(blob, entry.localHeaderOffset, entry.localHeaderOffset + 30));
  if (header.getUint32(0, true) !== SIG_LOCAL) {
    throw new ZipError(`Beschädigter Eintrag: ${entry.name}`);
  }
  const dataStart =
    entry.localHeaderOffset + 30 + header.getUint16(26, true) + header.getUint16(28, true);
  const raw = await sliceBytes(blob, dataStart, dataStart + entry.compressedSize);

  if (entry.method === METHOD_STORE) return raw;
  if (entry.method === METHOD_DEFLATE) return inflateRaw(raw);
  throw new ZipError(`Nicht unterstütztes Kompressionsverfahren ${entry.method}.`);
}

/** Like extractEntry, but hands the content back as UTF-8 text. */
export async function extractEntryAsText(blob, entry) {
  return new TextDecoder('utf-8').decode(await extractEntry(blob, entry));
}
