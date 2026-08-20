// "Who liked?" - demo server.
//
// Two jobs, and no more than that:
//   1. serve the static page, and
//   2. answer "can this post be shown publicly?" for one shortcode at a time.
//
// The check has to happen here rather than in the browser because Instagram
// sends no CORS headers - a page cannot look inside its own embed iframe and ask
// whether it rendered a post or an error card. Doing it server-side also means
// the players' browsers never talk to Instagram at all: thumbnails are fetched
// once, stored next to this file and served from here.
//
// Node's standard library only. Start with `node server.js`.

import { createServer } from 'node:http';
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import { createReadStream } from 'node:fs';
import { dirname, join, normalize, extname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readEmbedPage, embedUrl, isProxyableMediaUrl } from './lib/embed.js';

const ROOT = dirname(fileURLToPath(import.meta.url));
const PUBLIC_DIR = join(ROOT, 'public');
// Configurable so a second instance - or a test run - does not share the
// answers of the first.
const CACHE_DIR = process.env.WL_CACHE_DIR
  ? join(process.cwd(), process.env.WL_CACHE_DIR)
  : join(ROOT, '.cache');
const CACHE_FILE = join(CACHE_DIR, 'embeds.json');
const THUMB_DIR = join(CACHE_DIR, 'thumbs');

const PORT = Number(process.env.WL_PORT || 8787);
// Instagram tolerates a browser's pace, not a scraper's. One request every
// 250 ms with three in flight walks 350 posts in about a minute and a half, and
// the result is cached, so it happens once.
const MIN_GAP_MS = Number(process.env.WL_MIN_GAP_MS || 250);
const REQUEST_TIMEOUT_MS = Number(process.env.WL_TIMEOUT_MS || 12000);

// A public post rarely turns private, but it does happen; a private one may come
// back. Different lifetimes keep the cache useful without freezing either answer
// forever. 'unknown' is never cached - it is not an answer.
const TTL_PUBLIC_MS = 7 * 24 * 60 * 60 * 1000;
const TTL_UNAVAILABLE_MS = 24 * 60 * 60 * 1000;

const BROWSER_HEADERS = {
  'User-Agent':
    'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ' +
    '(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36',
  'Accept-Language': 'de-DE,de;q=0.9,en;q=0.8',
  Accept: 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8',
};

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.webp': 'image/webp',
  '.ico': 'image/x-icon',
};

const SHORTCODE = /^[A-Za-z0-9_-]{5,64}$/;

// ---------------------------------------------------------------- cache -----

/** code -> {state, reason, username, isVideo, caption, hasThumb, checkedAt} */
const cache = new Map();
let cacheDirty = false;

async function loadCache() {
  try {
    const raw = JSON.parse(await readFile(CACHE_FILE, 'utf8'));
    for (const [code, record] of Object.entries(raw)) cache.set(code, record);
  } catch {
    // No cache yet, or an unreadable one - either way we start empty.
  }
}

async function saveCache() {
  if (!cacheDirty) return;
  cacheDirty = false;
  await mkdir(CACHE_DIR, { recursive: true });
  await writeFile(CACHE_FILE, JSON.stringify(Object.fromEntries(cache), null, 1), 'utf8');
}

function cachedAnswer(code) {
  const record = cache.get(code);
  if (!record) return null;
  const ttl = record.state === 'public' ? TTL_PUBLIC_MS : TTL_UNAVAILABLE_MS;
  if (Date.now() - record.checkedAt > ttl) return null;
  return record;
}

// -------------------------------------------------------------- throttle ----

let nextSlot = 0;

/** Spaces upstream requests out so we stay under Instagram's patience. */
function waitForSlot() {
  const now = Date.now();
  const slot = Math.max(now, nextSlot);
  nextSlot = slot + MIN_GAP_MS;
  return new Promise((resolve) => setTimeout(resolve, slot - now));
}

async function fetchWithTimeout(url, options = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    return await fetch(url, { ...options, signal: controller.signal, redirect: 'follow' });
  } finally {
    clearTimeout(timer);
  }
}

// --------------------------------------------------------------- resolve ----

const inFlight = new Map();

async function downloadThumbnail(code, url) {
  if (!isProxyableMediaUrl(url)) return false;
  try {
    await waitForSlot();
    const response = await fetchWithTimeout(url, { headers: BROWSER_HEADERS });
    if (!response.ok) return false;
    const bytes = Buffer.from(await response.arrayBuffer());
    if (!bytes.length) return false;
    await mkdir(THUMB_DIR, { recursive: true });
    // Stored by shortcode, not by CDN URL: those are signed and expire within
    // hours, so a cached URL would hand the gallery broken images tomorrow.
    await writeFile(join(THUMB_DIR, `${code}.jpg`), bytes);
    return true;
  } catch {
    return false;
  }
}

