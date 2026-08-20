// Wiring between the screens, the ZIP import and the game state.

import { readZipIndex, extractEntryAsText, ZipError } from './zip.js';
import { parseLikedPosts, findLikedPostsEntry, guessAccountName } from './likes.js';
import { checkServer, resolveAll, thumbnailUrl, embedSrc } from './api.js';
import {
  PLAYER_COLORS, DEFAULT_ROUND_COUNT, buildPool, buildRounds,
  createGame, currentRound, submitGuess, advance, skipRound, ranking,
} from './game.js';

const $ = (id) => document.getElementById(id);

const state = {
  players: [],
  resolutions: new Map(),
  pool: null,
  game: null,
  galleryFilter: 'all',
  checkAbort: null,
};

let nextPlayerId = 1;

// ------------------------------------------------------------- screens -----

const SCREENS = ['lobby', 'check', 'game', 'gallery', 'result'];

let currentScreen = 'lobby';

// Also runs after an import, not just on a screen change: adding a player makes
// the bar appear, and its entries must not offer a screen that does not exist
// yet.
function updateNav() {
  $('topnav').hidden = state.players.length === 0;
  const reachable = { lobby: true, game: Boolean(state.game), gallery: Boolean(state.pool) };
  for (const link of document.querySelectorAll('.navlink')) {
    link.setAttribute('aria-current', String(link.dataset.goto === currentScreen));
    link.disabled = !reachable[link.dataset.goto];
  }
}

function show(name) {
  currentScreen = name;
  for (const screen of SCREENS) {
    $(`screen-${screen}`).hidden = screen !== name;
  }
  updateNav();
  window.scrollTo({ top: 0, behavior: 'instant' });
}

// -------------------------------------------------------------- import -----

function setImportStatus(message, isError = false) {
  const element = $('importStatus');
  element.textContent = message;
  element.classList.toggle('error', isError);
}

async function importZip(file) {
  setImportStatus(`„${file.name}“ wird gelesen …`);

  let index;
  try {
    index = await readZipIndex(file);
  } catch (error) {
    const hint = error instanceof ZipError ? error.message : 'Die Datei ließ sich nicht öffnen.';
    setImportStatus(`„${file.name}“: ${hint}`, true);
    return;
  }

  const entry = findLikedPostsEntry(index.entries);
  if (!entry) {
    setImportStatus(
      `„${file.name}“ enthält keine likes/liked_posts.json. Wurde der Export im ` +
      'Format JSON (nicht HTML) angefordert?',
      true,
    );
    return;
  }

  let parsed;
  try {
    // This is the only entry that gets inflated. Every other file in the archive
    // stays compressed and unread.
    parsed = parseLikedPosts(await extractEntryAsText(file, entry));
  } catch (error) {
    setImportStatus(`„${file.name}“: ${error.message || 'Entpacken fehlgeschlagen.'}`, true);
    return;
  }

  if (!parsed.posts.length) {
    setImportStatus(`„${file.name}“ enthält keine gelikten Beiträge.`, true);
    return;
  }

  const player = {
    id: `p${nextPlayerId++}`,
    name: guessAccountName(index.entries) || `Spieler ${state.players.length + 1}`,
    color: PLAYER_COLORS[state.players.length % PLAYER_COLORS.length],
    posts: parsed.posts,
    fileName: file.name,
    skippedFiles: index.entries.length - 1,
  };
  state.players.push(player);

  setImportStatus(
    `${parsed.posts.length} Likes übernommen. Gelesen wurde genau eine Datei ` +
    `(${entry.name.split('/').slice(-2).join('/')}); ${player.skippedFiles} weitere ` +
    'Dateien im Archiv wurden nicht entpackt.',
  );
  renderPlayers();
}

async function importFiles(files) {
  for (const file of files) {
    if (!/\.zip$/i.test(file.name)) {
      setImportStatus(`„${file.name}“ ist kein ZIP-Archiv.`, true);
      continue;
    }
    await importZip(file);
  }
}

