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
  status: null,
  chan: [],            // per channel: Set of callbacks
  statusSubs: new Set(),
};

const chanSubs = (i) => (store.chan[i] ||= new Set());
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

function Chart({ idx }) {
  useChannelTick(idx);

  const canvasRef = useRef(null);
  const [hover, setHover] = useState(null);
  const [tip, setTip] = useState(null);
  const [axis, setAxis] = useState({ lo: "", hi: "" });

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

    if (s.t.length < 2) return;

    const now = s.t[s.t.length - 1];
    let i0 = s.t.length - 1;
    while (i0 > 0 && now - s.t[i0 - 1] <= WINDOW_S) i0--;
    const n = s.t.length - i0;

    let lo = Infinity, hi = -Infinity;
    for (let k = i0; k < s.t.length; k++) {
      if (s.min[k] < lo) lo = s.min[k];
      if (s.max[k] > hi) hi = s.max[k];
    }

    if (!isFinite(lo) || !isFinite(hi)) return;
    if (hi - lo < 1e-12) { const c = (hi + lo) / 2 || 0; lo = c - 1e-6; hi = c + 1e-6; }

    const pad = (hi - lo) * 0.10; lo -= pad; hi += pad;
    const X = (k) => ((k - i0) / Math.max(1, n - 1)) * w;
    const Y = (v) => h - ((v - lo) / (hi - lo)) * h;

    // zero line, only when zero is actually in view
    if (lo < 0 && hi > 0) {
      g.strokeStyle = cs.getPropertyValue("--grid").trim();
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(0, Y(0) + .5); g.lineTo(w, Y(0) + .5); g.stroke();
    }

    // min/max envelope: same entity as the mean, so same hue, low alpha
    g.fillStyle = cs.getPropertyValue("--series-band").trim();
    g.beginPath();
    for (let k = i0; k < s.t.length; k++) g.lineTo(X(k), Y(s.max[k]));
    for (let k = s.t.length - 1; k >= i0; k--) g.lineTo(X(k), Y(s.min[k]));
    g.closePath(); g.fill();

    // the windowed average - the series itself
    g.strokeStyle = cs.getPropertyValue("--series").trim();
    g.lineWidth = 2; g.lineJoin = "round"; g.lineCap = "round";
    g.beginPath();

    for (let k = i0; k < s.t.length; k++) {
      const x = X(k), y = Y(s.mean[k]);
      k === i0 ? g.moveTo(x, y) : g.lineTo(x, y);
    }

    g.stroke();

    const loS = fmt(lo), hiS = fmt(hi);
    setAxis((a) => (a.lo === loS && a.hi === hiS ? a : { lo: loS, hi: hiS }));

    if (hover != null) {
      const k = Math.min(s.t.length - 1, Math.max(i0, i0 + Math.round((hover / w) * (n - 1))));
      const x = X(k);

      g.strokeStyle = cs.getPropertyValue("--axis").trim();
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(x + .5, 0); g.lineTo(x + .5, h); g.stroke();
      g.fillStyle = cs.getPropertyValue("--series").trim();
      g.beginPath(); g.arc(x, Y(s.mean[k]), 4, 0, 6.284); g.fill();

      const next = {
        x, y: Y(s.mean[k]),
        text: `avg ${fmt(s.mean[k])} - rms ${fmt(s.rms[k])}`,
        sub: `min ${fmt(s.min[k])} - max ${fmt(s.max[k])} · −${(now - s.t[k]).toFixed(1)}s`,
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

  return html`
    <div class="plot">
      <canvas
        ref=${canvasRef}
        onPointerMove=${onMove}
        onPointerLeave=${() => setHover(null)}
      ></canvas>
      <div class="yaxis">
        <div>${axis.hi}</div>
        <div>${axis.lo}</div>
      </div>
      ${tip && html`
        <div class="tip" style=${`left:${tip.x}px;top:${tip.y - 10}px`}>
          ${tip.text}<br/>${tip.sub}
        </div>`
      }
    </div>
    <div class="xlabel">last ${WINDOW_S}s</div>`;
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

      <${Readout} idx=${meta.idx} unit=${meta.unit} />

      ${editing
        ? html`<${ConfigForm} meta=${meta} schema=${schema} form=${form} setForm=${setForm} errors=${errors} />`
        : html`<${Chart} idx=${meta.idx} />`
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
        <span>${up ? "streaming from " + status.peer : "no board"}</span>
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
      store.series = m.channels.map(() => ({ t: [], mean: [], rms: [], min: [], max: [] }));
      m.channels.forEach((_, i) => chanSubs(i));
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

      if (m.type !== "block") return;

      m.ch.forEach((b, i) => {
        const s = store.series[i];
        if (!s) return;

        s.t.push(m.t); s.mean.push(b.mean); s.rms.push(b.rms);
        s.min.push(b.min); s.max.push(b.max);

        if (s.t.length > CAP) {
          for (const k of ["t", "mean", "rms", "min", "max"]) s[k].splice(0, s[k].length - CAP);
        }

        store.last[i] = b;
      });

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
