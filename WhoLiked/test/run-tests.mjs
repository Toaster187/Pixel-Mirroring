// Standalone self-checks for the "Who liked?" demo. No test framework: a plain
// main() and a check() that throws, in the style of the checks under Client/.
//
// Run with `npm test` or `node test/run-tests.mjs`.
//
// What is covered: the ZIP reader, the liked_posts.json parser, the public/
// private verdict, the round building and the HTTP surface. What is not, and
// cannot be from here: the live embed request against Instagram itself.

import { spawn } from 'node:child_process';
import { rm, mkdir, writeFile } from 'node:fs/promises';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { request } from 'node:http';

import { makeZip } from './make-zip.mjs';
import { readZipIndex, extractEntryAsText } from '../public/js/zip.js';
import { parseLikedPosts, findLikedPostsEntry, guessAccountName, fixMojibake } from '../public/js/likes.js';
import { readEmbedPage, isProxyableMediaUrl } from '../lib/embed.js';
import { buildPool, buildRounds, createGame, submitGuess, advance, ranking } from '../public/js/game.js';

const ROOT = dirname(dirname(fileURLToPath(import.meta.url)));
const TEST_CACHE = '.cache-test';
const TEST_PORT = 8799;

let checks = 0;
let failures = 0;
let group = '';

function check(condition, description) {
  checks++;
  if (condition) return;
  failures++;
  console.error(`  FAIL  [${group}] ${description}`);
}

function equal(actual, expected, description) {
  check(
    JSON.stringify(actual) === JSON.stringify(expected),
    `${description} (erwartet ${JSON.stringify(expected)}, war ${JSON.stringify(actual)})`,
  );
}

function section(name) {
  group = name;
  console.log(`\n${name}`);
}

// ------------------------------------------------------------------ zip -----

async function testZip() {
  section('ZIP-Leser');

  const likes = JSON.stringify([{
    timestamp: 1700000000,
    label_values: [
      { label: 'URL', value: 'https://www.instagram.com/reel/AAAAAAAAAAA/' },
      { title: 'Eigentümer', dict: [{ dict: [
        { label: 'URL', value: 'https://example.org' },
        { label: 'Name', value: 'Beispiel' },
        { label: 'Benutzername', value: 'beispiel' },
      ] }] },
    ],
  }]);

  const archive = await makeZip([
    { name: 'instagram-testuser-2026-08-18-AbCdEf/your_instagram_activity/messages/inbox/geheim/message_1.json', content: 'PRIVAT'.repeat(200) },
    { name: 'instagram-testuser-2026-08-18-AbCdEf/your_instagram_activity/likes/liked_posts.json', content: likes },
    { name: 'instagram-testuser-2026-08-18-AbCdEf/media/posts_1.json', content: 'x'.repeat(500), store: true },
  ]);
  const blob = new Blob([archive]);

  const index = await readZipIndex(blob);
  equal(index.entries.length, 3, 'alle Einträge im Verzeichnis');
  equal(index.entryCount, 3, 'angekündigte Anzahl stimmt');

  const entry = findLikedPostsEntry(index.entries);
  check(Boolean(entry), 'liked_posts.json wird gefunden');
  check(entry.name.endsWith('likes/liked_posts.json'), 'und zwar der Eintrag unter likes/');
  equal(guessAccountName(index.entries), 'testuser', 'Kontoname aus dem Ordnernamen');

  const text = await extractEntryAsText(blob, entry);
  equal(text, likes, 'entpackter Inhalt ist unverändert');

  // The stored entry exercises the method-0 path, which a real export uses for
  // already-compressed files.
  const stored = index.entries.find((item) => item.name.endsWith('posts_1.json'));
  equal(stored.method, 0, 'gespeicherter Eintrag ist unkomprimiert');
  equal((await extractEntryAsText(blob, stored)).length, 500, 'gespeicherter Eintrag lesbar');

  const deflated = index.entries.find((item) => item.name.endsWith('message_1.json'));
  equal(deflated.method, 8, 'Nachrichten-Eintrag ist deflate-komprimiert');
  check(deflated.compressedSize < deflated.uncompressedSize, 'und tatsächlich kleiner');

  // The real export, when it happens to be lying around. Never committed.
  const sample = join(ROOT, 'test', 'sample-export.zip');
  if (existsSync(sample)) {
    const real = new Blob([readFileSync(sample)]);
    const realIndex = await readZipIndex(real);
    const realEntry = findLikedPostsEntry(realIndex.entries);
    check(Boolean(realEntry), 'echter Export: liked_posts.json gefunden');
    const parsed = parseLikedPosts(await extractEntryAsText(real, realEntry));
    check(parsed.posts.length > 0, `echter Export: ${parsed.posts.length} Likes gelesen`);
  }
}

