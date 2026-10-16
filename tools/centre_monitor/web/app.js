// Centre monitor: polls /api/state (ETag, 304 when unchanged), re-renders only the parts that changed,
// and shows the selected piece on a large board with the tail of its output.

import { createDetailBoard, installSprite, miniBoardSvg, splitMoves } from './board.js';
import { createLineChart } from './charts.js';

const POLL_MS = 4500;
const TICK_MS = 1000;
const STALE_AFTER_S = 600; // the driver writes at least once per history point (5 minutes)
const TAIL_LINES = 80;
const ROOT = '1:60';
const SELECTION_KEY = 'centre.selected';
const THEME_KEY = 'centre.theme';

const RED_STATUSES = ['held', 'blue-wins', 'running', 'deferred', 'error', 'pending'];
const BLUE_STATUSES = ['blue-wins', 'held', 'running', 'deferred', 'blue-loses', 'error', 'pending'];
const STATUS_LABEL = {
  red: { held: 'held', 'blue-wins': 'blue wins', running: 'running', deferred: 'deferred', error: 'error', pending: 'pending' },
  blue: {
    'blue-wins': 'blue wins', held: 'held', running: 'running', deferred: 'deferred',
    'blue-loses': 'blue loses', error: 'error', pending: 'pending',
  },
};
const VERDICT_WORD = { open: 'Open', draw: 'Draw', 'red-wins': 'Red wins', 'blue-wins': 'Blue wins' };
const ICONS = {
  sun: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="4" fill="none" stroke="currentColor" stroke-width="2"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>',
  moon: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/></svg>',
};

