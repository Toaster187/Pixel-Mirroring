// Reads Instagram's public embed page and decides whether a post can be shown.
//
// This module is deliberately pure: HTML in, verdict out, no sockets. That is
// what makes it testable without reaching Instagram, and the network side in
// server.js stays a thin wrapper around it.
//
// The verdict is three-valued on purpose:
//   'public'      - a media URL was found, the post can be embedded
//   'unavailable' - the post is private, deleted, or the embed carries no media
//   'unknown'     - we could not find out (rate limit, login wall, network)
// Only 'public' ever reaches the game. 'unknown' must not silently become
// 'unavailable': a rate limit would otherwise look like "all your friends went
// private" and quietly empty the whole game.

/** Hosts whose images we are willing to fetch and proxy. */
const MEDIA_HOSTS = /(^|\.)(cdninstagram\.com|fbcdn\.net)$/i;

/** Phrases Instagram serves in place of a post that is gone or private. */
const GONE_MARKERS = [
  "sorry, this page isn't available",
  'sorry, this page isn&#x27;t available',
  'diese seite ist leider nicht verf', // "verfügbar", umlaut-agnostic
  'this post is unavailable',
  'dieser beitrag ist nicht verf',
  'content isn&#x27;t available',
  "content isn't available",
];

const LOGIN_MARKERS = [
  '/accounts/login',
  'loginandsignuppage',
  'log in to instagram',
];