async function resolveCode(code) {
  let response;
  try {
    await waitForSlot();
    response = await fetchWithTimeout(embedUrl(code), { headers: BROWSER_HEADERS });
  } catch (error) {
    // Offline, DNS down, blocked by a proxy, timed out: we do not know, and
    // saying "unavailable" here would quietly delete the post from the game.
    return { state: 'unknown', reason: error?.name === 'AbortError' ? 'timeout' : 'network' };
  }

  const html = await response.text().catch(() => '');
  const verdict = readEmbedPage({ status: response.status, url: response.url, html });

  const record = {
    state: verdict.state,
    reason: verdict.reason,
    username: verdict.username || '',
    isVideo: Boolean(verdict.isVideo),
    caption: verdict.caption || '',
    width: verdict.width || 0,
    height: verdict.height || 0,
    hasThumb: false,
    checkedAt: Date.now(),
  };

  if (verdict.state === 'public') {
    record.hasThumb = await downloadThumbnail(code, verdict.thumbnailUrl);
  }
  return record;
}

async function resolveCached(code) {
  const hit = cachedAnswer(code);
  if (hit) return { ...hit, cached: true };
  if (inFlight.has(code)) return { ...(await inFlight.get(code)), cached: false };

  const pending = resolveCode(code);
  inFlight.set(code, pending);
  try {
    const record = await pending;
    // An 'unknown' is a missing answer, not an answer: caching it would freeze a
    // rate limit into place for a day.
    if (record.state !== 'unknown') {
      cache.set(code, record);
      cacheDirty = true;
    }
    return { ...record, cached: false };
  } finally {
    inFlight.delete(code);
  }
}

// ---------------------------------------------------------------- serving ---

function sendJson(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
    'Cache-Control': 'no-store',
  });
  res.end(payload);
}

async function serveStatic(res, urlPath) {
  const relative = urlPath === '/' ? '/index.html' : urlPath;
  // normalize() collapses "..", and the prefix check rejects anything that
  // climbed out of public/ anyway.
  const target = join(PUBLIC_DIR, normalize(relative));
  if (!target.startsWith(PUBLIC_DIR)) {
    res.writeHead(403).end('Forbidden');
    return;
  }
  try {
    const info = await stat(target);
    if (!info.isFile()) throw new Error('not a file');
    res.writeHead(200, {
      'Content-Type': MIME[extname(target).toLowerCase()] || 'application/octet-stream',
      'Content-Length': info.size,
      'Cache-Control': 'no-cache',
    });
    createReadStream(target).pipe(res);
  } catch {
    res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' })
      .end('Nicht gefunden');
  }
}

async function serveThumbnail(res, code) {
  const file = join(THUMB_DIR, `${code}.jpg`);
  try {
    const info = await stat(file);
    res.writeHead(200, {
      'Content-Type': 'image/jpeg',
      'Content-Length': info.size,
      'Cache-Control': 'public, max-age=86400',
    });
    createReadStream(file).pipe(res);
  } catch {
    res.writeHead(404).end();
  }
}

const server = createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);

  try {
    if (url.pathname === '/api/status') {
      const states = { public: 0, unavailable: 0 };
      for (const record of cache.values()) {
        if (record.state in states) states[record.state]++;
      }
      sendJson(res, 200, { ok: true, cached: cache.size, states });
      return;
    }

    if (url.pathname === '/api/resolve') {
      const code = url.searchParams.get('code') || '';
      if (!SHORTCODE.test(code)) {
        sendJson(res, 400, { state: 'unknown', reason: 'bad-code' });
        return;
      }
      const record = await resolveCached(code);
      sendJson(res, 200, { code, ...record });
      return;
    }

    if (url.pathname === '/api/thumb') {
      const code = url.searchParams.get('code') || '';
      if (!SHORTCODE.test(code)) {
        res.writeHead(400).end();
        return;
      }
      await serveThumbnail(res, code);
      return;
    }

    if (url.pathname.startsWith('/api/')) {
      sendJson(res, 404, { error: 'unbekannter Endpunkt' });
      return;
    }

    await serveStatic(res, url.pathname);
  } catch (error) {
    console.error('[who-liked]', error);
    if (!res.headersSent) sendJson(res, 500, { error: 'interner Fehler' });
    else res.end();
  }
});

// The cache is written on a timer rather than on every answer, so a full run
// does not turn into 350 file writes.
const cacheTimer = setInterval(() => {
  saveCache().catch((error) => console.error('[who-liked] Cache:', error));
}, 5000);
cacheTimer.unref();

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => {
    saveCache().finally(() => process.exit(0));
  });
}

await loadCache();
server.listen(PORT, () => {
  console.log(`Who liked?  ->  http://localhost:${PORT}`);
  console.log(`${cache.size} Beiträge bereits geprüft (Cache: ${CACHE_FILE})`);
});
