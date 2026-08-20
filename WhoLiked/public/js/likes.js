// Parser for likes/liked_posts.json out of an Instagram data export.
//
// The file has had at least three shapes over the years and its field labels are
// translated into the account's language, so nothing here may key off a German
// (or English) label alone. The order is: known structured formats first, and a
// recursive sweep for post URLs as a safety net for whatever Instagram ships
// next. Everything is best-effort - a like we cannot read is dropped, never
// guessed at.

/** Matches the post part of any instagram.com permalink. */
const POST_URL = /https?:\/\/(?:www\.)?instagram\.com\/(p|reel|reels|tv)\/([A-Za-z0-9_-]{5,})/i;
const POST_URL_GLOBAL = new RegExp(POST_URL.source, 'gi');
const HANDLE = /^[A-Za-z0-9._]{1,30}$/;

// "URL" happens to be spelled URL in every locale Instagram exports, which is
// what makes the owner group findable without a translation table.
const URL_LABEL = 'url';

const OWNER_GROUP_TITLES = new Set([
  'eigentümer', 'owner', 'propietario', 'propriétaire', 'proprietario',
  'proprietário', 'eigenaar', 'właściciel', 'sahibi', 'ägare', 'ejer', 'eier',
  'omistaja', 'vlastník', 'владелец', 'pemilik',
]);

const USERNAME_LABELS = new Set([
  'benutzername', 'username', 'nombre de usuario', "nom d'utilisateur",
  'nome utente', 'nome de usuário', 'nome de utilizador', 'gebruikersnaam',
  'nazwa użytkownika', 'kullanıcı adı', 'användarnamn', 'brugernavn',
  'brukernavn', 'käyttäjätunnus', 'uživatelské jméno', 'имя пользователя',
]);

const CAPTION_LABELS = new Set([
  'untertitel', 'caption', 'bildunterschrift', 'subtítulo', 'légende',
  'didascalia', 'legenda', 'onderschrift', 'podpis', 'altyazı', 'bildtext',
  'kuvateksti', 'подпись', 'titel', 'title', 'título', 'titre', 'titolo',
]);

/**
 * Instagram exports UTF-8 that was written as if it were Latin-1, so a Japanese
 * character arrives as a run of accented Latin ones and "ü" as "Ã¼". Reversing
 * that is only safe when every code point fits in a byte and the result is valid
 * UTF-8 - otherwise the string was fine to begin with and is handed back
 * untouched.
 */
export function fixMojibake(text) {
  if (typeof text !== 'string' || !/[À-ÿ]/.test(text)) return text;
  let current = text;
  // Some fields went through the same broken conversion twice.
  for (let pass = 0; pass < 2; pass++) {
    const bytes = new Uint8Array(current.length);
    for (let i = 0; i < current.length; i++) {
      const code = current.charCodeAt(i);
      if (code > 0xff) return current;
      bytes[i] = code;
    }
    let decoded;
    try {
      decoded = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    } catch {
      return current;
    }
    if (decoded === current) return current;
    current = decoded;
    if (!/[À-ÿ]/.test(current)) return current;
  }
  return current;
}

function parsePostUrl(value) {
  const match = typeof value === 'string' ? value.match(POST_URL) : null;
  if (!match) return null;
  // /reels/<code> is the same thing as /reel/<code>; normalise so the same post
  // liked by two players collapses into one round.
  const kind = match[1].toLowerCase() === 'reels' ? 'reel' : match[1].toLowerCase();
  return { kind, code: match[2] };
}

function makePost({ kind, code }, { owner = '', caption = '', likedAt = 0 } = {}) {
  return {
    code,
    kind,
    url: `https://www.instagram.com/${kind}/${code}/`,
    owner: HANDLE.test(owner) ? owner.toLowerCase() : '',
    caption: (caption || '').trim(),
    likedAt: Number.isFinite(likedAt) ? likedAt : 0,
  };
}

/** Flattens one `label_values` group into {labelLowercase: value} pairs. */
function labelPairs(entries) {
  const pairs = new Map();
  for (const entry of entries || []) {
    if (typeof entry?.label === 'string') {
      pairs.set(fixMojibake(entry.label).toLowerCase(), fixMojibake(entry.value ?? ''));
    }
  }
  return pairs;
}

