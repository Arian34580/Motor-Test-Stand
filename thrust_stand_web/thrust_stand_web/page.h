// page.h - the control panel web page served by thrust_stand_web.ino
// Keep this file in the same folder as thrust_stand_web.ino. (It is a separate file because the
// Arduino IDE can mistake JavaScript inside a .ino file for C++ functions.)
#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Thrust Stand</title>
<style>
:root{--bg:#f3f4f1;--card:#fff;--ink:#15171a;--muted:#5d6166;--line:#dcdfe2;--accent:#2563eb;--go:#15803d;--bad:#dc2626;--warn:#a85b00}
@media (prefers-color-scheme:dark){:root{--bg:#0f1113;--card:#181b1f;--ink:#eceef0;--muted:#9ba1a8;--line:#2b2f35;--accent:#5b8cff;--go:#22a355;--bad:#ef4444;--warn:#e0a030}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.4 -apple-system,system-ui,"Segoe UI",Roboto,sans-serif;padding:12px 16px calc(100px + env(safe-area-inset-bottom))}
header{display:flex;align-items:center;justify-content:space-between;margin:2px 0 10px}
h1{font-size:20px;margin:0}
.conn{display:flex;align-items:center;gap:6px;font-size:13px;color:var(--muted)}
.dot{width:10px;height:10px;border-radius:50%;background:var(--bad)}
.dot.on{background:var(--go)}
#banner{border-radius:12px;padding:10px 12px;margin-bottom:12px;background:var(--card);border:1px solid var(--line);font-weight:600}
#banner.run{border-color:var(--accent);box-shadow:inset 4px 0 0 var(--accent)}
#banner.stop{background:var(--bad);border-color:var(--bad);color:#fff}
#toast{margin:-4px 0 12px;color:var(--bad);font-size:14px;font-weight:600}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px;margin-bottom:12px}
h2{font-size:13px;margin:0 0 8px;color:var(--muted);font-weight:700;text-transform:uppercase;letter-spacing:.05em}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.tile{background:var(--bg);border-radius:10px;padding:8px 10px;min-width:0}
.tile.big{grid-column:1/-1}
.k{font-size:12px;color:var(--muted)}
.v{font-size:26px;font-weight:700;font-variant-numeric:tabular-nums;white-space:nowrap}
.big .v{font-size:44px;line-height:1.1}
.u{font-size:14px;font-weight:600;color:var(--muted);margin-left:4px}
canvas{display:block;width:100%;height:150px;margin-top:10px}
.note{font-size:13px;color:var(--muted);margin-top:6px}
.warn{color:var(--warn)}
button{font:inherit;font-weight:700;border:0;border-radius:10px;padding:12px 14px;min-height:48px;cursor:pointer;touch-action:manipulation}
button:disabled{opacity:.35;cursor:default}
.go{background:var(--go);color:#fff}
.pri{background:var(--accent);color:#fff}
.gh{background:var(--bg);color:var(--ink);border:1px solid var(--line)}
.row{display:flex;gap:8px;margin-top:10px}
.row button{flex:1}
input[type=range]{width:100%;margin:8px 0 2px;accent-color:var(--accent);height:32px}
input[type=text]{font:inherit;width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--ink);margin-top:10px}
.sl{display:flex;justify-content:space-between;font-size:13px;color:var(--muted)}
.bar{height:8px;border-radius:4px;background:var(--bg);overflow:hidden;margin-top:10px}
.bar div{height:100%;width:0;background:var(--accent);transition:width .25s}
.run{border-top:1px solid var(--line);padding:10px 0}
.run:first-child{border-top:0;padding-top:2px}
.run b{display:block;word-break:break-all;color:var(--ink)}
.run a{display:inline-block;margin:6px 16px 0 0;color:var(--accent);font-weight:600;text-decoration:none}
.run a.del{color:var(--bad)}
.foot{position:fixed;left:0;right:0;bottom:0;display:flex;gap:8px;padding:10px 16px calc(10px + env(safe-area-inset-bottom));background:var(--bg);border-top:1px solid var(--line)}
#stopBtn{flex:1;background:var(--bad);color:#fff;font-size:20px;letter-spacing:.04em;min-height:62px}
#resetBtn{background:var(--ink);color:var(--bg);min-height:62px;padding:12px 20px}
.hide{display:none!important}
</style>
</head>
<body>
<header><h1>Thrust Stand</h1><div class="conn"><span class="dot" id="dot"></span><span id="connTxt">Connecting</span></div></header>
<div id="banner">Connecting&hellip;</div>
<div id="toast" class="hide"></div>

<section class="card">
  <div class="grid">
    <div class="tile big"><div class="k">Thrust</div><div class="v"><span id="thrust">&ndash;</span><span class="u">g</span></div></div>
    <div class="tile"><div class="k">Throttle</div><div class="v"><span id="pct">&ndash;</span><span class="u">%</span></div></div>
    <div class="tile"><div class="k">RPM</div><div class="v" id="rpm">&ndash;</div></div>
    <div class="tile"><div class="k">Voltage</div><div class="v"><span id="volts">&ndash;</span><span class="u">V</span></div></div>
    <div class="tile"><div class="k">Current</div><div class="v"><span id="amps">&ndash;</span><span class="u">A</span></div></div>
    <div class="tile"><div class="k">Power</div><div class="v"><span id="watts">&ndash;</span><span class="u">W</span></div></div>
    <div class="tile"><div class="k">ESC temp</div><div class="v"><span id="temp">&ndash;</span><span class="u">&deg;C</span></div></div>
  </div>
  <canvas id="chart"></canvas>
  <div class="note" id="telemNote"></div>
</section>

<section class="card">
  <h2>Auto test</h2>
  <div id="plan" class="note"></div>
  <div class="bar"><div id="prog"></div></div>
  <div id="testInfo" class="note"></div>
  <input type="text" id="label" maxlength="16" placeholder="Label for the files, e.g. MN501-18x6 (optional)" autocomplete="off">
  <div class="row"><button class="go" id="startBtn" disabled>Start auto test</button></div>
</section>

<section class="card">
  <h2>Manual throttle</h2>
  <input type="range" id="slider" min="0" max="20" step="1" value="0">
  <div class="sl"><span>0 %</span><span id="maxLbl">20 %</span></div>
  <div class="row"><button class="pri" id="setBtn" disabled>Set 0 %</button><button class="gh" id="offBtn" disabled>Motor off</button></div>
  <div class="row"><button class="gh" id="tareBtn" disabled>Tare load cell</button></div>
</section>

<section class="card">
  <h2>Recorded runs</h2>
  <div id="runs" class="note">Loading&hellip;</div>
  <div id="storage" class="note"></div>
</section>

<div class="foot"><button id="stopBtn">EMERGENCY STOP</button><button id="resetBtn" class="hide">Reset</button></div>

<script>
const $ = id => document.getElementById(id);
let S = null, lastOk = 0, savedSeen = -1, savingSeen = null, pts = [], toastTimer = 0;
const DASH = '–';

function f(v, d) { return (v === null || v === undefined || isNaN(v)) ? DASH : Number(v).toFixed(d); }
function esc(s) { return String(s).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c])); }
function toast(t) {
  const e = $('toast');
  e.textContent = t;
  e.classList.remove('hide');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => e.classList.add('hide'), 6000);
}

