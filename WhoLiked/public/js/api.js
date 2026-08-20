// Talks to the local check server.
//
// The browser cannot decide on its own whether an Instagram post is public:
// Instagram serves no CORS headers, so a page can neither read the embed nor
// look inside the iframe it renders. The server does that and answers with one
// of three states; only 'public' is ever shown.

const CONCURRENCY = 4;

export async function checkServer() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) return { available: false, cached: 0 };
    const body = await response.json();
    return { available: Boolean(body.ok), cached: body.cached || 0 };
  } catch {
    // Typically the page was opened straight from the file system, so there is
    // no server behind it at all.
    return { available: false, cached: 0 };
  }
}

async function resolveOne(code, signal) {
  try {
    const response = await fetch(`/api/resolve?code=${encodeURIComponent(code)}`, { signal });
    if (!response.ok) return { code, state: 'unknown', reason: `http-${response.status}` };
    return await response.json();
  } catch (error) {
    if (error?.name === 'AbortError') throw error;
    return { code, state: 'unknown', reason: 'offline' };
  }
}

/**
 * Resolves a list of shortcodes with a small worker pool.
 *
 * @param {string[]} codes
 * @param {{onResult?: Function, signal?: AbortSignal}} options
 * @returns {Promise<Map<string, object>>}
 */
export async function resolveAll(codes, { onResult, signal } = {}) {
  const results = new Map();
  let cursor = 0;

  async function worker() {
    while (cursor < codes.length) {
      if (signal?.aborted) return;
      const code = codes[cursor++];
      const record = await resolveOne(code, signal);
      results.set(code, record);
      onResult?.(record, results.size, codes.length);
    }
  }

  const workers = Array.from({ length: Math.min(CONCURRENCY, codes.length) }, worker);
  try {
    await Promise.all(workers);
  } catch (error) {
    if (error?.name !== 'AbortError') throw error;
  }
  return results;
}

/** URL of the locally cached preview image for a post. */
export function thumbnailUrl(code) {
  return `/api/thumb?code=${encodeURIComponent(code)}`;
}

/** Instagram's official embed, used only for posts already verified public. */
export function embedSrc(code) {
  return `https://www.instagram.com/p/${encodeURIComponent(code)}/embed/captioned/`;
}
