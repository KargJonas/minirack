import {
  html,
  render,
  useRef,
  useState,
  useEffect,
  useCallback,
} from "./assets/preact-htm.js";

const WINDOW_S = 60;
const CAP = 1200;   // ring capacity, ~2x a 60 s window at 10 Hz

// Spectrum y-axis. The range is quantised to whole steps and only ever grows
// within an update, because an axis that retunes itself to each frame makes a
// steady noise floor look like it is moving.
const DB_STEP = 20;
const DB_MIN_SPAN = 60;
const DB_MAX_SPAN = 160;
const SPEC_LABEL_H = 12;   // gutter for the decade labels, below the trace

// The timeline canvas: series and spectrogram in one drawing, sharing an x.
// They are one canvas rather than two because the whole point is that a mark at
// some time in the series sits directly above the same time in the spectrogram,
// and two canvases would make that an agreement to maintain rather than a fact.
const SERIES_H = 88;
const TL_GAP = 8;
const SGRAM_H = 100;
const TIME_AXIS_H = 15;
const TL_H = SERIES_H + TL_GAP + SGRAM_H + TIME_AXIS_H;

// Colour scale. The floor is a low percentile of the window rather than "peak
// minus a fixed range": the noise floor is where most bins actually live, so
// anchoring the light end to it spends the whole ramp on the part that varies.
// Anchored to the peak instead, the floor sits mid-ramp and the picture washes
// out to a uniform mid-blue with the tones barely darker.
const SGRAM_FLOOR_PCT = 0.25;
const SGRAM_MIN_SPAN_DB = 25;
const SGRAM_MAX_SPAN_DB = 90;
const RAMP_N = 13;           // --spec-0 .. --spec-12

// ---------------------------------------------------------------------------
// Data store
//
// Sample blocks arrive at 10 Hz. They are kept out of component state on
// purpose: routing them through the App would re-render every card ten times a
// second, and re-rendering a card that is being edited makes Preact touch its
// <select>, which closes the native dropdown mid-interaction.
//
// So blocks land here and only the components that actually display live
// numbers subscribe. Everything else - the card shell, the config form - is
// re-rendered solely by its own state.
// ---------------------------------------------------------------------------

const store = {
  series: [],          // per channel: {t, mean, rms, min, max} parallel arrays
  last: [],            // per channel: the most recent block
  spec: [],            // per channel: the most recent {db, peak, f0, thd}
  specAxis: null,      // {f, resolution_hz, ...}, once per board session
  hist: [],            // per channel: {t: [], col: []}, col = Uint8Array of dB codes
  histMeta: null,      // {db_offset, dt, seconds, bins}
  status: null,
  chan: [],            // per channel: Set of callbacks
  specChan: [],        // per channel: Set of callbacks, spectra only
  statusSubs: new Set(),
};

const chanSubs = (i) => (store.chan[i] ||= new Set());
// Spectra arrive at 2 Hz and blocks at 10 Hz. Separate subscriptions so the
// spectrum canvas is not redrawn five times for every one it has new data for.
const specSubs = (i) => (store.specChan[i] ||= new Set());
const notify = (set) => set.forEach((fn) => fn());

function useChannelTick(idx, enabled = true) {
  const [, setTick] = useState(0);

  useEffect(() => {
    if (!enabled) return;
    const fn = () => setTick((v) => v + 1);
    const subs = chanSubs(idx);
    subs.add(fn);
    return () => subs.delete(fn);
  }, [idx, enabled]);
}

function useSpectrumTick(idx) {
  const [, setTick] = useState(0);

  useEffect(() => {
    const fn = () => setTick((v) => v + 1);
    const subs = specSubs(idx);
    subs.add(fn);
    return () => subs.delete(fn);
  }, [idx]);
}

function useChannelOk(idx) {
  const [ok, setOk] = useState(true);

  useEffect(() => {
    const fn = () => {
      const v = store.last[idx] ? !!store.last[idx].ok : true;
      setOk((prev) => (prev === v ? prev : v));
    };
    const subs = chanSubs(idx);
    subs.add(fn);
    return () => subs.delete(fn);
  }, [idx]);
  return ok;
}

function useStatus() {
  const [, setTick] = useState(0);

  useEffect(() => {
    const fn = () => setTick((v) => v + 1);
    store.statusSubs.add(fn);
    return () => store.statusSubs.delete(fn);
  }, []);

  return store.status;
}

function ingestBlock(m) {
  m.ch.forEach((b, i) => {
    // Created on demand rather than assumed: /api/meta and the event stream are
    // two independent requests, and the stream regularly wins. Requiring meta
    // to land first silently dropped the whole replayed history, leaving a full
    // spectrogram above a line that started at page load.
    const s = store.series[i] ||
      (store.series[i] = { t: [], mean: [], rms: [], min: [], max: [] });

    s.t.push(m.t); s.mean.push(b.mean); s.rms.push(b.rms);
    s.min.push(b.min); s.max.push(b.max);

    if (s.t.length > CAP) {
      for (const k of ["t", "mean", "rms", "min", "max"]) s[k].splice(0, s[k].length - CAP);
    }

    store.last[i] = b;
  });
}