async function post(url) {
  try {
    const r = await fetch(url, {method: 'POST', cache: 'no-store'});
    let j = {};
    try { j = await r.json(); } catch (e) {}
    if (!r.ok || j.ok === false) { toast(j.error || ('Request failed (' + r.status + ')')); return false; }
    return true;
  } catch (e) {
    toast('No connection to the stand.');
    return false;
  }
}

async function poll() {
  const ac = new AbortController(), timer = setTimeout(() => ac.abort(), 1500);
  try {
    const r = await fetch('/api/status', {cache: 'no-store', signal: ac.signal});
    S = await r.json();
    lastOk = Date.now();
    render();
  } catch (e) {}
  clearTimeout(timer);
  const live = Date.now() - lastOk < 1500;
  $('dot').classList.toggle('on', live);
  $('connTxt').textContent = live ? 'Live' : 'No connection';
  if (!live) {
    const b = $('banner');
    b.className = 'stop';
    b.textContent = 'No connection to the stand. If the motor was running, it stops by itself within ' +
      (S ? S.linkTimeout / 1000 : 3) + ' s.';
    setButtons(null);
  }
  setTimeout(poll, 250);
}

function render() {
  const s = S, tel = s.telem;
  $('thrust').textContent = f(s.thrust, 1);
  $('pct').textContent = f(s.pct, Math.abs(s.pct - Math.round(s.pct)) < 0.05 ? 0 : 1);
  $('rpm').textContent = tel ? f(s.rpm, 0) : DASH;
  $('volts').textContent = tel ? f(s.volts, 2) : DASH;
  $('amps').textContent = tel ? f(s.amps, 2) : DASH;
  $('watts').textContent = tel ? f(s.volts * s.amps, 0) : DASH;
  $('temp').textContent = tel ? f(s.temp, 0) : DASH;
  const tn = $('telemNote');
  tn.textContent = tel ? '' : 'No ESC telemetry. Battery connected? Telemetry wire on D32 with the 4.7 kΩ pull-up?';
  tn.className = tel ? 'note' : 'note warn';

  const b = $('banner');
  b.textContent = s.msg;
  b.className = s.estop ? 'stop' : (s.mode !== 'idle' ? 'run' : '');

  const testing = s.mode === 'test';
  $('plan').textContent = '0 → ' + s.max + ' % in ' + s.stepPct + ' % steps, ' + s.hold + ' s each, about ' +
    Math.round(s.testTotal) + ' s in total.';
  $('prog').style.width = testing ? Math.max(0, Math.min(100, 100 * (1 - s.testLeft / s.testTotal))).toFixed(1) + '%' : '0%';
  let info = '';
  if (testing) info = (s.phase === 'step' ? 'Step ' + s.step + ' of ' + s.steps + ', ' + f(s.phaseLeft, 0) + ' s left in this step. ' : '') +
    f(s.testLeft, 0) + ' s left in the test.';
  else if (!s.fs) info = 'File storage not working: check Tools > Partition Scheme.';
  else if (!s.loadOk) info = 'Load cell not responding: check the HX711 wiring.';
  else if (!s.tared) info = 'Load cell starting up\u2026';
  else if (!s.cal) info = 'Load cell not calibrated: upload load_cell_calibrate first.';
  else if (s.saving) info = 'Saving the last run…';
  else if (s.last) info = 'Last run ' + s.last + ': ' + s.lastResult;
  $('testInfo').textContent = info;
  $('startBtn').textContent = testing ? 'Test running…' : 'Start auto test';

  const sl = $('slider');
  if (+sl.max !== s.max) { sl.max = s.max; $('maxLbl').textContent = s.max + ' %'; }
  $('resetBtn').classList.toggle('hide', !s.estop);
  setButtons(s);

  pts.push({t: Date.now(), v: s.thrust});
  draw();
  if (s.saved !== savedSeen || s.saving !== savingSeen) { savedSeen = s.saved; savingSeen = s.saving; loadFiles(); }
}