function renderPlayers() {
  const list = $('playerList');
  list.replaceChildren();

  for (const player of state.players) {
    const item = document.createElement('li');
    item.className = 'player';
    item.style.setProperty('--player-color', player.color);

    const name = document.createElement('input');
    name.className = 'player-name';
    name.value = player.name;
    name.setAttribute('aria-label', 'Name des Spielers');
    name.addEventListener('input', () => { player.name = name.value.trim() || 'Ohne Namen'; });

    const meta = document.createElement('span');
    meta.className = 'player-meta';
    meta.textContent = `${player.posts.length} Likes`;

    const remove = document.createElement('button');
    remove.className = 'player-remove';
    remove.type = 'button';
    remove.title = 'Spieler entfernen';
    remove.textContent = '✕';
    remove.addEventListener('click', () => {
      state.players = state.players.filter((other) => other.id !== player.id);
      renderPlayers();
    });

    item.append(name, meta, remove);
    list.append(item);
  }

  $('startCheck').disabled = state.players.length === 0;
  $('startCheck').textContent = state.players.length < 2
    ? 'Beiträge prüfen (Galerie)'
    : 'Beiträge prüfen';
  updateNav();
}

// --------------------------------------------------------------- check -----

function uniqueCodes() {
  const codes = new Set();
  for (const player of state.players) {
    for (const post of player.posts) codes.add(post.code);
  }
  return [...codes];
}

async function runCheck() {
  show('check');
  const codes = uniqueCodes();
  const tally = { public: 0, unavailable: 0, unknown: 0 };

  $('progressFill').style.width = '0%';
  $('progressLabel').textContent = `0 / ${codes.length}`;
  $('checkWarning').hidden = true;
  $('toGame').hidden = true;
  $('toGallery').hidden = true;
  $('cancelCheck').hidden = false;
  for (const [key, id] of [['public', 'tallyPublic'], ['unavailable', 'tallyPrivate'], ['unknown', 'tallyUnknown']]) {
    $(id).textContent = '0';
    tally[key] = 0;
  }

  const server = await checkServer();
  if (!server.available) {
    $('cancelCheck').hidden = true;
    $('checkWarning').hidden = false;
    $('checkWarning').textContent =
      'Der Prüf-Server antwortet nicht. Die Seite muss über „node server.js“ ' +
      'laufen – direkt aus dem Dateisystem geöffnet kann der Browser nicht ' +
      'feststellen, welche Beiträge öffentlich sind.';
    return;
  }

  const abort = new AbortController();
  state.checkAbort = abort;
  state.resolutions = await resolveAll(codes, {
    signal: abort.signal,
    onResult(record, done, total) {
      tally[record.state === 'public' ? 'public' : record.state === 'unavailable' ? 'unavailable' : 'unknown']++;
      $('tallyPublic').textContent = tally.public;
      $('tallyPrivate').textContent = tally.unavailable;
      $('tallyUnknown').textContent = tally.unknown;
      $('progressFill').style.width = `${(done / total) * 100}%`;
      $('progressLabel').textContent = `${done} / ${total}`;
    },
  });

  state.checkAbort = null;
  $('cancelCheck').hidden = true;
  // Cancelled runs leave a half-resolved pool behind; that must not turn into a
  // half-filled gallery on the screen the user just walked away from.
  if (abort.signal.aborted) return;
  finishCheck(tally);
}

function finishCheck(tally) {
  state.pool = buildPool(state.players, state.resolutions);
  const playable = state.pool.entries.length;

  if (tally.unknown > 0) {
    $('checkWarning').hidden = false;
    $('checkWarning').textContent =
      (tally.unknown === 1
        ? 'Ein Beitrag ließ sich nicht prüfen'
        : `${tally.unknown} Beiträge ließen sich nicht prüfen`) +
      ' – meist, weil Instagram die Anfragen vorübergehend bremst oder keine ' +
      'Internetverbindung besteht. Sie bleiben draußen, statt als leere Kachel im ' +
      'Spiel zu landen. Ein späterer erneuter Durchlauf holt sie nach.';
  }

  if (!playable) {
    $('checkWarning').hidden = false;
    $('checkWarning').textContent =
      'Kein einziger Beitrag ist öffentlich abrufbar – damit gibt es nichts zu zeigen.';
    return;
  }

  $('toGallery').hidden = false;
  $('toGame').hidden = state.players.length < 2;
  $('progressLabel').textContent =
    `Fertig: ${playable} spielbare Beiträge von ${uniqueCodes().length}.`;
}