// ---------------------------------------------------------------- likes -----

async function testLikes() {
  section('liked_posts.json');

  const modern = parseLikedPosts(JSON.stringify([
    {
      timestamp: 1700000001,
      label_values: [
        { label: 'URL', value: 'https://www.instagram.com/reel/CODE000001/' },
        { label: 'Untertitel', value: 'Ein Gruß' },
        { title: 'Eigentümer', dict: [{ dict: [
          { label: 'URL', value: 'http://nothing.tech' },
          { label: 'Name', value: 'Nothing' },
          { label: 'Benutzername', value: 'Nothing' },
        ] }] },
        { title: 'Markenpartner', dict: [] },
      ],
    },
    {
      timestamp: 1700000002,
      label_values: [
        { label: 'URL', value: 'https://www.instagram.com/reels/CODE000002/' },
        { title: 'Owner', dict: [{ dict: [
          { label: 'URL', value: 'https://instagram.com/someone' },
          { label: 'Name', value: 'Some One' },
          { label: 'Username', value: 'someone' },
        ] }] },
      ],
    },
    // Same post again: liked, unliked, liked once more.
    {
      timestamp: 1700000003,
      label_values: [{ label: 'URL', value: 'https://www.instagram.com/reel/CODE000001/' }],
    },
    // A story like or anything else without a post URL must not become a round.
    { timestamp: 1700000004, label_values: [{ label: 'URL', value: 'https://example.com/x' }] },
  ]));

  equal(modern.format, 'label_values', 'neues Format erkannt');
  equal(modern.posts.length, 2, 'zwei eindeutige Beiträge');
  equal(modern.duplicates, 1, 'ein Duplikat verworfen');
  equal(modern.posts[0].owner, 'nothing', 'Owner-Handle klein geschrieben');
  equal(modern.posts[0].caption, 'Ein Gruß', 'Untertitel übernommen');
  equal(modern.posts[1].kind, 'reel', '/reels/ wird zu /reel/ vereinheitlicht');
  equal(modern.posts[1].url, 'https://www.instagram.com/reel/CODE000002/', 'kanonische URL');

  // An export in a language the label tables do not know must still yield the
  // owner, via the positional fallback.
  const foreign = parseLikedPosts(JSON.stringify([{
    timestamp: 1700000005,
    label_values: [
      { label: 'URL', value: 'https://www.instagram.com/p/CODE000003/' },
      { title: 'Omanaja', dict: [{ dict: [
        { label: 'URL', value: 'https://example.org' },
        { label: 'Nimi', value: 'Ein Name' },
        { label: 'Kayttajatunnus', value: 'handle_x' },
      ] }] },
    ],
  }]));
  equal(foreign.posts[0].owner, 'handle_x', 'Owner auch ohne bekanntes Label');

  const classic = parseLikedPosts(JSON.stringify({
    likes_media_likes: [{
      title: 'oldschool',
      string_list_data: [{ href: 'https://www.instagram.com/p/CODE000004/', timestamp: 1600000000 }],
    }],
  }));
  equal(classic.format, 'likes_media_likes', 'klassisches Format erkannt');
  equal(classic.posts[0].owner, 'oldschool', 'Owner aus dem Titel');

  const swept = parseLikedPosts('{"neu":[{"x":"https://instagram.com/tv/CODE000005/"}]}');
  equal(swept.format, 'url-sweep', 'unbekanntes Format fällt auf die URL-Suche zurück');
  equal(swept.posts.length, 1, 'und findet den Beitrag trotzdem');

  equal(parseLikedPosts('kein json').posts, [], 'kaputte Datei liefert nichts, statt zu werfen');

  section('Zeichenkodierung');
  // Instagram writes UTF-8 as if it were Latin-1. "Grüße" survives the round
  // trip; the mangled form has to be repaired.
  equal(fixMojibake('GrÃ¼Ãe'), 'Grüße', 'doppelt kodierte Umlaute repariert');
  equal(fixMojibake('Grüße'), 'Grüße', 'saubere Umlaute bleiben unangetastet');
  equal(fixMojibake('plain ascii'), 'plain ascii', 'ASCII bleibt ASCII');
  // Katakana: three mangled Latin characters per real one.
  equal(fixMojibake('ã¢'), 'ア', 'japanische Zeichen repariert');
  equal(fixMojibake('100 % Café'), '100 % Café', 'einzelnes é wird nicht zerstört');
}