function setButtons(s) {
  const testing = !!s && s.mode === 'test';
  $('startBtn').disabled = !s || testing || s.estop || !s.loadOk || !s.tared || !s.cal || s.saving || !s.fs;
  $('setBtn').disabled = !s || testing || s.estop;
  $('slider').disabled = !s || testing || s.estop;
  $('offBtn').disabled = !s || testing;
  $('tareBtn').disabled = !s || s.mode !== 'idle' || !s.loadOk;
  $('resetBtn').disabled = !s;
}

function draw() {
  const c = $('chart'), w = c.clientWidth, h = c.clientHeight, r = window.devicePixelRatio || 1;
  if (!w || !h) return;
  if (c.width !== Math.round(w * r) || c.height !== Math.round(h * r)) { c.width = Math.round(w * r); c.height = Math.round(h * r); }
  const g = c.getContext('2d');
  g.setTransform(r, 0, 0, r, 0, 0);
  g.clearRect(0, 0, w, h);
  const now = Date.now(), span = 60000;
  pts = pts.filter(p => now - p.t <= span);
  const vals = pts.filter(p => p.v !== null).map(p => p.v);
  const cs = getComputedStyle(document.documentElement);
  const muted = cs.getPropertyValue('--muted').trim(), grid = cs.getPropertyValue('--line').trim(), acc = cs.getPropertyValue('--accent').trim();
  let lo = Math.min(0, ...vals), hi = Math.max(10, ...vals);
  const pad = (hi - lo) * 0.1;
  hi += pad;
  if (lo < 0) lo -= pad;
  const L = 42, R = 6, T = 8, B = 20;
  const x = t => L + (w - L - R) * (1 - (now - t) / span);
  const y = v => T + (h - T - B) * (1 - (v - lo) / (hi - lo));
  g.font = '11px system-ui, sans-serif';
  g.lineWidth = 1;
  for (let i = 0; i <= 3; i++) {
    const v = lo + (hi - lo) * i / 3, yy = Math.round(y(v)) + 0.5;
    g.strokeStyle = grid;
    g.beginPath(); g.moveTo(L, yy); g.lineTo(w - R, yy); g.stroke();
    g.fillStyle = muted;
    g.fillText(v.toFixed(hi - lo < 10 ? 1 : 0), 2, yy + 4);
  }
  g.fillStyle = muted;
  g.fillText('thrust (g), last 60 s', L, h - 5);
  g.strokeStyle = acc;
  g.lineWidth = 2;
  g.beginPath();
  let pen = false;
  for (const p of pts) {
    if (p.v === null) { pen = false; continue; }
    const X = x(p.t), Y = y(p.v);
    if (pen) g.lineTo(X, Y); else { g.moveTo(X, Y); pen = true; }
  }
  g.stroke();
}