// ---------------------------------------------------------------- game -----

function startGame() {
  const rounds = buildRounds(state.pool.entries, state.players, { count: DEFAULT_ROUND_COUNT });
  if (!rounds.length) return;
  state.game = createGame(state.players, rounds);
  show('game');
  renderRound();
}

function playerById(id) {
  return state.players.find((player) => player.id === id);
}

function renderScoreboard() {
  const board = $('scoreboard');
  board.replaceChildren();
  const round = currentRound(state.game);
  for (const player of state.players) {
    const chip = document.createElement('div');
    chip.className = 'score-chip';
    chip.style.setProperty('--player-color', player.color);
    if (round && round.guesserId === player.id) chip.classList.add('active');
    const dot = document.createElement('span');
    dot.className = 'dot';
    const label = document.createElement('span');
    label.textContent = player.name;
    const score = document.createElement('b');
    score.textContent = state.game.scores[player.id] ?? 0;
    chip.append(dot, label, score);
    board.append(chip);
  }
}

// How long to wait for Instagram's embed before falling back to the preview.
const EMBED_TIMEOUT_MS = 8000;

function renderEmbed(container, entry) {
  container.replaceChildren();
  container.classList.add('post-frame');

  // The cached preview goes in first, so the card is never a white void while
  // the embed loads - and stays as the card if the embed never arrives.
  const placeholder = document.createElement('img');
  placeholder.className = 'post-placeholder';
  placeholder.alt = entry.owner ? `Beitrag von @${entry.owner}` : 'Beitrag';
  placeholder.src = thumbnailUrl(entry.code);
  placeholder.addEventListener('error', () => placeholder.remove());
  container.append(placeholder);

  const loading = document.createElement('p');
  loading.className = 'post-loading';
  loading.textContent = 'Beitrag wird geladen …';
  container.append(loading);

  const frame = document.createElement('iframe');
  frame.src = embedSrc(entry.code);
  frame.height = 640;
  frame.setAttribute('scrolling', 'no');
  frame.setAttribute('allowtransparency', 'true');
  frame.setAttribute('allow', 'autoplay; clipboard-write; encrypted-media; picture-in-picture');
  container.append(frame);

  // An iframe pointing at a blocked host fires 'load' for its own error page or
  // never fires at all, so neither event says anything. Instagram's own size
  // report is the only reliable sign that the post really rendered.
  const note = document.createElement('p');
  note.className = 'post-fallback';
  note.hidden = true;
  const noteText = document.createElement('span');
  noteText.textContent = 'Vorschau – der Beitrag lässt sich hier gerade nicht einbetten. ';
  const noteLink = document.createElement('a');
  noteLink.href = `https://www.instagram.com/p/${encodeURIComponent(entry.code)}/`;
  noteLink.target = '_blank';
  noteLink.rel = 'noopener noreferrer';
  noteLink.textContent = 'Bei Instagram öffnen';
  note.append(noteText, noteLink);
  container.append(note);

  const timer = setTimeout(() => {
    // The round may be long over: replaceChildren() detaches the old frame but
    // cannot cancel this timer, and the note would land on the next post.
    if (!frame.isConnected) return;
    loading.remove();
    // Hidden rather than removed: a merely slow embed still gets to arrive, and
    // then swaps places with the note.
    frame.hidden = true;
    note.hidden = false;
  }, EMBED_TIMEOUT_MS);

  frame.addEventListener('embed-alive', () => {
    clearTimeout(timer);
    frame.hidden = false;
    loading.remove();
    note.remove();
    placeholder.remove();
  }, { once: true });

  return frame;
}