// ---------------------------------------------------------------- embed -----

function testEmbed() {
  section('Öffentlich-Prüfung');

  const withData = readEmbedPage({
    status: 200,
    url: 'https://www.instagram.com/p/X/embed/captioned/',
    html: `<script>window.__additionalDataLoaded('extra',{"shortcode_media":{
      "display_url":"https://scontent.cdninstagram.com/v/abc.jpg","is_video":true,
      "owner":{"username":"TestUser"},"dimensions":{"width":1080,"height":1350},
      "edge_media_to_caption":{"edges":[{"node":{"text":"Hallo"}}]}}});</script>`,
  });
  equal(withData.state, 'public', 'shortcode_media wird als öffentlich gewertet');
  equal(withData.username, 'testuser', 'Benutzername normalisiert');
  equal(withData.isVideo, true, 'Video erkannt');
  equal(withData.caption, 'Hallo', 'Bildunterschrift übernommen');
  equal(withData.width, 1080, 'Breite übernommen');

  const rendered = readEmbedPage({
    status: 200,
    url: 'https://www.instagram.com/p/X/embed/captioned/',
    html: `<a href="https://www.instagram.com/someone/?utm_source=ig_embed">someone</a>
      <img class="EmbeddedMediaImage" src="https://scontent-fra.cdninstagram.com/v/x.jpg?a=1&amp;b=2">
      <video src="blob:x"></video>`,
  });
  equal(rendered.state, 'public', 'server-gerendertes Embed erkannt');
  equal(rendered.username, 'someone', 'Profil-Link liefert den Benutzernamen');
  check(rendered.thumbnailUrl.includes('&b=2'), 'HTML-Entities in der Bild-URL aufgelöst');

  const og = readEmbedPage({
    status: 200,
    url: 'https://www.instagram.com/p/X/embed/',
    html: '<meta property="og:image" content="https://scontent.cdninstagram.com/v/og.jpg">',
  });
  equal(og.state, 'public', 'og:image reicht als Nachweis');

  // Everything below must never be shown.
  equal(
    readEmbedPage({ status: 200, url: 'x', html: "<p>Sorry, this page isn't available.</p>" }).reason,
    'private-or-deleted',
    'Fehlerseite als privat/gelöscht erkannt',
  );
  equal(readEmbedPage({ status: 200, url: 'x', html: '<html></html>' }).state, 'unavailable',
    'Seite ohne Medien gilt als nicht abrufbar');
  equal(readEmbedPage({ status: 404, url: 'x', html: '' }).state, 'unavailable', '404 ist nicht abrufbar');
  equal(readEmbedPage({ status: 410, url: 'x', html: '' }).state, 'unavailable', '410 ist nicht abrufbar');

  // ... and everything here is a missing answer, not a negative one.
  equal(readEmbedPage({ status: 429, url: 'x', html: '' }).state, 'unknown', '429 bleibt ungeklärt');
  equal(readEmbedPage({ status: 503, url: 'x', html: '' }).state, 'unknown', '5xx bleibt ungeklärt');
  equal(readEmbedPage({ status: 403, url: 'x', html: '' }).state, 'unknown', '403 bleibt ungeklärt');
  equal(
    readEmbedPage({ status: 200, url: 'https://www.instagram.com/accounts/login/?next=/p/X/', html: '' }).reason,
    'login-required',
    'Weiterleitung auf die Anmeldung ist kein „privat“',
  );

  // A thumbnail URL from anywhere else must not be fetched by the proxy.
  check(isProxyableMediaUrl('https://scontent.cdninstagram.com/v/a.jpg'), 'CDN-Host erlaubt');
  check(isProxyableMediaUrl('https://scontent-fra3-1.xx.fbcdn.net/v/a.jpg'), 'fbcdn erlaubt');
  check(!isProxyableMediaUrl('https://evil.example.com/a.jpg'), 'fremder Host abgelehnt');
  check(!isProxyableMediaUrl('https://cdninstagram.com.evil.example/a.jpg'), 'Host-Suffix-Trick abgelehnt');
  check(!isProxyableMediaUrl('file:///etc/passwd'), 'Datei-URL abgelehnt');
}