function b64ToBytes(s) {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// A live spectrum is already a spectrogram column, so the page quantises the
// one it just received rather than the server sending it twice. This is why
// the spectrogram costs no extra bandwidth once the page is up.
function appendColumn(i, t, db) {
  const h = store.hist[i], meta = store.histMeta;
  if (!h || !meta) return;

  const q = new Uint8Array(db.length);
  for (let k = 0; k < db.length; k++) {
    q[k] = Math.max(0, Math.min(255, Math.round(db[k] - meta.db_offset)));
  }
  h.t.push(t);
  h.col.push(q);

  const cap = Math.ceil(meta.seconds / meta.dt) + 4;
  if (h.t.length > cap) {
    h.t.splice(0, h.t.length - cap);
    h.col.splice(0, h.col.length - cap);
  }
}

// Sequential ramp for the spectrogram, read from CSS so light and dark are each
// selected rather than one being an inversion of the other - --spec-0 is always
// the quiet end and recedes toward that mode's own surface.
let rampCache = { key: "", lut: null };

function rampLUT(cs) {
  const stops = [];
  for (let i = 0; i < RAMP_N; i++) stops.push(cs.getPropertyValue(`--spec-${i}`).trim());

  const key = stops.join(",");
  if (rampCache.key === key && rampCache.lut) return rampCache.lut;

  const rgb = stops.map((h) => [
    parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16),
  ]);
  const lut = new Uint8Array(256 * 3);
  for (let i = 0; i < 256; i++) {
    const p = (i / 255) * (RAMP_N - 1);
    const a = Math.min(RAMP_N - 2, Math.floor(p)), f = p - a;
    for (let k = 0; k < 3; k++) lut[i * 3 + k] = rgb[a][k] + (rgb[a + 1][k] - rgb[a][k]) * f;
  }
  rampCache = { key, lut };
  return lut;
}

// Which display bin each pixel row of the spectrogram shows. Frequency runs up
// the axis on a log scale, so this is a lookup rather than a scale factor, and
// it only changes when the axis or the height does.
function rowBins(f, h) {
  const n = f.length, lf0 = Math.log10(f[0]), lf1 = Math.log10(f[n - 1]);
  const rows = new Int32Array(h);
  for (let y = 0; y < h; y++) {
    const want = Math.pow(10, lf0 + (1 - y / (h - 1)) * (lf1 - lf0));
    let lo = 0, hi = n - 1;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (f[mid] < want) lo = mid + 1; else hi = mid; }
    if (lo > 0 && Math.abs(f[lo - 1] - want) < Math.abs(f[lo] - want)) lo--;
    rows[y] = lo;
  }
  return rows;
}

// The icons are inlined so they can easily be themed using currentColor.

const Icon = (vb, d, sw) => html`
  <svg viewBox=${vb} aria-hidden="true" fill="none" stroke="currentColor"
       stroke-linecap="round" stroke-linejoin="round" stroke-width=${sw}>
    <path d=${d} />
  </svg>`;

const ConfigIcon = () => Icon(
  "0 0 16 16",
  "M7.997 9.694a1.726 1.695 0 1 0 0-3.39a1.726 1.695 0 0 0 0 3.39m3.021-6.78l3.021 " +
  "5.085l-3.021 5.085H4.976L1.955 7.999l3.021-5.085z", 1.711);

const CheckIcon = () => Icon("0 0 24 24", "M5 13l4 4L19 7", 3);
const XIcon = () => Icon("0 0 24 24", "M18 6L6 18M6 6l12 12", 4);

function fmt(v) {
  if (v == null || !isFinite(v)) return "–";
  const a = Math.abs(v);
  if (a !== 0 && (a < 1e-3 || a >= 1e6)) return v.toExponential(2);
  return v.toFixed(a >= 100 ? 1 : a >= 1 ? 3 : 5);
}

function fmtScale(v) {
  if (v == null || !isFinite(v)) return "–";
  const a = Math.abs(v);
  if (a !== 0 && (a < 1e-3 || a >= 1e6)) return v.toExponential(4);
  return String(Number(v.toPrecision(6)));
}

async function postConfig(idx, config) {
  const r = await fetch("/api/config", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ idx, config }),
  });

  const body = await r.json().catch(() => ({}));
  return { ok: r.ok, status: r.status, body };
}

// The big, live number in each card
function Readout({ idx, unit }) {
  useChannelTick(idx);
  const b = store.last[idx];
  
  return html`
    <div class="readout">
      <div class="val">${fmt(b?.mean)}<span class="unit"> ${unit}</span></div>
      <div class="rms"><span>rms</span> ${fmt(b?.rms)}</div>
    </div>`;
}

function niceStep(span) {
  for (const s of [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600]) {
    if (span / s <= 6) return s;
  }
  return 3600;
}

function fmtAgo(sec) {
  if (sec < 60) return `${Math.round(sec)}s`;
  if (sec < 3600) return `${Math.round(sec / 60)}m`;
  return `${(sec / 3600).toFixed(sec % 3600 ? 1 : 0)}h`;
}