function renderRound() {
  const round = currentRound(state.game);
  if (!round) {
    showResult();
    return;
  }

  renderScoreboard();
  $('roundCounter').textContent = `Runde ${state.game.index + 1} von ${state.game.rounds.length}`;
  $('roundTurn').textContent = `${playerById(round.guesserId).name} ist dran`;

  renderEmbed($('postFrame'), round);

  $('guessArea').hidden = false;
  $('revealArea').hidden = true;

  const buttons = $('guessButtons');
  buttons.replaceChildren();
  for (const player of state.players) {
    const button = document.createElement('button');
    button.className = 'guess-button';
    button.type = 'button';
    button.style.setProperty('--player-color', player.color);
    button.textContent = player.name;
    button.addEventListener('click', () => onGuess(player.id, button));
    buttons.append(button);
  }
}

function onGuess(playerId, button) {
  const round = currentRound(state.game);
  const result = submitGuess(state.game, playerId);
  if (!result) return;

  for (const other of $('guessButtons').querySelectorAll('button')) other.disabled = true;
  button.classList.add(result.correct ? 'is-right' : 'is-wrong');

  const likers = round.likedBy.map((id) => playerById(id)?.name ?? '?');
  $('revealVerdict').textContent = result.correct ? 'Richtig!' : 'Daneben.';
  $('revealVerdict').className = `reveal-verdict ${result.correct ? 'right' : 'wrong'}`;
  $('revealDetail').textContent = likers.length === 1
    ? `Geliked hat ${likers[0]}${round.owner ? ` – Beitrag von @${round.owner}` : ''}.`
    : `Geliked haben ${likers.join(', ')}${round.owner ? ` – Beitrag von @${round.owner}` : ''}.`;

  renderScoreboard();
  $('revealArea').hidden = false;
}

function showResult() {
  const list = $('resultList');
  list.replaceChildren();
  for (const { player, score } of ranking(state.game)) {
    const item = document.createElement('li');
    item.style.setProperty('--player-color', player.color);
    const name = document.createElement('span');
    name.textContent = `${player.name}: `;
    const points = document.createElement('b');
    points.textContent = `${score} ${score === 1 ? 'Punkt' : 'Punkte'}`;
    item.append(name, points);
    list.append(item);
  }
  show('result');
}

// ------------------------------------------------------------- gallery -----

function renderGalleryFilters() {
  const filters = $('galleryFilters');
  filters.replaceChildren();

  const options = [{ id: 'all', name: 'Alle', color: 'var(--accent)' }, ...state.players];
  for (const option of options) {
    const chip = document.createElement('button');
    chip.className = 'filter-chip';
    chip.type = 'button';
    chip.style.setProperty('--player-color', option.color);
    chip.textContent = option.name;
    chip.setAttribute('aria-pressed', String(state.galleryFilter === option.id));
    chip.addEventListener('click', () => {
      state.galleryFilter = option.id;
      renderGallery();
    });
    filters.append(chip);
  }
}

function renderGallery() {
  renderGalleryFilters();
  const grid = $('galleryGrid');
  grid.replaceChildren();

  const entries = state.pool.entries.filter((entry) =>
    state.galleryFilter === 'all' || entry.likedBy.includes(state.galleryFilter));

  for (const entry of entries) {
    const tile = document.createElement('button');
    tile.className = 'tile';
    tile.type = 'button';
    tile.title = entry.owner ? `@${entry.owner}` : entry.code;

    const image = document.createElement('img');
    image.loading = 'lazy';
    image.alt = entry.owner ? `Beitrag von @${entry.owner}` : 'Beitrag';
    image.src = thumbnailUrl(entry.code);
    // No cached preview: show the shortcode rather than a broken image icon.
    image.addEventListener('error', () => {
      const fallback = document.createElement('span');
      fallback.className = 'tile-fallback';
      fallback.textContent = entry.owner ? `@${entry.owner}` : entry.code;
      image.replaceWith(fallback);
    });
    tile.append(image);

    if (entry.isVideo) {
      const badge = document.createElement('span');
      badge.className = 'tile-badge';
      badge.textContent = '▶';
      tile.append(badge);
    }

    const dots = document.createElement('span');
    dots.className = 'tile-likers';
    for (const id of entry.likedBy) {
      const dot = document.createElement('i');
      dot.style.background = playerById(id)?.color ?? '#fff';
      dots.append(dot);
    }
    tile.append(dots);

    if (entry.owner) {
      const owner = document.createElement('span');
      owner.className = 'tile-owner';
      owner.textContent = `@${entry.owner}`;
      tile.append(owner);
    }

    tile.addEventListener('click', () => openLightbox(entry));
    grid.append(tile);
  }

  const { unavailable, unknown } = state.pool.hidden;
  const parts = [`${entries.length} öffentliche Beiträge`];
  if (unavailable) parts.push(`${unavailable} nicht mehr öffentlich abrufbar (ausgeblendet)`);
  if (unknown) parts.push(`${unknown} nicht prüfbar (ausgeblendet)`);
  $('galleryNote').textContent = parts.join(' · ');
}