function decodeHtmlEntities(text) {
  return text
    .replace(/&quot;/g, '"')
    .replace(/&#x27;|&#39;/g, "'")
    .replace(/&#x2F;|&#47;/g, '/')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&');
}

/** Unescapes the JS string literals Instagram inlines into <script> blocks. */
function decodeJsString(text) {
  return text
    .replace(/\\u0026/gi, '&')
    .replace(/\\u003C/gi, '<')
    .replace(/\\u003E/gi, '>')
    .replace(/\\\//g, '/')
    .replace(/\\"/g, '"');
}

function isMediaUrl(value) {
  if (typeof value !== 'string' || !value.startsWith('http')) return false;
  try {
    return MEDIA_HOSTS.test(new URL(value).hostname);
  } catch {
    return false;
  }
}

/** True when the URL may be fetched by the image proxy. */
export function isProxyableMediaUrl(value) {
  return isMediaUrl(value);
}

function firstMatch(html, pattern, group = 1) {
  const match = html.match(pattern);
  return match ? match[group] : '';
}

// Tier 1: older embeds hand the whole media object to a bootstrap call. When it
// is there it is by far the best source - it carries the owner, the caption and
// an explicit is_video flag instead of guesses.
function fromAdditionalData(html) {
  const start = html.indexOf('__additionalDataLoaded');
  if (start === -1) return null;
  const braceStart = html.indexOf('{', start);
  if (braceStart === -1) return null;

  // Walk the braces to find where the JSON argument ends; a regex cannot do
  // this, and the argument runs to the end of a very long line.
  let depth = 0;
  let inString = false;
  let escaped = false;
  let end = -1;
  for (let i = braceStart; i < html.length; i++) {
    const char = html[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (char === '\\') {
      escaped = true;
      continue;
    }
    if (char === '"') inString = !inString;
    if (inString) continue;
    if (char === '{') depth++;
    if (char === '}' && --depth === 0) {
      end = i + 1;
      break;
    }
  }
  if (end === -1) return null;

  let media;
  try {
    media = JSON.parse(html.slice(braceStart, end))?.graphql?.shortcode_media ??
      JSON.parse(html.slice(braceStart, end))?.shortcode_media;
  } catch {
    return null;
  }
  if (!media) return null;

  const thumb = [media.display_url, media.thumbnail_src].find(isMediaUrl);
  if (!thumb) return null;
  return {
    thumbnailUrl: thumb,
    username: typeof media.owner?.username === 'string' ? media.owner.username : '',
    isVideo: Boolean(media.is_video),
    caption: media.edge_media_to_caption?.edges?.[0]?.node?.text ?? '',
    width: media.dimensions?.width ?? 0,
    height: media.dimensions?.height ?? 0,
  };
}

// Tier 2: the server-rendered embed. The class name is the stable part here -
// attribute order around it is not, so the src is pulled from the whole tag.
function fromEmbeddedMarkup(html) {
  const imageTags = [...html.matchAll(/<img\b[^>]*>/gi)].map((match) => match[0]);
  const embedded = imageTags.filter((tag) => /EmbeddedMediaImage/i.test(tag));
  const candidates = (embedded.length ? embedded : imageTags)
    .map((tag) => decodeHtmlEntities(firstMatch(tag, /\bsrc=["']([^"']+)["']/i)))
    .filter(isMediaUrl);
  if (!candidates.length) return null;

  // The profile link in the embed header is the only bare /handle/ link on the
  // page; post and explore links have a second path segment.
  const profileLink = firstMatch(
    html,
    /href=["']https?:\/\/(?:www\.)?instagram\.com\/([A-Za-z0-9._]{1,30})\/?(?:\?[^"']*)?["']/i,
  );

  return {
    thumbnailUrl: candidates[0],
    username: profileLink && profileLink !== 'p' && profileLink !== 'reel' ? profileLink : '',
    isVideo: /<video\b/i.test(html) || /"is_video"\s*:\s*true/i.test(html) ||
      /property=["']og:video/i.test(html),
    caption: '',
    width: 0,
    height: 0,
  };
}

// Tier 3: the Open Graph tags. Present even on stripped-down embeds and enough
// to tell "there is media here" from "there is not".
function fromOpenGraph(html) {
  const image = decodeHtmlEntities(firstMatch(
    html,
    /<meta[^>]+property=["']og:image["'][^>]+content=["']([^"']+)["']/i,
  ));
  if (!isMediaUrl(image)) return null;
  return {
    thumbnailUrl: image,
    username: '',
    isVideo: /property=["']og:video/i.test(html),
    caption: '',
    width: 0,
    height: 0,
  };
}

function containsAny(haystack, needles) {
  return needles.some((needle) => haystack.includes(needle));
}

/**
 * Turns one embed response into a verdict.
 *
 * @param {{status: number, url: string, html: string}} response
 * @returns {{state: 'public'|'unavailable'|'unknown', reason: string, thumbnailUrl?: string,
 *            username?: string, isVideo?: boolean, caption?: string,
 *            width?: number, height?: number}}
 */
export function readEmbedPage({ status, url, html }) {
  if (status === 404 || status === 410) {
    return { state: 'unavailable', reason: 'not-found' };
  }
  if (status === 429) return { state: 'unknown', reason: 'rate-limited' };
  if (status >= 500) return { state: 'unknown', reason: `server-${status}` };
  if (status !== 200) return { state: 'unknown', reason: `http-${status}` };

  const lower = (html || '').toLowerCase();
  // A redirect to the login page means Instagram stopped answering us, not that
  // the post is private - those two must not be confused.
  if (containsAny((url || '').toLowerCase(), ['/accounts/login']) ||
      (containsAny(lower, LOGIN_MARKERS) && !lower.includes('embeddedmediaimage'))) {
    return { state: 'unknown', reason: 'login-required' };
  }

  const found = fromAdditionalData(decodeJsString(html)) ||
    fromEmbeddedMarkup(html) ||
    fromOpenGraph(html);

  if (!found) {
    return {
      state: 'unavailable',
      reason: containsAny(lower, GONE_MARKERS) ? 'private-or-deleted' : 'no-media',
    };
  }

  return {
    state: 'public',
    reason: 'ok',
    thumbnailUrl: found.thumbnailUrl,
    username: (found.username || '').toLowerCase(),
    isVideo: found.isVideo,
    caption: found.caption || '',
    width: found.width || 0,
    height: found.height || 0,
  };
}

/** The embed URL for a shortcode. /p/ serves reels and IGTV alike. */
export function embedUrl(code) {
  return `https://www.instagram.com/p/${encodeURIComponent(code)}/embed/captioned/`;
}
