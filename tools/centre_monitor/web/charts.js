// Time-series line chart (SVG, no libraries): one series over the run's history, a recessive grid,
// a crosshair with a readout on hover, and the latest value in the head.

const MARGIN = { top: 8, right: 8, bottom: 18, left: 40 };
const Y_TICKS = 4;
const X_TICKS = 5;
const NICE_STEPS = [1, 2, 2.5, 5, 10];

function niceMax(value) {
  if (!(value > 0)) return 1;
  const magnitude = 10 ** Math.floor(Math.log10(value / Y_TICKS));
  const step = NICE_STEPS.map((s) => s * magnitude).find((s) => s * Y_TICKS >= value) ?? 10 * magnitude;
  return step * Y_TICKS;
}

const DAY_LABEL_SPAN_MS = 20 * 3600 * 1000;

function clockLabel(ms, withDay = false) {
  const d = new Date(ms);
  const hm = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  return withDay ? `${d.toLocaleDateString('en-US', { weekday: 'short' })} ${hm}` : hm;
}

/**
 * `value(point)` reads the series from a history point, `format(v)` labels values.
 * Returns {el, update(history)}.
 */
export function createLineChart({ title, value, format, axisFormat = format }) {
  const el = document.createElement('figure');
  el.className = 'chart';
  el.innerHTML = `<figcaption class="chart-head"><span class="chart-title"></span><span class="chart-now"></span></figcaption>
    <div class="chart-plot" role="img"><svg class="chart-svg" aria-hidden="true"></svg><div class="chart-tip" hidden></div></div>`;
  el.querySelector('.chart-title').textContent = title;
  const now = el.querySelector('.chart-now');
  const plot = el.querySelector('.chart-plot');
  const svg = el.querySelector('.chart-svg');
  const tip = el.querySelector('.chart-tip');
  let points = [];
  let geo = null;

  new ResizeObserver(() => draw()).observe(plot);
  plot.addEventListener('pointermove', (e) => hover(e.clientX));
  plot.addEventListener('pointerleave', () => hover(null));

  function update(history) {
    points = (history ?? [])
      .map((p) => ({ t: Date.parse(p?.t), v: Number(value(p)) }))
      .filter((p) => Number.isFinite(p.t) && Number.isFinite(p.v))
      .sort((a, b) => a.t - b.t);
    const last = points[points.length - 1];
    now.textContent = last ? format(last.v) : '';
    plot.setAttribute('aria-label', last ? `${title}: ${format(last.v)} at ${clockLabel(last.t)}` : `${title}: no data yet`);
    draw();
  }

  function draw() {
    const w = plot.clientWidth;
    const h = plot.clientHeight;
    if (!w || !h) return;
    svg.setAttribute('viewBox', `0 0 ${w} ${h}`);
    if (points.length === 0) {
      svg.innerHTML = `<text class="chart-axis" x="${w / 2}" y="${h / 2}" text-anchor="middle">no history yet</text>`;
      geo = null;
      return;
    }
    const t0 = points[0].t;
    const t1 = Math.max(points[points.length - 1].t, t0 + 60_000);
    const ymax = niceMax(Math.max(...points.map((p) => p.v)));
    const pw = w - MARGIN.left - MARGIN.right;
    const ph = h - MARGIN.top - MARGIN.bottom;
    const x = (t) => MARGIN.left + ((t - t0) / (t1 - t0)) * pw;
    const y = (v) => MARGIN.top + ph - (v / ymax) * ph;
    geo = { x, y, t0, t1, pw };

    let out = '';
    for (let k = 0; k <= Y_TICKS; k++) {
      const v = (ymax * k) / Y_TICKS;
      const yy = Math.round(y(v)) + 0.5;
      out += `<line class="${k ? 'chart-grid' : 'chart-base'}" x1="${MARGIN.left}" x2="${w - MARGIN.right}" y1="${yy}" y2="${yy}"/>`;
      out += `<text class="chart-axis" x="${MARGIN.left - 6}" y="${yy + 3}" text-anchor="end">${axisFormat(v)}</text>`;
    }
    const ticks = pw < 260 ? 2 : X_TICKS;
    for (let k = 0; k <= ticks; k++) {
      const t = t0 + ((t1 - t0) * k) / ticks;
      const anchor = k === 0 ? 'start' : k === ticks ? 'end' : 'middle';
      out += `<text class="chart-axis" x="${x(t)}" y="${h - 4}" text-anchor="${anchor}">${clockLabel(t, t1 - t0 > DAY_LABEL_SPAN_MS)}</text>`;
    }
    const line = points.map((p, i) => `${i ? 'L' : 'M'}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}`).join('');
    const base = y(0).toFixed(1);
    out += `<path class="chart-area" d="${line}L${x(points[points.length - 1].t).toFixed(1)},${base}L${x(points[0].t).toFixed(1)},${base}Z"/>`;
    out += `<path class="chart-line" d="${line}"/>`;
    out += `<line class="chart-cross" y1="${MARGIN.top}" y2="${MARGIN.top + ph}" visibility="hidden"/>`;
    out += `<circle class="chart-dot" r="4" visibility="hidden"/>`;
    svg.innerHTML = out;
  }

  function hover(clientX) {
    const cross = svg.querySelector('.chart-cross');
    const dot = svg.querySelector('.chart-dot');
    if (clientX === null || !geo || points.length === 0) {
      tip.hidden = true;
      cross?.setAttribute('visibility', 'hidden');
      dot?.setAttribute('visibility', 'hidden');
      return;
    }
    const rect = svg.getBoundingClientRect();
    const t = geo.t0 + ((clientX - rect.left - MARGIN.left) / geo.pw) * (geo.t1 - geo.t0);
    let best = points[0];
    for (const p of points) if (Math.abs(p.t - t) < Math.abs(best.t - t)) best = p;
    const px = geo.x(best.t);
    cross.setAttribute('x1', px);
    cross.setAttribute('x2', px);
    cross.setAttribute('visibility', 'visible');
    dot.setAttribute('cx', px);
    dot.setAttribute('cy', geo.y(best.v));
    dot.setAttribute('visibility', 'visible');
    tip.innerHTML = `<b>${format(best.v)}</b> <span class="muted">at ${clockLabel(best.t, true)}</span>`;
    tip.hidden = false;
    const width = plot.clientWidth;
    const tw = tip.offsetWidth;
    tip.style.left = `${px + 12 + tw > width ? Math.max(0, px - 12 - tw) : px + 12}px`;
  }

  return { el, update };
}