async function loadFiles() {
  try {
    const j = await (await fetch('/api/files', {cache: 'no-store'})).json();
    const runs = {};
    for (const fl of j.files) {
      const m = fl.n.match(/^(.+)_(raw\.csv|summary\.csv|info\.txt)$/);
      if (m) (runs[m[1]] = runs[m[1]] || {})[m[2]] = fl;
    }
    const names = Object.keys(runs).sort().reverse();
    const label = {'summary.csv': 'Summary CSV', 'raw.csv': 'Raw data CSV', 'info.txt': 'Info'};
    $('runs').innerHTML = names.length ? names.map(n => {
      const links = n === j.recording ? '<div class="note">Recording…</div>' :
        ['summary.csv', 'raw.csv', 'info.txt'].filter(k => runs[n][k]).map(k =>
          '<a href="/download?f=' + encodeURIComponent(n + '_' + k) + '" download="' + esc(n + '_' + k) + '">' + label[k] + '</a>').join('') +
        '<a href="#" class="del" data-run="' + esc(n) + '">Delete</a>';
      return '<div class="run"><b>' + esc(n) + '</b>' + links + '</div>';
    }).join('') : 'No runs recorded yet.';
    $('storage').textContent = j.total ? 'Storage: ' + Math.round(j.used / 1024) + ' of ' + Math.round(j.total / 1024) + ' KB used' : '';
  } catch (e) {
    $('runs').textContent = 'Could not load the list of runs.';
  }
}

$('slider').addEventListener('input', () => { $('setBtn').textContent = 'Set ' + $('slider').value + ' %'; });
$('setBtn').onclick = () => post('/api/manual?pct=' + $('slider').value);
$('offBtn').onclick = () => {
  $('slider').value = 0;
  $('setBtn').textContent = 'Set 0 %';
  post('/api/manual?pct=0');
};
$('tareBtn').onclick = () => post('/api/tare');
function estop(e) {
  if (e) e.preventDefault();
  const b = $('banner');
  b.className = 'stop';
  b.textContent = 'Stopping…';
  for (let i = 0; i < 3; i++) post('/api/estop');
}
$('stopBtn').addEventListener('pointerdown', estop);
$('stopBtn').addEventListener('click', () => estop());
$('resetBtn').onclick = () => post('/api/reset');
$('startBtn').onclick = () => {
  if (!S) return;
  const lbl = $('label').value.trim().replace(/[^A-Za-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 16);
  if (!confirm('Start the auto test?\n\n0 → ' + S.max + ' % in ' + S.stepPct + ' % steps, ' + S.hold +
    ' s each.\n\nStand clamped down, prop area clear, everyone behind the stand?')) return;
  const d = new Date(), p = n => String(n).padStart(2, '0');
  const name = d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-' + p(d.getHours()) + p(d.getMinutes()) +
    p(d.getSeconds()) + (lbl ? '_' + lbl : '');
  post('/api/start?name=' + encodeURIComponent(name));
};
$('runs').addEventListener('click', async e => {
  const a = e.target.closest('a.del');
  if (!a) return;
  e.preventDefault();
  if (confirm('Delete run ' + a.dataset.run + '? This cannot be undone.') &&
      await post('/api/delete?run=' + encodeURIComponent(a.dataset.run))) loadFiles();
});
poll();
</script>
</body>
</html>
)HTML";