function openLightbox(entry) {
  $('lightbox').hidden = false;
  renderEmbed($('lightboxBody'), entry);
}

function closeLightbox() {
  $('lightbox').hidden = true;
  $('lightboxBody').replaceChildren();
}

// Instagram's embed reports its rendered height to the parent window. Without
// it every post sits in a fixed-height box and is either cropped or floating in
// white space.
window.addEventListener('message', (event) => {
  if (!/(^|\.)instagram\.com$/.test(new URL(event.origin).hostname)) return;
  let payload = event.data;
  if (typeof payload === 'string') {
    try {
      payload = JSON.parse(payload);
    } catch {
      return;
    }
  }
  const height = payload?.type === 'MEASURE' ? Number(payload.details?.height) : 0;
  if (!height) return;
  for (const frame of document.querySelectorAll('iframe')) {
    if (frame.contentWindow !== event.source) continue;
    frame.height = Math.ceil(height);
    frame.dispatchEvent(new CustomEvent('embed-alive'));
  }
});

// ---------------------------------------------------------------- wiring ---

function wire() {
  const dropzone = $('dropzone');
  const input = $('fileInput');

  dropzone.addEventListener('click', () => input.click());
  dropzone.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      input.click();
    }
  });
  input.addEventListener('change', async () => {
    await importFiles([...input.files]);
    input.value = '';
  });

  for (const type of ['dragenter', 'dragover']) {
    dropzone.addEventListener(type, (event) => {
      event.preventDefault();
      dropzone.classList.add('dragover');
    });
  }
  for (const type of ['dragleave', 'drop']) {
    dropzone.addEventListener(type, () => dropzone.classList.remove('dragover'));
  }
  dropzone.addEventListener('drop', async (event) => {
    event.preventDefault();
    await importFiles([...event.dataTransfer.files]);
  });

  $('startCheck').addEventListener('click', runCheck);
  $('cancelCheck').addEventListener('click', () => {
    state.checkAbort?.abort();
    show('lobby');
  });
  $('toGame').addEventListener('click', startGame);
  $('toGallery').addEventListener('click', () => {
    renderGallery();
    show('gallery');
  });

  $('nextRound').addEventListener('click', () => {
    if (advance(state.game)) renderRound();
    else showResult();
  });
  $('skipRound').addEventListener('click', () => {
    if (skipRound(state.game)) renderRound();
    else showResult();
  });

  $('playAgain').addEventListener('click', startGame);
  $('resultGallery').addEventListener('click', () => {
    renderGallery();
    show('gallery');
  });

  $('lightboxClose').addEventListener('click', closeLightbox);
  $('lightbox').addEventListener('click', (event) => {
    if (event.target === $('lightbox')) closeLightbox();
  });
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && !$('lightbox').hidden) closeLightbox();
  });

  for (const link of document.querySelectorAll('.navlink')) {
    link.addEventListener('click', () => {
      const target = link.dataset.goto;
      if (target === 'lobby') show('lobby');
      if (target === 'game' && state.game) show('game');
      if (target === 'gallery' && state.pool) {
        renderGallery();
        show('gallery');
      }
    });
  }
}

wire();
renderPlayers();
show('lobby');