const $ = (id) => document.getElementById(id);
const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const esc = (v) => String(v ?? '').replace(/[&<>"']/g, (c) => ESCAPES[c]);
const arr = (v) => (Array.isArray(v) ? v : []);
const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const statusOf = (item, allowed) => (allowed.includes(item?.status) ? item.status : 'pending');

// ------------------------------------------------------------------ formatting

function fmtDuration(seconds) {
  if (seconds === null || !Number.isFinite(seconds)) return '–';
  const s = Math.max(0, Math.round(seconds));
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${String(m).padStart(2, '0')}m`;
  if (m) return `${m}m ${String(s % 60).padStart(2, '0')}s`;
  return `${s}s`;
}

const fmtThreadHours = (seconds) => (num(seconds) === null ? '–' : (seconds / 3600).toFixed(seconds < 36000 ? 2 : 1));
const fmtInt = (v) => (num(v) === null ? '–' : Math.round(v).toLocaleString('en-US'));

function fmtClock(ms, withDay = false) {
  const d = new Date(ms);
  const hm = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  return withDay ? `${d.toLocaleDateString('en-US', { weekday: 'short' })} ${hm}` : hm;
}

const isToday = (ms) => new Date(ms).toDateString() === new Date().toDateString();

// ------------------------------------------------------------------ store

let state = null;
let etag = null;
let waiting = false;
let pollFailed = false;
let selectedId = null;
try { selectedId = localStorage.getItem(SELECTION_KEY); } catch { /* storage unavailable */ }
const seen = new Map(); // section -> signature last rendered

function changed(section, value) {
  const sig = typeof value === 'string' ? value : JSON.stringify(value ?? null);
  if (seen.get(section) === sig) return false;
  seen.set(section, sig);
  return true;
}

let pollTimer = null;
let polling = false;

function schedulePoll(delay) {
  clearTimeout(pollTimer);
  pollTimer = setTimeout(poll, delay);
}

/** One state request; a hidden tab skips it and polls again as soon as it is shown. */
async function poll() {
  if (polling) return;
  if (document.hidden) {
    schedulePoll(POLL_MS);
    return;
  }
  polling = true;
  try {
    const headers = etag ? { 'If-None-Match': etag } : {};
    const response = await fetch('api/state', { headers, cache: 'no-store' });
    pollFailed = false;
    if (response.status === 304) return;
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    etag = response.headers.get('ETag');
    if (data?.waiting) {
      waiting = true;
      $('waiting-text').innerHTML = `No <code>state.json</code> in <code>${esc(data.runDir ?? 'the run directory')}</code> yet. The page checks again every few seconds.`;
      return;
    }
    waiting = false;
    state = data && typeof data === 'object' ? data : {};
    derive();
    renderAll();
  } catch (err) {
    pollFailed = true;
    console.warn('state poll failed', err);
  } finally {
    polling = false;
    renderShell();
    schedulePoll(POLL_MS);
  }
}

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) schedulePoll(0);
});

// ------------------------------------------------------------------ derived data

/** The state's lists, cleaned once per new state, with lookups by piece id. */
let derived = null;

function derive() {
  const isItem = (x) => x && typeof x === 'object' && typeof x.id === 'string';
  const red = arr(state?.red?.lines).filter(isItem);
  const blue = arr(state?.blue?.replies).filter(isItem);
  const running = arr(state?.running).filter(isItem);
  derived = {
    red, blue, running,
    line: new Map(red.map((l) => [l.id, l])),
    reply: new Map(blue.map((r) => [r.id, r])),
    piece: new Map(running.map((p) => [p.id, p])),
  };
}

const redLines = () => derived?.red ?? [];
const blueReplies = () => derived?.blue ?? [];
const runningPieces = () => derived?.running ?? [];
const lineById = (id) => derived?.line.get(id);
const replyById = (id) => derived?.reply.get(id);
const runningById = (id) => derived?.piece.get(id);

function counts(items, statuses) {
  const out = Object.fromEntries(statuses.map((s) => [s, 0]));
  for (const item of items) out[statusOf(item, statuses)] += 1;
  return out;
}

function blueB1() {
  return state?.blue?.b1 || '4B:16,27,28,39';
}

/** Moves of a piece for the boards: [{move, side, kind}], plies alternate red/blue from the root. */
function pieceMoves(id) {
  const running = runningById(id);
  if (running && running.moves) {
    const moves = splitMoves(running.moves);
    return { moves, trying: true, running };
  }
  if (id?.startsWith('R:')) {
    const line = lineById(id);
    if (!line) return null;
    const moves = [ROOT, line.b1];
    if (line.hold) moves.push(line.hold);
    else if (line.current) return { moves: [...moves, line.current], trying: true, line };
    return { moves, trying: false, line };
  }
  if (id?.startsWith('B:')) {
    const reply = replyById(id);
    if (!reply) return null;
    const moves = [ROOT, blueB1(), reply.r2];
    if (reply.answer) moves.push(reply.answer);
    return { moves, trying: false, reply };
  }
  return null;
}

function boardSpec(moves, trying) {
  return moves.map((move, ply) => ({
    move, side: ply % 2, kind: ply === moves.length - 1 && ply > 1 ? (trying ? 'trying' : 'last') : '',
  }));
}

// ------------------------------------------------------------------ shell: connection, header, theme

function renderShell() {
  const run = state?.run ?? {};
  const connection = $('connection');
  const text = connection.querySelector('.connection-text');
  const updated = Date.parse(run.updated);
  const stale = !waiting && state && ['running', 'starting'].includes(run.status)
    && Number.isFinite(updated) && (Date.now() - updated) / 1000 > STALE_AFTER_S;
  connection.className = 'connection';
  if (pollFailed) {
    text.textContent = 'Offline';
    connection.title = 'The monitor server does not answer';
  } else if (waiting) {
    connection.classList.add('waiting');
    text.textContent = 'Waiting';
    connection.title = 'No state.json yet';
  } else if (stale) {
    connection.classList.add('stale');
    text.textContent = 'Stale';
    connection.title = 'The run has not written its state for a while';
  } else {
    connection.classList.add('online');
    text.textContent = 'Live';
    connection.title = `Updated ${Number.isFinite(updated) ? fmtClock(updated) : ''}`;
  }
  $('waiting').hidden = !waiting || Boolean(state);
  $('dashboard').hidden = !state;
  const staleEl = $('stale');
  staleEl.hidden = !stale;
  if (stale) staleEl.textContent = `No update from the driver for ${fmtDuration((Date.now() - updated) / 1000)}: is it still running?`;
}

function renderThemeButton() {
  const dark = document.documentElement.dataset.theme !== 'light';
  const button = $('theme-toggle');
  button.innerHTML = dark ? ICONS.sun : ICONS.moon;
  button.title = dark ? 'Switch to light theme' : 'Switch to dark theme';
}

$('theme-toggle').addEventListener('click', () => {
  const theme = document.documentElement.dataset.theme === 'light' ? 'dark' : 'light';
  document.documentElement.dataset.theme = theme;
  try { localStorage.setItem(THEME_KEY, theme); } catch { /* storage unavailable */ }
  renderThemeButton();
});

// ------------------------------------------------------------------ summary

function renderSummary() {
  const run = state.run ?? {};
  const summary = state.summary ?? {};
  if (changed('summary', [run, summary])) {
    const verdict = VERDICT_WORD[summary.verdict] ? summary.verdict : 'open';
    const word = $('verdict-word');
    word.textContent = VERDICT_WORD[verdict];
    word.className = `verdict-word ${verdict}`;
    const status = $('run-status');
    status.textContent = run.status || 'unknown';
    status.className = `status-badge ${esc(run.status || '')}`;
    $('verdict-text').textContent = summary.verdictText || '';
    const sha = typeof run.binarySha256 === 'string' ? run.binarySha256.slice(0, 12) : '';
    $('run-meta').innerHTML = [
      run.name ? `<span>run <b>${esc(run.name)}</b></span>` : '',
      `<span>root <code>${esc(run.root || ROOT)}</code></span>`,
      run.binary ? `<span title="${esc(run.binarySha256 || '')}">binary <code>${esc(run.binary)}</code>${sha ? ` · ${esc(sha)}` : ''}</span>` : '',
      Number.isFinite(Date.parse(run.started)) ? `<span>started ${esc(fmtClock(Date.parse(run.started), true))}</span>` : '',
    ].join('');
    const chip = $('run-chip');
    chip.hidden = false;
    chip.innerHTML = `<span>${esc(run.root || ROOT)}</span><span class="chip-sep">·</span><span>${esc(run.status || '…')}</span>`
      + `<span class="chip-detail"><span class="chip-sep"> · </span>${fmtInt(summary.piecesDone)} / ${fmtInt(summary.piecesTotal)}</span>`;
    document.title = `${verdict === 'open' ? `${fmtInt(summary.piecesDone)}/${fmtInt(summary.piecesTotal)}` : VERDICT_WORD[verdict]} · Centre Solve`;
  }
  renderStats();
}

/** The figures that tick with the clock are re-rendered every second (cheap: five tiles). */
function renderStats() {
  if (!state) return;
  const run = state.run ?? {};
  const summary = state.summary ?? {};
  const started = Date.parse(run.started);
  const updated = Date.parse(run.updated);
  const live = ['running', 'starting', 'stopping'].includes(run.status);
  const end = live ? Date.now() : (Number.isFinite(updated) ? updated : Date.now());
  const elapsed = Number.isFinite(started) ? (end - started) / 1000 : null;
  const done = num(summary.piecesDone);
  const total = num(summary.piecesTotal);
  const eta = num(summary.etaSeconds);
  // the driver's estimate is as of its last write
  const finish = live && eta !== null ? (Number.isFinite(updated) ? updated : Date.now()) + eta * 1000 : null;
  const threads = num(run.threads);
  const busy = num(run.busyThreads);
  const tiles = [
    ['Elapsed', fmtDuration(elapsed), Number.isFinite(started) ? `since ${fmtClock(started, !isToday(started))}` : ''],
    ['Thread-hours', fmtThreadHours(num(summary.threadSeconds)), 'all pieces, incl. imported'],
    ['Pieces', done === null ? '–' : `${fmtInt(done)}`, total ? `of ${fmtInt(total)} · ${((100 * (done ?? 0)) / total).toFixed(1)}%` : ''],
    ['ETA', finish === null ? '–' : fmtDuration((finish - Date.now()) / 1000), finish === null ? (run.status || '') : `≈ ${fmtClock(finish, true)}`],
    ['Busy threads', busy === null ? '–' : `${busy}${threads ? ` / ${threads}` : ''}`, `${runningPieces().length} pieces running`],
  ];
  const sig = JSON.stringify(tiles);
  if (!changed('stats', sig)) return;
  $('stats').innerHTML = tiles.map(([label, value, sub]) => `<div class="stat"><div class="stat-label">${esc(label)}</div>`
    + `<div class="stat-value">${esc(value)}</div><div class="stat-sub">${esc(sub)}</div></div>`).join('');
}

function segbar(half, items, statuses) {
  const c = counts(items, statuses);
  const total = items.length || 1;
  const segs = statuses.filter((s) => c[s] && s !== 'pending')
    .map((s) => `<span class="st-${s} ${half}" style="width:${((100 * c[s]) / total).toFixed(3)}%" title="${STATUS_LABEL[half][s]}: ${c[s]}"></span>`)
    .join('');
  const legend = statuses.filter((s) => c[s] || s === 'pending' || s === statuses[0])
    .map((s) => `<span class="legend-item"><span class="key st-${s} ${half}"></span>${STATUS_LABEL[half][s]} <b>${c[s]}</b></span>`)
    .join('');
  return { c, segs, legend };
}

function renderProgress() {
  const red = redLines();
  const blue = blueReplies();
  if (changed('progress', [red.map((l) => l.status), blue.map((r) => r.status)])) {
    const r = segbar('red', red, RED_STATUSES);
    const settledRed = r.c.held + r.c['blue-wins'];
    $('red-progress').innerHTML = `<div class="half-progress-head"><span><span class="red-ink">Red's half</span> · a hold for each blue reply</span>`
      + `<b>${settledRed} / ${red.length}</b></div><div class="segbar">${r.segs}</div><div class="legend">${r.legend}</div>`;
    const b = segbar('blue', blue, BLUE_STATUSES);
    const settledBlue = b.c.held + b.c['blue-wins'] + b.c['blue-loses'];
    $('blue-progress').innerHTML = `<div class="half-progress-head"><span><span class="blue-ink">Blue's half</span> · an answer to each red reply</span>`
      + `<b>${settledBlue} / ${blue.length}</b></div><div class="segbar">${b.segs}</div><div class="legend">${b.legend}</div>`;
    $('red-sub').textContent = `${r.c.held} held · ${r.c.running} running · ${r.c.pending} pending${r.c['blue-wins'] ? ` · ${r.c['blue-wins']} BLUE WINS` : ''}`;
    $('blue-sub').textContent = `${b.c['blue-wins'] + b.c.held} answered · ${b.c.running} running · ${b.c.pending} pending${b.c['blue-loses'] ? ` · ${b.c['blue-loses']} BLUE LOSES` : ''}`;
    renderAlarms(red, blue);
  }
}

/** Results that break a half of the proof, and pieces that failed, stay in view until they change. */
function renderAlarms(red, blue) {
  const breaks = [
    ...red.filter((l) => l.status === 'blue-wins').map((l) => [l.id, `blue's ${l.b1} wins: no red reply holds`]),
    ...blue.filter((r) => r.status === 'blue-loses').map((r) => [r.id, `red's ${r.r2} wins: no blue answer holds`]),
  ];
  const errors = [...red, ...blue].filter((x) => x.status === 'error').map((x) => [x.id, `${x.id} failed`]);
  const el = $('alarm');
  el.hidden = breaks.length + errors.length === 0;
  const chips = (items) => items.map(([id, text]) => `<button type="button" class="alarm-chip" data-id="${esc(id)}">${esc(text)}</button>`).join('');
  el.innerHTML = (breaks.length ? `<span class="alarm-title">Breaks the proof</span>${chips(breaks)}` : '')
    + (errors.length ? `<span class="alarm-title">Errors</span>${chips(errors)}` : '');
}

$('alarm').addEventListener('click', (e) => {
  const chip = e.target.closest('[data-id]');
  if (chip) select(chip.dataset.id, true);
});

// ------------------------------------------------------------------ running now

const runningEls = new Map();

function renderRunning() {
  const pieces = runningPieces();
  $('running-pulse').hidden = pieces.length === 0;
  const ids = pieces.map((p) => p.id).join('\n');
  const list = $('running');
  if (changed('running-ids', ids)) {
    runningEls.clear();
    list.innerHTML = '';
    for (const p of pieces) {
      const el = document.createElement('button');
      el.type = 'button';
      el.dataset.id = p.id;
      el.className = `run-card ${p.half === 'blue' ? 'blue' : 'red'}`;
      list.appendChild(el);
      runningEls.set(p.id, { el, sig: null });
    }
    if (!pieces.length) list.innerHTML = '<p class="empty-note">Nothing running.</p>';
  }
  const threads = pieces.reduce((n, p) => n + (num(p.threads) ?? 0), 0);
  $('running-sub').textContent = pieces.length ? `${pieces.length} pieces · ${threads} threads` : '';
  for (const p of pieces) {
    const entry = runningEls.get(p.id);
    if (!entry) continue;
    const sig = JSON.stringify(p);
    entry.el.classList.toggle('selected', p.id === currentSelection());
    if (entry.sig === sig) continue;
    entry.sig = sig;
    const moves = splitMoves(p.moves);
    const done = num(p.done) ?? 0;
    const total = num(p.total) ?? 0;
    const pct = total ? Math.min(100, (100 * done) / total) : 0;
    const label = p.id.replace(/^[RB]:/, '');
    entry.el.innerHTML = `${miniBoardSvg(boardSpec(moves, true))}
      <div class="run-title"><span class="${p.half === 'blue' ? 'blue-ink' : 'red-ink'}">${p.half === 'blue' ? 'B' : 'R'}</span><span>${esc(label)}</span></div>
      <div class="run-line"><span>${esc(p.phase || '')}${p.detail ? ` · ${esc(p.detail)}` : ''}</span></div>
      <div class="bar" title="${done} of ${total}"><span style="width:${pct.toFixed(1)}%"></span></div>
      <div class="run-line full"><span><b>${fmtInt(done)}</b> / ${fmtInt(total)}</span>`
      + `<span><b>${num(p.threads) ?? '–'}</b> thr · <b data-started="${esc(p.started || '')}">${elapsedSince(p.started)}</b></span></div>`;
  }
}

function elapsedSince(iso) {
  const t = Date.parse(iso);
  return Number.isFinite(t) ? fmtDuration((Date.now() - t) / 1000) : '–';
}

$('running').addEventListener('click', (e) => {
  const card = e.target.closest('[data-id]');
  if (card) select(card.dataset.id, true);
});

// ------------------------------------------------------------------ red tiles

const redEls = new Map();

function redOrder(lines) {
  return lines.map((l, i) => ({ l, i }))
    .sort((a, b) => (num(b.l.rating) ?? -1) - (num(a.l.rating) ?? -1) || a.i - b.i)
    .map((x) => x.l);
}

function renderRed() {
  const lines = redOrder(redLines());
  $('red-goal').textContent = state.red?.goal || '';
  const running = new Map(runningPieces().map((p) => [p.id, p]));
  if (changed('red-ids', lines.map((l) => l.id).join('\n'))) {
    redEls.clear();
    const grid = $('red-grid');
    grid.innerHTML = '';
    for (const line of lines) {
      const el = document.createElement('div');
      el.dataset.id = line.id;
      el.tabIndex = 0;
      el.setAttribute('role', 'button');
      grid.appendChild(el);
      redEls.set(line.id, { el, sig: null });
    }
    if (!lines.length) grid.innerHTML = '<p class="empty-note">No lines yet.</p>';
  }
  const selected = currentSelection();
  for (const line of lines) {
    const entry = redEls.get(line.id);
    if (!entry) continue;
    const piece = running.get(line.id);
    const status = statusOf(line, RED_STATUSES);
    const sig = [status, line.hold, line.holdRank, line.tried, line.current, piece?.moves].join('|');
    entry.el.classList.toggle('selected', line.id === selected);
    if (entry.sig === sig) continue;
    entry.sig = sig;
    entry.el.className = `tile st-${status}${line.id === selected ? ' selected' : ''}`;
    entry.el.setAttribute('aria-label', `${line.b1}: ${STATUS_LABEL.red[status]}`);
    const spec = pieceMoves(line.id);
    const tag = status === 'held' ? `#${line.holdRank ?? '?'}`
      : status === 'running' ? `${(num(line.tried) ?? 0) + 1}`
        : status === 'blue-wins' ? '!' : '';
    entry.el.innerHTML = miniBoardSvg(boardSpec(spec?.moves ?? [ROOT, line.b1], spec?.trying))
      + (tag ? `<span class="tile-tag">${esc(tag)}</span>` : '');
  }
}

// ------------------------------------------------------------------ blue heat grid

const blueEls = new Map();

function renderBlue() {
  const blue = state.blue ?? {};
  const replies = blueReplies().slice().sort((a, b) => (num(a.rank) ?? 1e9) - (num(b.rank) ?? 1e9));
  $('blue-goal').textContent = blue.goal || '';
  if (changed('b1-history', [blue.b1, blue.b1History])) {
    const hist = arr(blue.b1History);
    $('b1-history').innerHTML = (hist.length ? hist : [{ b1: blueB1(), status: 'active' }])
      .map((h) => `<span class="b1-chip ${esc(h?.status || '')}" title="${esc(h?.note || '')}"><span class="muted">${esc(h?.status || '')}</span><span class="mono">${esc(h?.b1 || '')}</span></span>`)
      .join('');
  }
  const grid = $('blue-grid');
  if (changed('blue-ids', replies.map((r) => r.id).join('\n'))) {
    blueEls.clear();
    grid.innerHTML = '';
    const frag = document.createDocumentFragment();
    for (const reply of replies) {
      const el = document.createElement('span');
      el.dataset.id = reply.id;
      frag.appendChild(el);
      blueEls.set(reply.id, { el, status: null });
    }
    grid.appendChild(frag);
    $('heat-axis-end').textContent = replies.length ? `weakest (rank ${replies.length})` : '';
    if (!replies.length) grid.innerHTML = '<p class="empty-note">No replies yet.</p>';
  }
  const selected = currentSelection();
  for (const reply of replies) {
    const entry = blueEls.get(reply.id);
    if (!entry) continue;
    const status = statusOf(reply, BLUE_STATUSES);
    const cls = `heat st-${status} blue${reply.id === selected ? ' selected' : ''}`;
    if (entry.status !== cls) {
      entry.status = cls;
      entry.el.className = cls;
    }
  }
}

// ------------------------------------------------------------------ tooltip

const tooltip = $('tooltip');

function tooltipHtml(id) {
  const spec = pieceMoves(id);
  if (!spec) return '';
  const rows = [];
  let title = '';
  if (id.startsWith('R:')) {
    const line = spec.line ?? lineById(id) ?? {};
    const status = statusOf(line, RED_STATUSES);
    title = `blue ${line.b1}`;
    rows.push(['status', STATUS_LABEL.red[status]]);
    if (num(line.rating) !== null) rows.push(['rating', `${(100 * line.rating).toFixed(0)}% for blue`]);
    if (line.hold) rows.push(['hold', `${line.hold}${line.holdRank ? ` (rank ${line.holdRank})` : ''}`]);
    rows.push(['tried', `${num(line.tried) ?? 0} red replies`]);
    if (line.current && !line.hold) rows.push(['trying', line.current]);
    if (num(line.threadSeconds)) rows.push(['cost', `${fmtThreadHours(line.threadSeconds)} thread-hours`]);
  } else {
    const reply = spec.reply ?? replyById(id) ?? {};
    const status = statusOf(reply, BLUE_STATUSES);
    title = `red ${reply.r2}`;
    rows.push(['rank', `${reply.rank ?? '?'} of ${blueReplies().length}`]);
    rows.push(['status', STATUS_LABEL.blue[status] + (reply.sweep ? ' (full sweep)' : '')]);
    if (reply.answer) rows.push(['answer', reply.answer]);
    if (num(reply.threadSeconds)) rows.push(['cost', `${fmtThreadHours(reply.threadSeconds)} thread-hours`]);
  }
  if (spec.running) rows.push(['now', `${spec.running.phase || ''} ${fmtInt(spec.running.done)}/${fmtInt(spec.running.total)}`]);
  return `${miniBoardSvg(boardSpec(spec.moves, spec.trying))}<div class="tip-title">${esc(title)}</div>`
    + `<div class="tip-rows">${rows.map(([k, v]) => `<span>${esc(k)}</span><span>${esc(v)}</span>`).join('')}</div>`;
}

function placeTooltip(e) {
  const pad = 14;
  const w = tooltip.offsetWidth;
  const h = tooltip.offsetHeight;
  let x = e.clientX + pad;
  let y = e.clientY + pad;
  if (x + w > window.innerWidth - 4) x = e.clientX - pad - w;
  if (y + h > window.innerHeight - 4) y = e.clientY - pad - h;
  tooltip.style.left = `${Math.max(4, x)}px`;
  tooltip.style.top = `${Math.max(4, y)}px`;
}

let tipId = null;
for (const gridId of ['red-grid', 'blue-grid']) {
  const grid = $(gridId);
  grid.addEventListener('pointermove', (e) => {
    const target = e.target.closest('[data-id]');
    if (!target) {
      tooltip.hidden = true;
      tipId = null;
      return;
    }
    if (target.dataset.id !== tipId) {
      tipId = target.dataset.id;
      tooltip.innerHTML = tooltipHtml(tipId);
      tooltip.hidden = !tooltip.innerHTML;
    }
    placeTooltip(e);
  });
  grid.addEventListener('pointerleave', () => {
    tooltip.hidden = true;
    tipId = null;
  });
  grid.addEventListener('click', (e) => {
    const target = e.target.closest('[data-id]');
    if (target) select(target.dataset.id, true);
  });
  grid.addEventListener('keydown', (e) => {
    const target = e.target.closest('[data-id]');
    if (target && (e.key === 'Enter' || e.key === ' ')) {
      e.preventDefault();
      select(target.dataset.id, true);
    }
  });
}

// ------------------------------------------------------------------ detail

let detailBoard = null;
let tailFile = null;
let tailBusy = false;

/** The explicit selection while it exists, otherwise the first running piece. */
function currentSelection() {
  if (selectedId && pieceMoves(selectedId)) return selectedId;
  return runningPieces()[0]?.id ?? null;
}

function select(id, scroll) {
  selectedId = id;
  try { localStorage.setItem(SELECTION_KEY, id); } catch { /* storage unavailable */ }
  seen.delete('detail');
  renderRed();
  renderBlue();
  renderRunning();
  renderDetail(true);
  if (scroll && window.matchMedia('(max-width: 1180px)').matches) {
    $('detail').scrollIntoView({ behavior: 'smooth', block: 'start' });
  }
}

const ROLE = ['red: the centre', 'blue reply', "red's reply", "blue's answer", 'red', 'blue'];

function renderDetail(force = false) {
  const id = currentSelection();
  const spec = id ? pieceMoves(id) : null;
  const line = id?.startsWith('R:') ? lineById(id) ?? null : null;
  const reply = id?.startsWith('B:') ? replyById(id) ?? null : null;
  const running = runningById(id) ?? null;
  if (!changed('detail', [id, spec?.moves, line, reply, running]) && !force) return;
  $('detail-empty').hidden = Boolean(spec);
  $('detail-body').hidden = !spec;
  const statusEl = $('detail-status');
  if (!spec) {
    statusEl.hidden = true;
    $('detail-head').textContent = 'Detail';
    $('tail').hidden = true;
    tailFile = null;
    return;
  }
  if (!detailBoard) detailBoard = createDetailBoard($('detail-board'));
  detailBoard.show(spec.moves, { trying: spec.trying });
  const half = id.startsWith('R:') ? 'red' : 'blue';
  const status = half === 'red' ? statusOf(line, RED_STATUSES) : statusOf(reply, BLUE_STATUSES);
  $('detail-head').innerHTML = half === 'red' ? '<span class="red-ink">Red\'s half</span> · line' : '<span class="blue-ink">Blue\'s half</span> · reply';
  statusEl.hidden = false;
  statusEl.textContent = STATUS_LABEL[half][status];
  statusEl.className = `status-badge ${status === 'running' ? 'running' : status === 'held' || (half === 'blue' && status === 'blue-wins') ? 'done' : status === 'blue-wins' || status === 'blue-loses' || status === 'error' ? 'error' : 'stopped'}`;
  $('detail-title').textContent = spec.moves.join(' ');
  const roleOf = (ply) => {
    if (ply === spec.moves.length - 1 && spec.trying && ply > 1) return 'being settled';
    if (half === 'red' && ply === 2 && line?.hold) return `red's hold${line.holdRank ? ` (rank ${line.holdRank})` : ''}`;
    if (half === 'blue' && ply === 1) return "blue's reply b1";
    if (half === 'blue' && ply === 2) return `red's reply${reply?.rank ? ` (rank ${reply.rank})` : ''}`;
    if (half === 'blue' && ply === 3 && reply?.answer) return "blue's answer";
    return ROLE[ply] ?? (ply % 2 ? 'blue' : 'red');
  };
  $('detail-moves').innerHTML = spec.moves.map((m, ply) => `<li class="${ply === spec.moves.length - 1 ? 'last' : ''}">`
    + `<span class="ply">${ply + 1}</span><span class="mv ${ply % 2 ? 'blue' : 'red'}">${esc(m)}</span><span class="role">${esc(roleOf(ply))}</span></li>`).join('');
  const kv = [];
  const item = line ?? reply ?? {};
  if (line && num(line.rating) !== null) kv.push(['engine rating', `${(100 * line.rating).toFixed(1)}% for blue`]);
  if (line) kv.push(['red replies settled', `${num(line.tried) ?? 0}`]);
  if (reply) kv.push(['rank', `${reply.rank ?? '?'} of ${blueReplies().length}`]);
  if (reply?.sweep) kv.push(['answer from', 'the full sweep']);
  if (running) {
    kv.push(['phase', `${running.phase || '–'}${running.detail ? ` · ${running.detail}` : ''}`]);
    kv.push(['progress', `${fmtInt(running.done)} / ${fmtInt(running.total)} on ${num(running.threads) ?? '–'} threads, ${elapsedSince(running.started)}`]);
  }
  if (num(item.threadSeconds)) kv.push(['cost', `${fmtThreadHours(item.threadSeconds)} thread-hours`]);
  if (item.source) kv.push(['source', item.source]);
  $('detail-kv').innerHTML = kv.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join('');
  const out = running?.out || item.out || null;
  if (out !== tailFile) {
    tailFile = out;
    $('tail-text').textContent = '';
  }
  $('tail').hidden = !out;
  if (out && (force || running || !$('tail-text').textContent)) loadTail();
}

async function loadTail() {
  if (!tailFile || tailBusy) return;
  tailBusy = true;
  const file = tailFile;
  try {
    const response = await fetch(`api/tail?file=${encodeURIComponent(file)}&lines=${TAIL_LINES}`, { cache: 'no-store' });
    const data = await response.json().catch(() => ({}));
    if (file !== tailFile) return;
    const pre = $('tail-text');
    const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 4;
    $('tail-file').textContent = file;
    if (response.status === 503 && pre.textContent) {
      // the file is being replaced: keep what is shown until the next poll
    } else if (!response.ok) {
      pre.textContent = data?.error || `HTTP ${response.status}`;
      $('tail-size').textContent = '';
    } else {
      pre.textContent = arr(data.lines).join('\n') || '(empty)';
      $('tail-size').textContent = num(data.size) !== null ? `${(data.size / 1024).toFixed(1)} KB` : '';
      if (atBottom) pre.scrollTop = pre.scrollHeight;
    }
  } catch (err) {
    console.warn('tail failed', err);
  } finally {
    tailBusy = false;
  }
}

// ------------------------------------------------------------------ charts and events

const charts = {
  pieces: createLineChart({ title: 'Pieces settled', value: (p) => p.piecesDone, format: fmtInt }),
  threadHours: createLineChart({
    title: 'Thread-hours',
    value: (p) => (num(p.threadSeconds) === null ? NaN : p.threadSeconds / 3600),
    format: (v) => `${v.toFixed(1)} th`,
    axisFormat: (v) => (v >= 100 ? v.toFixed(0) : v.toFixed(1)),
  }),
};
$('charts').append(charts.pieces.el, charts.threadHours.el);

function renderHistory() {
  const history = arr(state.history);
  if (!changed('history', `${history.length}|${JSON.stringify(history[history.length - 1] ?? null)}|${JSON.stringify(history[0] ?? null)}`)) return;
  charts.pieces.update(history);
  charts.threadHours.update(history);
}

function renderEvents() {
  const events = arr(state.events);
  if (!changed('events', `${events.length}|${JSON.stringify(events[events.length - 1] ?? null)}`)) return;
  $('events-sub').textContent = events.length ? `last ${events.length}` : '';
  $('events').innerHTML = events.slice().reverse().map((ev) => {
    const t = Date.parse(ev?.t);
    const time = Number.isFinite(t) ? fmtClock(t, !isToday(t)) : '';
    const id = ev?.id && pieceMoves(ev.id) ? ev.id : '';
    return `<li${id ? ` data-id="${esc(id)}" title="Show ${esc(id)}"` : ''}><span class="ev-time">${esc(time)}</span>`
      + `<span class="ev-kind ${esc(ev?.kind || '')} ${esc(ev?.half || '')}">${esc(ev?.kind || '')}</span>`
      + `<span class="ev-text">${esc(ev?.text || '')}</span></li>`;
  }).join('') || '<li><span></span><span></span><span class="muted">No events yet.</span></li>';
}

$('events').addEventListener('click', (e) => {
  const li = e.target.closest('[data-id]');
  if (li) select(li.dataset.id, true);
});

// ------------------------------------------------------------------ main loop

function renderAll() {
  if (!state) return;
  renderSummary();
  renderProgress();
  renderRunning();
  renderRed();
  renderBlue();
  renderDetail();
  renderHistory();
  renderEvents();
}

function tick() {
  if (state && !document.hidden) {
    renderStats();
    for (const el of document.querySelectorAll('[data-started]')) el.textContent = elapsedSince(el.dataset.started);
    renderShell();
  }
}

installSprite();
renderThemeButton();
renderShell();
poll();
setInterval(tick, TICK_MS);