// ----------------------------------------------------------------- game -----

function post(code, owner = 'someone') {
  return { code, kind: 'reel', url: `https://www.instagram.com/reel/${code}/`, owner, caption: '', likedAt: 0 };
}

function testGame() {
  section('Spiellogik');

  const players = [
    { id: 'p1', name: 'Anna', color: '#f00', posts: [post('AAA'), post('BBB'), post('SHARED')] },
    { id: 'p2', name: 'Bo', color: '#0f0', posts: [post('CCC'), post('PRIV'), post('SHARED'), post('DUNNO')] },
  ];
  const resolutions = new Map([
    ['AAA', { state: 'public', username: 'a', isVideo: false, caption: '' }],
    ['BBB', { state: 'public', username: 'b', isVideo: true, caption: '' }],
    ['CCC', { state: 'public', username: 'c', isVideo: false, caption: '' }],
    ['SHARED', { state: 'public', username: 's', isVideo: false, caption: '' }],
    ['PRIV', { state: 'unavailable', reason: 'private-or-deleted' }],
    ['DUNNO', { state: 'unknown', reason: 'rate-limited' }],
  ]);

  const pool = buildPool(players, resolutions);
  equal(pool.entries.length, 4, 'nur öffentliche Beiträge im Pool');
  equal(pool.hidden.unavailable, 1, 'ein privater Beitrag ausgeblendet');
  equal(pool.hidden.unknown, 1, 'ein ungeklärter Beitrag ausgeblendet');
  const shared = pool.entries.find((entry) => entry.code === 'SHARED');
  equal(shared.likedBy, ['p1', 'p2'], 'gemeinsamer Like wird zu einem Eintrag zusammengefasst');
  check(!pool.entries.some((entry) => entry.code === 'PRIV'), 'privater Beitrag kommt nicht ins Spiel');

  // Deterministic order so the assertions below are stable.
  const rounds = buildRounds(pool.entries, players, { count: 10, rng: () => 0 });
  equal(rounds.length, 4, 'alle Einträge werden zu Runden');
  check(
    rounds.slice(0, 3).every((round) => round.likedBy.length === 1),
    'eindeutige Beiträge kommen vor den gemeinsamen',
  );
  equal(rounds[rounds.length - 1].code, 'SHARED', 'der gemeinsame Beitrag kommt zuletzt');
  equal(rounds.map((round) => round.guesserId), ['p1', 'p2', 'p1', 'p2'], 'Rateposition wechselt reihum');

  const game = createGame(players, rounds);
  const first = rounds[0];
  const wrongPlayer = players.find((player) => !first.likedBy.includes(player.id));
  equal(submitGuess(game, wrongPlayer.id).correct, false, 'falscher Tipp gibt keinen Punkt');
  equal(game.scores[first.guesserId], 0, 'Punktestand bleibt bei 0');
  equal(submitGuess(game, first.likedBy[0]), null, 'zweiter Tipp in derselben Runde zählt nicht');

  advance(game);
  const second = game.rounds[1];
  equal(submitGuess(game, second.likedBy[0]).correct, true, 'richtiger Tipp erkannt');
  equal(game.scores[second.guesserId], 1, 'Punkt geht an die ratende Person');

  // On a shared post naming any one of the likers counts.
  game.index = 3;
  game.answered = false;
  equal(submitGuess(game, 'p2').correct, true, 'bei gemeinsamem Like zählt jede der Personen');

  const board = ranking(game);
  check(board[0].score >= board[board.length - 1].score, 'Endstand ist absteigend sortiert');
}

// --------------------------------------------------------------- server -----

function httpGet(path) {
  return new Promise((resolve, reject) => {
    const req = request({ host: '127.0.0.1', port: TEST_PORT, path, method: 'GET' }, (res) => {
      const chunks = [];
      res.on('data', (chunk) => chunks.push(chunk));
      res.on('end', () => resolve({
        status: res.statusCode,
        headers: res.headers,
        body: Buffer.concat(chunks),
      }));
    });
    req.on('error', reject);
    req.end();
  });
}