// Series and spectrogram, one canvas, one time axis.
//
// They share a drawing so that "the same x is the same instant" is structural,
// rather than two independently scaled plots that have to be kept in agreement.
// That also fixed something the series alone got away with: x used to be the
// sample *index*, which only equals time while nothing is missing. Under a
// dropout an index axis quietly closes the gap, and the spectrogram below it
// would have slid out of step with the line above exactly when you most wanted
// to read the two together.
function Timeline({ idx, unit }) {
  useChannelTick(idx);      // series, 10 Hz
  useSpectrumTick(idx);     // spectrogram columns, 2 Hz

  const canvasRef = useRef(null);
  const offRef = useRef(null);
  const rowsRef = useRef({ key: "", rows: null });
  const scaleRef = useRef({ key: "", topQ: 0, botQ: 0 });
  const [hover, setHover] = useState(null);
  const [tip, setTip] = useState(null);
  const [axis, setAxis] = useState({ lo: "", hi: "", top: "", bot: "" });

  useEffect(() => {
    const cv = canvasRef.current;
    const s = store.series[idx];
    if (!cv || !s) return;

    const cs = getComputedStyle(document.body);
    const dpr = window.devicePixelRatio || 1;
    const w = cv.clientWidth, h = cv.clientHeight;
    if (!w || !h) return;

    if (cv.width !== w * dpr || cv.height !== h * dpr) {
      cv.width = w * dpr; cv.height = h * dpr;
    }

    const g = cv.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);

    const hist = store.hist[idx], meta = store.histMeta;
    const ax = store.specAxis;

    // One clock for both plots. `now` is the newest thing either has, so a
    // spectrogram that updates at 2 Hz does not make the 10 Hz series appear
    // to lag behind it.
    let t1 = s.t.length ? s.t[s.t.length - 1] : 0;
    if (hist && hist.t.length) t1 = Math.max(t1, hist.t[hist.t.length - 1]);
    if (!t1) return;

    const t0 = t1 - WINDOW_S;
    const X = (t) => ((t - t0) / WINDOW_S) * w;
    const SG_Y = SERIES_H + TL_GAP;

    // ---------------- series ----------------
    let i0 = s.t.length;
    while (i0 > 0 && s.t[i0 - 1] >= t0) i0--;

    if (s.t.length - i0 >= 2) {
      let lo = Infinity, hi = -Infinity;
      for (let k = i0; k < s.t.length; k++) {
        if (s.min[k] < lo) lo = s.min[k];
        if (s.max[k] > hi) hi = s.max[k];
      }
      if (isFinite(lo) && isFinite(hi)) {
        if (hi - lo < 1e-12) { const c = (hi + lo) / 2 || 0; lo = c - 1e-6; hi = c + 1e-6; }
        const pad = (hi - lo) * 0.10; lo -= pad; hi += pad;
        const Y = (v) => SERIES_H - ((v - lo) / (hi - lo)) * SERIES_H;

        if (lo < 0 && hi > 0) {
          g.strokeStyle = cs.getPropertyValue("--grid").trim();
          g.lineWidth = 1;
          g.beginPath(); g.moveTo(0, Y(0) + .5); g.lineTo(w, Y(0) + .5); g.stroke();
        }

        g.fillStyle = cs.getPropertyValue("--series-band").trim();
        g.beginPath();
        for (let k = i0; k < s.t.length; k++) g.lineTo(X(s.t[k]), Y(s.max[k]));
        for (let k = s.t.length - 1; k >= i0; k--) g.lineTo(X(s.t[k]), Y(s.min[k]));
        g.closePath(); g.fill();

        g.strokeStyle = cs.getPropertyValue("--series").trim();
        g.lineWidth = 2; g.lineJoin = "round"; g.lineCap = "round";
        g.beginPath();
        for (let k = i0; k < s.t.length; k++) {
          const x = X(s.t[k]), y = Y(s.mean[k]);
          k === i0 ? g.moveTo(x, y) : g.lineTo(x, y);
        }
        g.stroke();

        const loS = fmt(lo), hiS = fmt(hi);
        setAxis((a) => (a.lo === loS && a.hi === hiS ? a : { ...a, lo: loS, hi: hiS }));
      }
    }

    // ---------------- spectrogram ----------------
    let topQ = 0, botQ = 0;
    if (hist && hist.t.length && meta && ax) {
      const nb = meta.bins;

      let j0 = hist.t.length;
      while (j0 > 0 && hist.t[j0 - 1] >= t0) j0--;

      // Values are already dB codes, so a 256-bin histogram is exact and the
      // percentile is a scan. Recomputed per column, not per frame - the series
      // ticks five times as often and the scale has not changed in between.
      const scaleKey = hist.t[hist.t.length - 1] + ":" + j0;
      if (scaleRef.current.key !== scaleKey) {
        const counts = new Int32Array(256);
        let total = 0;
        for (let j = j0; j < hist.col.length; j++) {
          const c = hist.col[j];
          for (let k = 0; k < nb; k++) { counts[c[k]]++; total++; }
        }
        let top = 255;
        while (top > 0 && counts[top] === 0) top--;
        let acc = 0, floor = 0;
        for (let v = 0; v <= top; v++) {
          acc += counts[v];
          if (acc >= total * SGRAM_FLOOR_PCT) { floor = v; break; }
        }
        const sp = Math.max(SGRAM_MIN_SPAN_DB,
                            Math.min(SGRAM_MAX_SPAN_DB, top - floor));
        scaleRef.current = { key: scaleKey, topQ: top, botQ: top - sp };
      }
      topQ = scaleRef.current.topQ;
      botQ = scaleRef.current.botQ;

      // Built in screen space, one pixel column at a time, so a gap in the
      // record stays a gap on the plot instead of being closed up by drawing
      // the columns shoulder to shoulder.
      //
      // At *device* resolution, so drawImage lands 1:1 under the dpr transform.
      // Built at CSS pixels and stretched, the resampling beat against the
      // column spacing and drew faint vertical seams that looked like dropouts.
      const lut = rampLUT(cs);
      const off = offRef.current || (offRef.current = document.createElement("canvas"));
      const W = Math.max(1, Math.round(w * dpr));
      const H = Math.max(1, Math.round(SGRAM_H * dpr));

      const rkey = `${nb}x${H}`;
      if (rowsRef.current.key !== rkey) rowsRef.current = { key: rkey, rows: rowBins(ax.f, H) };
      const rows = rowsRef.current.rows;

      // Only redrawn when the picture can actually have changed: the series
      // ticks at 10 Hz and columns arrive at 2, so four frames in five would
      // otherwise rebuild an identical image.
      const imgKey = `${scaleKey}|${W}x${H}|${topQ}|${botQ}|${rampCache.key}`;
      if (off.dataset.key !== imgKey) {
        if (off.width !== W || off.height !== H) { off.width = W; off.height = H; }
        const octx = off.getContext("2d");
        const img = octx.createImageData(W, H);
        const px = img.data;
        const scale = 255 / Math.max(1, topQ - botQ);

        let j = j0;
        for (let x = 0; x < W; x++) {
          const t = t0 + ((x + 0.5) / W) * WINDOW_S;
          while (j + 1 < hist.t.length && hist.t[j + 1] <= t) j++;
          if (j >= hist.t.length) continue;

          // A column covers the ground up to the next one, not a nominal dt.
          // Cadence jitters by a few ms, and testing against dt exactly left
          // whichever intervals ran long uncovered - scattered 1px blanks that
          // read as dropouts. Capped at 1.5 dt so a genuinely missing column
          // still shows as a gap rather than being smeared over by its
          // neighbour.
          const nxt = j + 1 < hist.t.length ? hist.t[j + 1] : hist.t[j] + meta.dt;
          const cover = Math.min(nxt - hist.t[j], meta.dt * 1.5);
          if (t < hist.t[j] - meta.dt * 0.5 || t > hist.t[j] + cover) continue;

          const col = hist.col[j];
          for (let y = 0; y < H; y++) {
            let v = (col[rows[y]] - botQ) * scale;
            v = v < 0 ? 0 : v > 255 ? 255 : v | 0;
            const o = (y * W + x) * 4, c = v * 3;
            px[o] = lut[c]; px[o + 1] = lut[c + 1]; px[o + 2] = lut[c + 2]; px[o + 3] = 255;
          }
        }
        octx.putImageData(img, 0, 0);
        off.dataset.key = imgKey;
      }
      g.imageSmoothingEnabled = false;
      g.drawImage(off, 0, SG_Y, w, SGRAM_H);

      // decade ticks up the frequency axis
      const f = ax.f, n = f.length;
      const lf0 = Math.log10(f[0]), lf1 = Math.log10(f[n - 1]);
      g.font = "10px system-ui, sans-serif";
      g.textBaseline = "middle";
      g.textAlign = "left";
      for (const [hz, label] of decades(f[0], f[n - 1])) {
        const y = SG_Y + (1 - (Math.log10(hz) - lf0) / (lf1 - lf0)) * SGRAM_H;
        g.strokeStyle = cs.getPropertyValue("--axis").trim();
        g.globalAlpha = 0.35;
        g.lineWidth = 1;
        g.beginPath(); g.moveTo(0, Math.round(y) + .5); g.lineTo(w, Math.round(y) + .5); g.stroke();
        g.globalAlpha = 1;
        // On a chip: muted ink over a mid-ramp blue is unreadable, and the
        // ramp puts a different blue under the label on every channel.
        const tw = g.measureText(label).width;
        g.fillStyle = cs.getPropertyValue("--surface").trim();
        g.globalAlpha = 0.85;
        g.fillRect(2, y - 12.5, tw + 7, 12);
        g.globalAlpha = 1;
        g.fillStyle = cs.getPropertyValue("--ink-2").trim();
        g.fillText(label, 5.5, y - 6.5);
      }

      const topS = (topQ + meta.db_offset).toFixed(0) + " dB";
      const botS = (botQ + meta.db_offset).toFixed(0) + " dB";
      setAxis((a) => (a.top === topS && a.bot === botS ? a : { ...a, top: topS, bot: botS }));
    }

    // ---------------- shared time axis ----------------
    const step = niceStep(WINDOW_S);
    g.font = "10px system-ui, sans-serif";
    g.textBaseline = "bottom";
    g.fillStyle = cs.getPropertyValue("--muted").trim();
    g.strokeStyle = cs.getPropertyValue("--grid").trim();
    g.lineWidth = 1;

    for (let a = 0; a <= WINDOW_S; a += step) {
      const x = X(t1 - a);
      if (x < 12) continue;
      g.beginPath(); g.moveTo(Math.round(x) + .5, SERIES_H); g.lineTo(Math.round(x) + .5, SERIES_H + TL_GAP); g.stroke();
      g.textAlign = a === 0 ? "right" : "center";
      g.fillText(a === 0 ? "now" : "−" + fmtAgo(a), a === 0 ? w - 2 : x, h - 2);
    }

    // ---------------- cursor ----------------
    if (hover != null) {
      const x = Math.max(0, Math.min(w, hover.x));
      const t = t0 + (x / w) * WINDOW_S;

      // One line across both plots - the whole reason they share a canvas.
      g.strokeStyle = cs.getPropertyValue("--axis").trim();
      g.lineWidth = 1;
      g.beginPath();
      g.moveTo(Math.round(x) + .5, 0);
      g.lineTo(Math.round(x) + .5, SG_Y + SGRAM_H);
      g.stroke();

      const inSpec = hover.y >= SG_Y && hist && hist.t.length && meta && ax;
      let next = null;

      if (inSpec) {
        let j = 0, best = Infinity;
        for (let q = 0; q < hist.t.length; q++) {
          const d = Math.abs(hist.t[q] - t);
          if (d < best) { best = d; j = q; }
        }
        if (best <= meta.dt) {
          // rows is indexed in device pixels, the pointer arrives in CSS ones.
          const rows = rowsRef.current.rows;
          const yy = Math.max(0, Math.min(rows.length - 1,
                                          Math.round((hover.y - SG_Y) * dpr)));
          const bin = rows[yy];
          const db = hist.col[j][bin] + meta.db_offset;
          next = {
            x, y: hover.y,
            text: `${fmtHz(ax.f[bin])} · ${db.toFixed(0)} dB`,
            sub: `${fmt(Math.pow(10, db / 20))} ${unit} rms · −${(t1 - hist.t[j]).toFixed(1)}s`,
          };
        }
      } else if (s.t.length - i0 >= 2) {
        let k = i0, best = Infinity;
        for (let q = i0; q < s.t.length; q++) {
          const d = Math.abs(s.t[q] - t);
          if (d < best) { best = d; k = q; }
        }
        next = {
          x, y: hover.y,
          text: `avg ${fmt(s.mean[k])} - rms ${fmt(s.rms[k])}`,
          sub: `min ${fmt(s.min[k])} - max ${fmt(s.max[k])} · −${(t1 - s.t[k]).toFixed(1)}s`,
        };
      }

      setTip((p) => (
        p && next && p.x === next.x && p.y === next.y &&
        p.text === next.text && p.sub === next.sub ? p : next
      ));
    } else setTip((p) => (p === null ? p : null));
  });

  const onMove = useCallback((e) => {
    const r = e.currentTarget.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    setHover((p) => (p && p.x === x && p.y === y ? p : { x, y }));
  }, []);

  return html`
    <div class="plot">
      <canvas
        class="tl"
        style=${`height:${TL_H}px`}
        ref=${canvasRef}
        onPointerMove=${onMove}
        onPointerLeave=${() => setHover(null)}
      ></canvas>
      <div class="yaxis" style=${`bottom:calc(100% - ${SERIES_H}px)`}>
        <div>${axis.hi}</div>
        <div>${axis.lo}</div>
      </div>
      ${tip && html`
        <div class="tip" style=${`left:${tip.x}px;top:${tip.y - 10}px`}>
          ${tip.text}<br/>${tip.sub}
        </div>`
      }
    </div>
    <div class="tlfoot">
      <span class="dim">series above, spectrogram below - same ${fmtAgo(WINDOW_S)}</span>
      <span class="cbar">
        <span>${axis.bot}</span><i></i><span>${axis.top}</span>
      </span>
    </div>`;
}