// The owner sits in a nested group whose title is localised. Three tiers, most
// specific first, so an unknown language still lands on the right group instead
// of on "branded partners".
function findOwner(labelValues) {
  const groups = labelValues.filter((entry) => Array.isArray(entry?.dict));
  const byTitle = groups.find((group) =>
    OWNER_GROUP_TITLES.has(fixMojibake(group.title ?? '').toLowerCase()));
  const candidates = byTitle ? [byTitle] : groups;

  for (const group of candidates) {
    for (const member of group.dict) {
      const pairs = labelPairs(member?.dict);
      for (const [label, value] of pairs) {
        if (USERNAME_LABELS.has(label) && HANDLE.test(value)) return value;
      }
      // Unknown language: the handle is the last non-URL value in a group that
      // also carries a URL, which is the layout every export has used so far.
      if (pairs.has(URL_LABEL) && pairs.size >= 2) {
        const values = [...pairs].filter(([label]) => label !== URL_LABEL).map(([, v]) => v);
        const last = values[values.length - 1];
        if (last && HANDLE.test(last)) return last;
      }
    }
  }
  return '';
}

function findCaption(pairs) {
  for (const [label, value] of pairs) {
    if (CAPTION_LABELS.has(label) && value) return value;
  }
  return '';
}

// Current format (seen in exports from 2025 onwards): a flat array of entries,
// each with a timestamp and a list of localised label/value pairs.
function parseLabelValueFormat(root) {
  if (!Array.isArray(root)) return null;
  const posts = [];
  for (const item of root) {
    const labelValues = item?.label_values;
    if (!Array.isArray(labelValues)) continue;
    const pairs = labelPairs(labelValues);
    const link = parsePostUrl(pairs.get(URL_LABEL)) ||
      parsePostUrl(labelValues.find((entry) => entry?.href)?.href);
    if (!link) continue;
    posts.push(makePost(link, {
      owner: findOwner(labelValues),
      caption: findCaption(pairs),
      likedAt: item.timestamp,
    }));
  }
  return posts.length ? posts : null;
}

// Classic format: {"likes_media_likes":[{title, string_list_data:[{href,timestamp}]}]}
// Here `title` is the post author's handle.
function parseClassicFormat(root) {
  const list = root?.likes_media_likes;
  if (!Array.isArray(list)) return null;
  const posts = [];
  for (const item of list) {
    const owner = fixMojibake(item?.title ?? '');
    for (const link of item?.string_list_data ?? []) {
      const parsed = parsePostUrl(link?.href) || parsePostUrl(link?.value);
      if (!parsed) continue;
      posts.push(makePost(parsed, { owner, likedAt: link?.timestamp }));
    }
  }
  return posts.length ? posts : null;
}

// Safety net: pull every post permalink out of the raw text, in document order.
// Loses the owner and the like date, but a format change then costs metadata
// rather than the whole import.
function sweepForPostUrls(text) {
  const seen = new Set();
  const posts = [];
  for (const match of text.matchAll(POST_URL_GLOBAL)) {
    const link = parsePostUrl(match[0]);
    if (!link || seen.has(link.code)) continue;
    seen.add(link.code);
    posts.push(makePost(link));
  }
  return posts;
}

/**
 * Parses the contents of liked_posts.json.
 * @returns {{posts: Array, format: string, duplicates: number}}
 */
export function parseLikedPosts(text) {
  let root = null;
  try {
    root = JSON.parse(text);
  } catch {
    root = null;
  }

  let format = 'label_values';
  let posts = root ? parseLabelValueFormat(root) : null;
  if (!posts) {
    format = 'likes_media_likes';
    posts = parseClassicFormat(root);
  }
  if (!posts) {
    format = 'url-sweep';
    posts = sweepForPostUrls(text);
  }

  // The same post can appear twice (liked, unliked, liked again). Keep the first
  // occurrence - the export lists the most recent like first.
  const byCode = new Map();
  let duplicates = 0;
  for (const post of posts) {
    if (byCode.has(post.code)) {
      duplicates++;
      continue;
    }
    byCode.set(post.code, post);
  }

  return { posts: [...byCode.values()], format, duplicates };
}

/**
 * Finds the liked-posts entry in a ZIP index. Instagram nests it under a
 * per-export folder whose name contains the account and the export date, and
 * older exports put it in a `json/` sibling, so only the tail of the path is
 * matched.
 */
export function findLikedPostsEntry(entries) {
  const candidates = entries.filter((entry) =>
    /(^|\/)liked_posts(_\d+)?\.json$/i.test(entry.name));
  if (!candidates.length) return null;
  // Prefer the canonical likes/ location over anything else that matched.
  return candidates.find((entry) => /(^|\/)likes\/liked_posts\.json$/i.test(entry.name)) ||
    candidates[0];
}

/**
 * Guesses the account handle from the export's folder name
 * ("instagram-<handle>-2026-08-18-Bhv5PiTc"). Only used to pre-fill the player
 * name field, and silently gives up when the name does not match.
 */
export function guessAccountName(entries) {
  for (const entry of entries) {
    const match = entry.name.match(/^instagram-([A-Za-z0-9._]{1,30})-\d{4}-\d{2}-\d{2}/);
    if (match) return match[1];
  }
  return '';
}
