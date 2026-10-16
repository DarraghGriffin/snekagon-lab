// Hex boards: geometry as in ui/index.html and src/core.hpp (cell = (r+5)*11 + (q+5), axial q, r in -5..5
// with |q+r| <= 5, pointy-top hexes), a shared empty-board sprite for the many mini boards, and the large
// detail board.

const S3 = Math.sqrt(3);
const SIZE = 10;
const NS = 'http://www.w3.org/2000/svg';
export const CENTRE = 60;

export const CELLS = [];
for (let i = 0; i < 121; i++) {
  const q = (i % 11) - 5;
  const r = Math.floor(i / 11) - 5;
  if (Math.abs(q + r) <= 5) CELLS.push(i);
}

export function cellXY(i) {
  const q = (i % 11) - 5;
  const r = Math.floor(i / 11) - 5;
  return [SIZE * S3 * (q + r / 2), SIZE * 1.5 * r];
}

function hexPoints(x, y, s) {
  const pts = [];
  for (let k = 0; k < 6; k++) {
    const a = (Math.PI / 180) * (60 * k - 30);
    pts.push(`${(x + s * Math.cos(a)).toFixed(2)},${(y + s * Math.sin(a)).toFixed(2)}`);
  }
  return pts.join(' ');
}

const HALF_W = SIZE * S3 * 5.5 + 2;
const HALF_H = SIZE * 1.5 * 5 + SIZE + 2;
export const VIEWBOX = `${-HALF_W} ${-HALF_H} ${2 * HALF_W} ${2 * HALF_H}`;

/** Cells of a move in CLI notation ('4B:16,27,28,39', 'pass' or junk gives none). */
export function moveCells(move) {
  const m = /^[^:\s]+:([\d,]+)$/.exec(String(move ?? '').trim());
  if (!m) return [];
  return m[1].split(',').map(Number).filter((c) => Number.isInteger(c) && c >= 0 && c < 121);
}

/** A move list string ('1:60 4B:16,27,28,39 ...') as an array of moves. */
export function splitMoves(moves) {
  return String(moves ?? '').trim().split(/\s+/).filter(Boolean);
}

/** One hidden SVG holding the empty board; mini boards reference it with <use>. */
export function installSprite() {
  if (document.getElementById('board-sprite')) return;
  const polys = CELLS.map((i) => {
    const [x, y] = cellXY(i);
    return `<polygon points="${hexPoints(x, y, SIZE - 0.9)}"/>`;
  }).join('');
  const holder = document.createElement('div');
  holder.innerHTML = `<svg id="board-sprite" width="0" height="0" style="position:absolute" aria-hidden="true">
    <defs><g id="board-empty" class="cells-empty">${polys}</g></defs></svg>`;
  document.body.prepend(holder.firstElementChild);
}

/**
 * Markup of a mini board: the shared empty board plus the given moves. `moves` is a list of
 * {move, side: 0|1, kind: ''|'last'|'trying'}.
 */
export function miniBoardSvg(moves, cls = 'mini-board') {
  let body = '';
  for (const { move, side, kind } of moves) {
    for (const c of moveCells(move)) {
      const [x, y] = cellXY(c);
      body += `<polygon class="pc p${side}${kind ? ' ' + kind : ''}" points="${hexPoints(x, y, SIZE - 0.9)}"/>`;
    }
  }
  return `<svg class="${cls}" viewBox="${VIEWBOX}" aria-hidden="true"><use href="#board-empty"/>${body}</svg>`;
}

/** The large board: every cell its own polygon, with ply numbers on the pieces. */
export function createDetailBoard(container) {
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', VIEWBOX);
  svg.setAttribute('class', 'detail-board');
  svg.setAttribute('role', 'img');
  const backdrop = document.createElementNS(NS, 'polygon');
  backdrop.setAttribute('points', hexPoints(0, 0, SIZE * 10.25));
  backdrop.setAttribute('transform', 'rotate(30)');
  backdrop.setAttribute('class', 'board-backdrop');
  svg.appendChild(backdrop);
  const polys = new Map();
  const labels = new Map();
  for (const i of CELLS) {
    const [x, y] = cellXY(i);
    const p = document.createElementNS(NS, 'polygon');
    p.setAttribute('points', hexPoints(x, y, SIZE - 0.7));
    p.setAttribute('class', 'cell');
    const title = document.createElementNS(NS, 'title');
    title.textContent = `cell ${i}`;
    p.appendChild(title);
    svg.appendChild(p);
    polys.set(i, p);
    const t = document.createElementNS(NS, 'text');
    t.setAttribute('x', x.toFixed(2));
    t.setAttribute('y', y.toFixed(2));
    t.setAttribute('class', 'cell-label');
    svg.appendChild(t);
    labels.set(i, t);
  }
  container.appendChild(svg);

  /** `moves`: array of move strings, ply 0 is red's. `trying` marks the last move as a candidate. */
  function show(moves, { trying = false, label = '' } = {}) {
    svg.setAttribute('aria-label', label || moves.join(' '));
    const owner = new Map();
    moves.forEach((move, ply) => {
      for (const c of moveCells(move)) owner.set(c, ply);
    });
    const last = moves.length - 1;
    for (const i of CELLS) {
      const ply = owner.get(i);
      const p = polys.get(i);
      const t = labels.get(i);
      if (ply === undefined) {
        p.setAttribute('class', 'cell');
        t.textContent = '';
        continue;
      }
      const kind = ply === last ? (trying ? ' trying' : ' last') : '';
      p.setAttribute('class', `cell pc p${ply % 2}${kind}`);
      // the ply number on the piece's first cell, as in the move list
      t.textContent = moveCells(moves[ply])[0] === i ? String(ply + 1) : '';
      t.setAttribute('class', `cell-label on-piece${kind}`);
    }
  }

  return { el: svg, show };
}