function fmtHz(f) {
  if (f == null || !isFinite(f)) return "–";
  if (f >= 1000) return (f / 1000).toFixed(f >= 10000 ? 1 : 2) + " kHz";
  return f.toFixed(f >= 100 ? 1 : 2) + " Hz";
}

// Decade ticks that actually fall inside the axis, so a 1.17 Hz .. 4.8 kHz
// sweep is labelled 10 / 100 / 1k rather than at whatever the ends happen to be.
function decades(f0, f1) {
  const out = [];
  for (let e = Math.ceil(Math.log10(f0)); Math.pow(10, e) <= f1; e++) {
    const f = Math.pow(10, e);
    out.push([f, f >= 1000 ? f / 1000 + "k" : String(f)]);
  }
  return out;
}

// Frequency domain. Log frequency, dB amplitude, with the decaying peak-hold
// trace behind the live one - a spike that happened while you were reading a
// different card is still on the plot when you get here.
function Spectrum({ idx, unit }) {
  useSpectrumTick(idx);

  const canvasRef = useRef(null);
  const [hover, setHover] = useState(null);
  const [tip, setTip] = useState(null);
  const [axis, setAxis] = useState({ lo: "", hi: "" });

  const ax = store.specAxis;
  const s = store.spec[idx];

  useEffect(() => {
    const cv = canvasRef.current;
    if (!cv || !ax || !s) return;

    const cs = getComputedStyle(document.body);
    const dpr = window.devicePixelRatio || 1;
    const w = cv.clientWidth, h = cv.clientHeight;
    if (!w || !h) return;

    if (cv.width !== w * dpr || cv.height !== h * dpr) {
      cv.width = w * dpr; cv.height = h * dpr;
    }

    const g = cv.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);

    const f = ax.f, n = f.length;
    if (n < 2) return;

    // Range from the peak trace, not the live one, so the axis does not jump
    // every time a transient lands.
    let top = -Infinity, bot = Infinity;
    for (let k = 0; k < n; k++) {
      if (s.peak[k] > top) top = s.peak[k];
      if (s.db[k] < bot && s.db[k] > ax.db_floor) bot = s.db[k];
    }
    if (!isFinite(top)) return;
    if (!isFinite(bot)) bot = top - DB_MIN_SPAN;

    top = Math.ceil((top + 3) / DB_STEP) * DB_STEP;
    bot = Math.floor(bot / DB_STEP) * DB_STEP;
    bot = Math.min(bot, top - DB_MIN_SPAN);
    bot = Math.max(bot, top - DB_MAX_SPAN);

    // The trace stops above a gutter the decade labels own, rather than being
    // drawn under them - a label sitting in the fill is hard to read exactly
    // where the fill is densest.
    const ph = h - SPEC_LABEL_H;
    const lf0 = Math.log10(f[0]), lf1 = Math.log10(f[n - 1]);
    const X = (hz) => ((Math.log10(hz) - lf0) / (lf1 - lf0)) * w;
    const Y = (db) => ph - ((db - bot) / (top - bot)) * ph;

    // decade grid
    g.strokeStyle = cs.getPropertyValue("--grid").trim();
    g.lineWidth = 1;
    for (const [hz] of decades(f[0], f[n - 1])) {
      const x = Math.round(X(hz)) + 0.5;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, ph); g.stroke();
    }

    // peak hold, behind
    g.strokeStyle = cs.getPropertyValue("--axis").trim();
    g.lineWidth = 1;
    g.beginPath();
    for (let k = 0; k < n; k++) {
      const x = X(f[k]), y = Y(s.peak[k]);
      k ? g.lineTo(x, y) : g.moveTo(x, y);
    }
    g.stroke();

    // live spectrum, filled to the floor
    g.beginPath();
    g.moveTo(X(f[0]), ph);
    for (let k = 0; k < n; k++) g.lineTo(X(f[k]), Y(Math.max(s.db[k], bot)));
    g.lineTo(X(f[n - 1]), ph);
    g.closePath();
    g.fillStyle = cs.getPropertyValue("--series-band").trim();
    g.fill();

    g.strokeStyle = cs.getPropertyValue("--series").trim();
    g.lineWidth = 1.5; g.lineJoin = "round";
    g.beginPath();
    for (let k = 0; k < n; k++) {
      const x = X(f[k]), y = Y(Math.max(s.db[k], bot));
      k ? g.lineTo(x, y) : g.moveTo(x, y);
    }
    g.stroke();

    // decade labels, on the canvas so they cannot drift from their gridlines
    g.fillStyle = cs.getPropertyValue("--muted").trim();
    g.font = "10px system-ui, sans-serif";
    g.textBaseline = "bottom";
    for (const [hz, label] of decades(f[0], f[n - 1])) {
      g.textAlign = X(hz) > w - 18 ? "right" : "left";
      g.fillText(label, X(hz) + (g.textAlign === "left" ? 3 : -3), h - 1);
    }

    const hiS = top + " dB", loS = bot + " dB";
    setAxis((a) => (a.lo === loS && a.hi === hiS ? a : { lo: loS, hi: hiS }));

    if (hover != null) {
      // Nearest bin by screen position: the axis is log, so nearest-in-Hz
      // would snap to the wrong side everywhere below a decade boundary.
      let k = 0, best = Infinity;
      for (let j = 0; j < n; j++) {
        const d = Math.abs(X(f[j]) - hover);
        if (d < best) { best = d; k = j; }
      }
      const x = X(f[k]);

      g.strokeStyle = cs.getPropertyValue("--axis").trim();
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(x + .5, 0); g.lineTo(x + .5, ph); g.stroke();
      g.fillStyle = cs.getPropertyValue("--series").trim();
      g.beginPath(); g.arc(x, Y(Math.max(s.db[k], bot)), 3.5, 0, 6.284); g.fill();

      const next = {
        x, y: Y(Math.max(s.db[k], bot)),
        text: `${fmtHz(f[k])} · ${s.db[k].toFixed(1)} dB`,
        sub: `${fmt(Math.pow(10, s.db[k] / 20))} ${unit} rms · peak ${s.peak[k].toFixed(1)} dB`,
      };
      setTip((p) => (
        p && p.x === next.x && p.text === next.text &&
        p.sub === next.sub && p.y === next.y ? p : next
      ));
    } else setTip((p) => (p === null ? p : null));
  });

  const onMove = useCallback((e) => {
    const r = e.currentTarget.getBoundingClientRect();
    const x = e.clientX - r.left;
    setHover((p) => (p === x ? p : x));
  }, []);

  if (!ax || !s) {
    return html`<div class="spec-wait">
      ${store.status?.connected ? "Filling the FFT window ..." : "No spectrum"}
    </div>`;
  }

  return html`
    <div class="plot">
      <canvas
        class="spec"
        ref=${canvasRef}
        onPointerMove=${onMove}
        onPointerLeave=${() => setHover(null)}
      ></canvas>
      <div class="yaxis spec">
        <div>${axis.hi}</div>
        <div>${axis.lo}</div>
      </div>
      ${tip && html`
        <div class="tip" style=${`left:${tip.x}px;top:${tip.y - 10}px`}>
          ${tip.text}<br/>${tip.sub}
        </div>`
      }
    </div>
    <div class="specfoot">
      <span>
        ${s.f0 == null
          ? html`<span class="dim">no dominant tone</span>`
          : html`<b>${fmtHz(s.f0)}</b> ${fmt(s.f0_amp)} ${unit} · THD <b>${(s.thd * 100).toFixed(2)}%</b>`}
      </span>
      <span class="dim">${ax.resolution_hz} Hz bins</span>
    </div>`;
}

