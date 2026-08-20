// Game state for "Who liked?".
//
// Kept free of the DOM on purpose: everything in here is data in, data out, so
// the round building and the scoring can be tested in Node without a browser.

/** Player colours, in the order they get handed out. */
export const PLAYER_COLORS = [
  '#e1306c', '#3fa7ff', '#2fbf71', '#f9a03f',
  '#a06cf5', '#ff6b9a', '#39d0c8', '#f2d024',
];

export const DEFAULT_ROUND_COUNT = 15;

function shuffle(items, rng) {
  const copy = [...items];
  for (let i = copy.length - 1; i > 0; i--) {
    const j = Math.floor(rng() * (i + 1));
    [copy[i], copy[j]] = [copy[j], copy[i]];
  }
  return copy;
}

/**
 * Merges every player's likes into one pool and keeps only the posts the server
 * confirmed as publicly viewable.
 *
 * A post two people liked collapses into a single entry with two likers - the
 * same post must not come up twice in one game.
 *
 * @param {Array<{id: string, posts: Array}>} players
 * @param {Map<string, {state: string}>} resolutions
 */
export function buildPool(players, resolutions) {
  const byCode = new Map();
  const hidden = { unavailable: 0, unknown: 0 };
  const countedHidden = new Set();

  for (const player of players) {
    for (const post of player.posts) {
      const record = resolutions.get(post.code);
      const state = record?.state ?? 'unknown';

      if (state !== 'public') {
        // Count each hidden post once, however many players liked it.
        if (!countedHidden.has(post.code)) {
          countedHidden.add(post.code);
          hidden[state === 'unavailable' ? 'unavailable' : 'unknown']++;
        }
        continue;
      }

      const existing = byCode.get(post.code);
      if (existing) {
        if (!existing.likedBy.includes(player.id)) existing.likedBy.push(player.id);
        continue;
      }
      byCode.set(post.code, {
        code: post.code,
        post,
        // The server's username beats the export's: it comes from the live post,
        // while the export can be months old.
        owner: record.username || post.owner || '',
        isVideo: Boolean(record.isVideo),
        caption: record.caption || post.caption || '',
        likedBy: [player.id],
      });
    }
  }

  return { entries: [...byCode.values()], hidden };
}

/**
 * Picks the rounds and whose turn it is.
 *
 * Posts liked by exactly one player come first - those have one clean answer.
 * Only when there are not enough of them do posts with several likers get used,
 * and there naming any one of the likers counts.
 *
 * The player whose turn it is is *not* excluded from their own likes. With two
 * players that exclusion would make every answer "the other one", and with more
 * players remembering whether a post is your own is half the game.
 */
export function buildRounds(entries, players, { count = DEFAULT_ROUND_COUNT, rng = Math.random } = {}) {
  const single = shuffle(entries.filter((entry) => entry.likedBy.length === 1), rng);
  const shared = shuffle(entries.filter((entry) => entry.likedBy.length > 1), rng);
  const chosen = [...single, ...shared].slice(0, count);

  return chosen.map((entry, index) => ({
    ...entry,
    guesserId: players[index % players.length].id,
  }));
}

/** True when the guess names one of the players who actually liked the post. */
export function isCorrect(round, playerId) {
  return round.likedBy.includes(playerId);
}

export function createGame(players, rounds) {
  return {
    players,
    rounds,
    index: 0,
    scores: Object.fromEntries(players.map((player) => [player.id, 0])),
    answered: false,
    lastGuess: null,
  };
}

export function currentRound(game) {
  return game.rounds[game.index] ?? null;
}

/** Records a guess and awards the point. Ignores a second guess in a round. */
export function submitGuess(game, playerId) {
  const round = currentRound(game);
  if (!round || game.answered) return null;
  const correct = isCorrect(round, playerId);
  if (correct) game.scores[round.guesserId]++;
  game.answered = true;
  game.lastGuess = { playerId, correct };
  return { correct, likedBy: round.likedBy };
}

/** Moves on. Returns false when the last round is done. */
export function advance(game) {
  game.index++;
  game.answered = false;
  game.lastGuess = null;
  return game.index < game.rounds.length;
}

/** Drops the current round without scoring, e.g. when an embed will not load. */
export function skipRound(game) {
  game.rounds.splice(game.index, 1);
  game.answered = false;
  game.lastGuess = null;
  return game.index < game.rounds.length;
}

export function ranking(game) {
  return game.players
    .map((player) => ({ player, score: game.scores[player.id] ?? 0 }))
    .sort((a, b) => b.score - a.score || a.player.name.localeCompare(b.player.name, 'de'));
}