async function waitForServer(attempts = 50) {
  for (let i = 0; i < attempts; i++) {
    try {
      await httpGet('/api/status');
      return true;
    } catch {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  return false;
}

async function testServer() {
  section('HTTP-Oberfläche');

  const cacheDir = join(ROOT, TEST_CACHE);
  await rm(cacheDir, { recursive: true, force: true });
  await mkdir(join(cacheDir, 'thumbs'), { recursive: true });
  // Seeding the cache lets the HTTP surface be tested without reaching
  // Instagram - which is exactly what a repeat run does for real, too.
  await writeFile(join(cacheDir, 'embeds.json'), JSON.stringify({
    CACHED0001: {
      state: 'public', reason: 'ok', username: 'someone', isVideo: false,
      caption: 'Hallo', width: 1080, height: 1080, hasThumb: true, checkedAt: Date.now(),
    },
    CACHED0002: {
      state: 'unavailable', reason: 'private-or-deleted', hasThumb: false, checkedAt: Date.now(),
    },
    EXPIRED001: {
      state: 'public', reason: 'ok', hasThumb: false,
      checkedAt: Date.now() - 30 * 24 * 60 * 60 * 1000,
    },
  }));
  await writeFile(join(cacheDir, 'thumbs', 'CACHED0001.jpg'), Buffer.from([0xff, 0xd8, 0xff, 0xd9]));

  const server = spawn(process.execPath, ['server.js'], {
    cwd: ROOT,
    env: { ...process.env, WL_PORT: String(TEST_PORT), WL_CACHE_DIR: TEST_CACHE, WL_TIMEOUT_MS: '2000' },
    stdio: 'ignore',
  });

  try {
    check(await waitForServer(), 'Server startet');

    const status = await httpGet('/api/status');
    equal(status.status, 200, '/api/status antwortet');
    equal(JSON.parse(status.body).cached, 3, 'Cache wurde geladen');

    const cached = JSON.parse((await httpGet('/api/resolve?code=CACHED0001')).body);
    equal(cached.state, 'public', 'zwischengespeicherte Antwort wird ausgeliefert');
    equal(cached.cached, true, 'und als solche gekennzeichnet');
    equal(cached.username, 'someone', 'mit Benutzername');

    const gone = JSON.parse((await httpGet('/api/resolve?code=CACHED0002')).body);
    equal(gone.state, 'unavailable', 'nicht abrufbarer Beitrag bleibt nicht abrufbar');

    const bad = await httpGet('/api/resolve?code=' + encodeURIComponent('../../etc/passwd'));
    equal(bad.status, 400, 'ungültiger Shortcode wird abgewiesen');

    const thumb = await httpGet('/api/thumb?code=CACHED0001');
    equal(thumb.status, 200, 'Vorschaubild wird ausgeliefert');
    equal(thumb.headers['content-type'], 'image/jpeg', 'als JPEG');
    const missingThumb = await httpGet('/api/thumb?code=CACHED0002');
    equal(missingThumb.status, 404, 'fehlendes Vorschaubild ergibt 404');
    equal((await httpGet('/api/thumb?code=..%2f..%2fserver.js')).status, 400,
      'Pfad-Trick im Shortcode wird abgewiesen');

    const page = await httpGet('/');
    equal(page.status, 200, 'Startseite wird ausgeliefert');
    check(page.headers['content-type'].includes('charset=utf-8'), 'als UTF-8 ausgezeichnet');
    check(page.body.toString('utf8').includes('Wer hat das'), 'und enthält die Überschrift');
    check(page.body.toString('utf8').includes('Beiträge'), 'Umlaute kommen unverfälscht an');

    equal((await httpGet('/js/app.js')).status, 200, 'JavaScript wird ausgeliefert');
    equal((await httpGet('/../server.js')).status, 404, 'Ausbruch aus public/ scheitert');
    equal((await httpGet('/..%2f..%2fserver.js')).status, 404, 'kodierter Ausbruch scheitert');
    equal((await httpGet('/gibtsnicht.html')).status, 404, 'unbekannte Datei ergibt 404');
    equal((await httpGet('/api/quatsch')).status, 404, 'unbekannter Endpunkt ergibt 404');
  } finally {
    server.kill('SIGKILL');
    await rm(cacheDir, { recursive: true, force: true });
  }
}

// ----------------------------------------------------------------- main -----

async function main() {
  await testZip();
  await testLikes();
  testEmbed();
  testGame();
  await testServer();

  console.log(`\n${checks - failures} / ${checks} Prüfungen bestanden.`);
  if (failures) {
    console.error(`${failures} fehlgeschlagen.`);
    process.exitCode = 1;
  }
}

await main();