// Config form
function Field({ spec, value, onInput, error, scope, prefix = "" }) {
  const kind = spec.type || "number";
  const id = `ch${scope}-${prefix}${spec.key}`;

  if (kind === "bool") {
    return html`
      <div class="field row ${error ? "bad" : ""}">
        <input
          type="checkbox"
          id=${id}
          checked=${!!value}
          onChange=${(e) => onInput(e.currentTarget.checked)}
        />
        
        <label for=${id}>${spec.label}</label>

        ${error &&
          html`<div class="err">${error}</div>`
        }
      </div>`;
  }

  return html`
    <div class="field ${error ? "bad" : ""}">
      <label for=${id}>
        ${spec.label}${spec.unit ? html` <span class="u">(${spec.unit})</span>` : null}
      </label>

      <input
        id=${id}
        type=${kind === "text" ? "text" : "number"}
        step="any"
        value=${value ?? ""}
        onInput=${(e) => onInput(e.currentTarget.value)}
      />

      ${spec.help && html`
        <div class="help">${spec.help}</div>
      `}

      ${error && html`
        <div class="err">${error}</div>
      `}
    </div>`;
}

function ConfigForm({ meta, schema, form, setForm, errors }) {
  const types = schema.frontend_types;
  const typeFields = types[form.type]?.fields ?? [];

  const setParam = (k, v) =>
    setForm({ ...form, params: { ...form.params, [k]: v } });

  const unit = form.type === "fixed"
    ? (form.params.unit || "V")
    : (types[form.type]?.unit || "");

  return html`
    <div class="cfg-form">
      <div class="cfg-scale">
        applied scale <b>${fmtScale(meta.scale)}</b> ${unit || "unit"} per volt at pin
      </div>

      <div class="cfg-group">Front end</div>
      <div class="field ${errors.type ? "bad" : ""}">
        <label for=${`ch${meta.idx}-type`}>Type</label>

        <select
          id=${`ch${meta.idx}-type`}
          value=${form.type}
          onChange=${(e) => setForm({ ...form, type: e.currentTarget.value, params: {} })}>
          ${Object.entries(types).map(([k, t]) => html`<option key=${k} value=${k}>${t.label}</option>`)}
        </select>

        ${types[form.type]?.help &&
          html`<div class="help">${types[form.type].help}</div>`
        }

        ${errors.type && html`
          <div class="err">${errors.type}</div>
        `}
      </div>

      ${typeFields.map((spec) => html`
        <${Field}
          key=${spec.key}
          spec=${spec}
          prefix="params."
          scope=${meta.idx}
          value=${form.params[spec.key] ?? spec.default}
          error=${errors["params." + spec.key]}
          onInput=${(v) => setParam(spec.key, v)}
        />`)
      }

      <div class="cfg-group">Correction</div>

      ${schema.common_fields.map((spec) => html`
        <${Field}
          key=${spec.key}
          spec=${spec}
          scope=${meta.idx}
          value=${form[spec.key]} error=${errors[spec.key]}
          onInput=${(v) => setForm({ ...form, [spec.key]: v })}
        />`)
      }
    </div>`;
}

function Confirm({ kind, onYes, onNo, busy }) {
  const apply = kind === "apply";
  return html`
    <div class="confirm-wrap" onClick=${(e) => e.target === e.currentTarget && onNo()}>
      <div class="confirm" role="dialog" aria-modal="true">
        <h3>${apply ? "Apply changes?" : "Discard changes?"}</h3>

        <p>
          ${apply
            ? "The new front-end config is saved and takes effect on the next block."
            : "Your edits are thrown away and the saved config stays as it is."}
        </p>

        <div class="row">
          <button class="btn" onClick=${onNo} disabled=${busy}>Cancel</button>
          <button class=${"btn " + (apply ? "primary" : "danger")} onClick=${onYes} disabled=${busy}>
            ${busy ? "Saving ..." : apply ? "Apply" : "Discard"}
          </button>
        </div>
      </div>
    </div>`;
}

function formFromMeta(m) {
  const params = {};
  for (const [k, v] of Object.entries(m.params || {})) params[k] = v;
  return {
    type: m.type,
    params,
    name: m.name,
    zero_offset_v: m.zero_offset_v,
    gain_correction: m.gain_correction,
    r_thevenin: m.r_thevenin,
    invert: m.invert,
  };
}

function ChannelCard({ meta, schema, onSaved }) {
  const [mode, setMode] = useState("view");      // view | edit
  const [confirm, setConfirm] = useState(null);  // null | apply | discard
  const [form, setForm] = useState(() => formFromMeta(meta));
  const [errors, setErrors] = useState({});
  const [busy, setBusy] = useState(false);
  const [toast, setToast] = useState(null);
  const ok = useChannelOk(meta.idx);

  const startEdit = () => {
    setForm(formFromMeta(meta));
    setErrors({}); setToast(null); setMode("edit");
  };

  const doApply = async () => {
    setBusy(true);
    const { ok: sent, status, body } = await postConfig(meta.idx, form);
    setBusy(false);

    if (sent) {
      setErrors({}); setConfirm(null); setMode("view");
      onSaved(body.channels);
    } else {
      setErrors(body.fields || {});
      setConfirm(null);
      setToast(body.error || `save failed (${status})`);
    }
  };

  const doDiscard = () => {
    setForm(formFromMeta(meta));
    setErrors({}); setToast(null); setConfirm(null); setMode("view");
  };

  const editing = mode === "edit";

  return html`
    <div class=${"tile" + (editing ? " editing" : "") + (ok ? "" : " stale")}>
      <div class="actions">
        ${editing ? html`
          <button
            class="iconbtn apply" title="Apply changes"
            aria-label="Apply changes"
            onClick=${() => setConfirm("apply")}><${CheckIcon}
          /></button>

          <button
            class="iconbtn discard" title="Discard changes"
            aria-label="Discard changes"
            onClick=${() => setConfirm("discard")}><${XIcon}
          /></button>
        ` : html`
          <button
            class="iconbtn cfg" title="Configure analog front end"
            aria-label="Configure analog front end"
            onClick=${startEdit}><${ConfigIcon}
          /></button>`}
      </div>

      <div class="tile-head">
        <span class="tile-name">${meta.name}</span>
        <span class="tile-src">(ADC${meta.adc} ch${meta.ch})</span>
      </div>

      <div class="tile-fe">
        ${meta.type_label}
        ${meta.note && html`<span class="warn"> - ${meta.note}</span>`}
      </div>

      ${editing
        ? html`
            <${Readout} idx=${meta.idx} unit=${meta.unit} />
            <${ConfigForm} meta=${meta} schema=${schema} form=${form} setForm=${setForm} errors=${errors} />`
        : html`
            <div class="panes">
              <div class="pane-now">
                <${Readout} idx=${meta.idx} unit=${meta.unit} />
                <${Spectrum} idx=${meta.idx} unit=${meta.unit} />
              </div>
              <div class="pane-past">
                <${Timeline} idx=${meta.idx} unit=${meta.unit} />
              </div>
            </div>`
      }

      ${toast && html`<div class="toast">${toast}</div>`}

      ${confirm && html`
        <${Confirm}
          kind=${confirm} busy=${busy}
          onYes=${confirm === "apply" ? doApply : doDiscard}
          onNo=${() => setConfirm(null)}
        />`
      }
    </div>`;
}

function StatusBar() {
  const status = useStatus();
  const up = status?.connected;
  const f = [];

  if (status?.session != null) f.push(["session", status.session.toString(16).toUpperCase()]);
  if (status?.sps_actual) f.push(["Rate", status.sps_actual.toLocaleString() + " SPS"]);
  if (status?.block_frames) f.push(["Block", status.block_frames + " frames"]);
  if (status) {
    f.push(["Packets", status.packets.toLocaleString()]);
    f.push(["Gaps", status.gaps]);
    f.push(["Lost", status.lost.toLocaleString()]);
    if (status.crc) f.push(["crc errors", status.crc]);
  }

  return html`
    <header>
      <h1>Rack Monitor</h1>
      <div class=${"status " + (up ? "up" : "down")}>
        <span class="dot"></span>
        <span>${up ? "streaming from " + status.peer : "No board"}</span>
      </div>
      <div class="spacer"></div>
      <div class="stats">
        ${f.map(([k, v]) => html`<span>${k} <b>${v}</b></span>`)}
      </div>
    </header>`;
}

function ConnectionNotice() {
  const status = useStatus();
  if (status?.connected) return null;
  return html`<div class="empty">Waiting for the board to connect ...</div>`;
}

function App() {
  const [schema, setSchema] = useState(null);

  useEffect(() => {
    let alive = true;
    fetch("/api/meta").then((r) => r.json()).then((m) => {
      if (!alive) return;
      // Fill in only what the stream has not already created - meta arriving
      // second must not discard a history that landed first.
      m.channels.forEach((_, i) => {
        store.series[i] ||= { t: [], mean: [], rms: [], min: [], max: [] };
        chanSubs(i); specSubs(i);
      });
      if (m.spectrum) store.specAxis = m.spectrum;
      setSchema(m);
    });

    const es = new EventSource("/events");
    es.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.type === "status") {
        store.status = m;
        notify(store.statusSubs);
        return;
      }

      if (m.type === "spectrum_axis") {
        // A new board session can be clocked differently, so the axis replaces
        // the old one and the held traces that were drawn against it.
        store.specAxis = m;
        store.spec = [];
        store.specChan.forEach((s) => notify(s));
        return;
      }

      if (m.type === "spectrogram_history") {
        // The past, which the page cannot reconstruct - it only ever sees
        // columns from the moment it connected. Arrives once, as bytes.
        const bytes = b64ToBytes(m.data);
        store.histMeta = {
          db_offset: m.db_offset, dt: m.dt, seconds: m.seconds, bins: m.bins,
        };
        store.hist = [];
        for (let c = 0; c < m.nch; c++) store.hist[c] = { t: [], col: [] };
        for (let j = 0; j < m.t.length; j++) {
          for (let c = 0; c < m.nch; c++) {
            const off = (j * m.nch + c) * m.bins;
            store.hist[c].t.push(m.t[j]);
            // subarray, not slice: a view costs nothing and the buffer is
            // dropped whole once every column referencing it has aged out.
            store.hist[c].col.push(bytes.subarray(off, off + m.bins));
          }
        }
        store.hist.forEach((_, i) => notify(specSubs(i)));
        return;
      }

      if (m.type === "spectrum") {
        m.ch.forEach((c, i) => {
          store.spec[i] = c;
          appendColumn(i, m.t, c.db);
        });
        m.ch.forEach((_, i) => notify(specSubs(i)));
        return;
      }

      if (m.type === "block_history") {
        // Arrives columnar, to keep the key names off the wire. Expanded back
        // into block shape here and pushed through the same path as a live
        // block, so there is still one definition of what a block does to the
        // store rather than a second one that has to agree with it.
        for (let j = 0; j < m.t.length; j++) {
          ingestBlock({
            t: m.t[j],
            ch: m.ch.map((c) => ({
              mean: c.mean[j], rms: c.rms[j], min: c.min[j], max: c.max[j],
              ok: !!c.ok[j],
            })),
          });
        }
        store.series.forEach((_, i) => notify(chanSubs(i)));
        return;
      }

      if (m.type !== "block") return;

      ingestBlock(m);
      m.ch.forEach((_, i) => notify(chanSubs(i)));
    };

    return () => { alive = false; es.close(); };
  }, []);

  const onSaved = useCallback((chs) => {
    setSchema((s) => (s ? { ...s, channels: chs } : s));
  }, []);

  if (!schema) return html`<div class="empty">Loading ...</div>`;

  return html`
    <${StatusBar} />

    <div class="grid">
      ${schema.channels.map((m) =>
        html`<${ChannelCard} key=${m.idx} meta=${m} schema=${schema} onSaved=${onSaved} />`)
      }
    </div>

    <${ConnectionNotice} />`;
}

render(html`<${App} />`, document.getElementById("app"));
