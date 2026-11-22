// The page's Tube Color panel (web/daphne.js, doc/N28) in Node: a minimal fake DOM and MIKU's page API. Run by CI
// (build.yml, linux): node tests/miku_page.js
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const path = require('path');
const src = fs.readFileSync(process.argv[2] || path.join(__dirname, '..', 'src', 'Daphne.Extension', 'web', 'daphne.js'), 'utf8');

class El {
  constructor(tag, attrs) {
    this.tag = tag; this.attrs = {}; this.children = []; this.style = {}; this.disabled = false; this.parent = null;
    this.classList = { set: new Set(), add: (...c) => c.forEach(x => this.classList.set.add(x)), remove: c => this.classList.set.delete(c),
      contains: c => this.classList.set.has(c) };
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') String(v).split(/\s+/).filter(Boolean).forEach(c => this.classList.add(c));
      else if (k === 'style') Object.assign(this.style, v);
      else if (k.startsWith('on')) this[k] = v;
      else if (k === 'html') this.html = v;
      else { this.attrs[k] = v; if (k in { value: 1, min: 1, max: 1, step: 1, type: 1, title: 1 }) this[k] = v; if (k === 'disabled') this.disabled = !!v; }
    }
  }
  append(...xs) { for (const x of xs.flat()) if (x != null) { if (typeof x === 'object') x.parent = this; this.children.push(x); } }
  replaceChildren(...xs) { this.children = []; this.append(...xs); }
  get isConnected() { return true; }
  get textContent() { return this.children.map(c => (typeof c === 'object' ? c.textContent : String(c))).join(''); }
  set textContent(v) { this.children = [String(v)]; }
  all() { const out = []; for (const c of this.children) if (typeof c === 'object') out.push(c, ...c.all()); return out; }
  querySelectorAll(sel) {
    const tags = sel.split(',').map(s => s.trim());
    return this.all().filter(e => tags.includes(e.tag));
  }
  matches() { return false; }
}
const h = (tag, attrs, ...kids) => { const e = new El(tag, attrs); e.append(...kids); return e; };

const calls = [];
let state;
const timers = [];
const intervals = [];
const ctx = {
  console, h, setTimeout: (f, ms) => { timers.push(f); return timers.length; }, clearTimeout: () => {},
  setInterval: (f, ms) => { intervals.push(f); return intervals.length; }, clearInterval: () => {},
  document: { contains: () => true, createElement: () => ({ content: { firstChild: null } }), body: new El('body') },
  I18N: { lang: 'en' }, Router: { render: () => { ctx.rerendered = (ctx.rerendered || 0) + 1; } }, toast: () => {},
  App: { settings: { outputMode: 'exclusive', fixedRate: 0, maxDsd: 0 } },
  MikuExt: { register: m => { ctx.module = m; } },
};
vm.createContext(ctx);
vm.runInContext(src, ctx);

// what dc_color_preview answers, made up: H2 grows with the drive, the response a gentle tilt
let previewAvailable = true;
function preview(drive) {
  if (!previewAvailable) return { available: false };
  return { available: true, preset: 'set-300b', load: 'bookshelf-vented-smooth', drive, signal: { hz: 1000, dbfs_rms: -18 },
    harmonics: [2, 3, 4, 5, 6, 7].map(n => ({ n, db: -49 - 12 * (n - 2) + 20 * Math.log10(Math.max(drive, 1e-3)), pct: 100 * Math.pow(10, (-49 - 12 * (n - 2)) / 20) })),
    response: { hz: [20, 100, 1000, 10000, 20000], db: [-0.8, -0.6, 0, -2.4, -3.6] } };
}

function apiFor(info, colorNow, options) {
  state = { options, color: info, colorNow };
  const settings = {};
  return {
    settings,
    rpc: async (m, a) => {
      calls.push([m, a]);
      if (m === 'set') Object.assign(state.options, a);
      if (m === 'colorPreview') return preview(a.drive);   // the core's pictures (dc_color_preview)
      return JSON.parse(JSON.stringify(state));
    },
    on: () => {}, signalPath: () => {}, addSettings: (where, f) => { settings[where] = f; },
  };
}
const ui = {
  core: { id: 'daphne' },
  section: () => new El('section'),
  field: (t, s, c) => h('div', { class: 'field' }, h('b', null, t), s ? h('small', null, s) : null, h('div', { class: 'ctl' }, c)),
  select: (opts, v, f) => { const e = h('select', { value: v }); e.opts = opts; e.onchange = () => f(e.value); return e; },
  sw: (v, f) => { const e = h('input', { type: 'checkbox', value: v }); e.onclick = () => f(!v); return e; },
  hide: () => {},
};
const info = {
  available: true, curve: 'test', drive_max: 2, makeup_trim_max_db: 6, feedback_db: [0, 6, 12, 20, 30], load_ohms: [4, 8, 16],
  operating_point_adjustable: false, default_load: 'bookshelf-vented-smooth',
  loads: [{ id: 'bookshelf-vented-smooth', label: 'Bookshelf speaker (its tilt, no resonance)' }, { id: 'resistor-8', label: '8 ohm resistor (the amplifier alone)' }],
  presets: [{ id: 'set-300b', label: '300B single-ended', push_pull: false }],   // v1 (doc/N28)
};
// a later version's core, with more amplifiers (the menu, the push-pull feedback)
const laterInfo = { ...info, presets: [...info.presets, { id: 'set-845', label: '845 single-ended', push_pull: false },
                                       { id: 'pp-el34-ul', label: 'EL34 push-pull (ultralinear)', push_pull: true }] };
const eq = (a, b) => assert.strictEqual(JSON.stringify(a), JSON.stringify(b));
const tick = () => new Promise(r => setImmediate(r));

function colorGroup(card) {
  return card.all().find(e => e.classList.contains('dp-color'));
}
function fieldByTitle(g, title) {
  return g.all().find(e => e.classList.contains('field') && e.children[0].textContent === title);
}

(async () => {
  // 1. available, 300B, playing idle via bypass
  let api = apiFor(info, { playing: true, inChain: true, reason: 'DSP active (Tube Color)', report: ['Tube Color idle (drive 0)'], lastEvent: null },
    { colorEnabled: true, colorPreset: 'set-300b', colorDrive: 1.35, colorFeedbackDb: 6, colorTrimDb: 0, colorBypass: true, upsampler: 'off' });
  await ctx.module.init(api);
  let card = new El('card');
  ui.section = () => card;
  api.settings.core(card, ui);
  let g = colorGroup(card);
  assert.ok(g, 'the Tube Color group is drawn');
  assert.ok(!g.classList.contains('dp-off'));
  for (const t of ['Tube Color', 'Amplifier', 'Strength', 'Level', 'Bypass (A/B)']) assert.ok(fieldByTitle(g, t), t);
  assert.ok(!fieldByTitle(g, 'Feedback'), 'no feedback for 300B');
  assert.ok(!g.all().some(e => e.tag === 'b' && /bias|tap/i.test(e.textContent)), 'no bias / output tap (M3; the Load menu is what it drives)');
  const strength = fieldByTitle(g, 'Strength').all().find(e => e.tag === 'input');
  assert.strictEqual(strength.max, 200);
  assert.strictEqual(strength.value, 135);
  const status = g.all().find(e => e.classList.contains('dp-color-status'));
  assert.match(status.textContent, /Bypassed: the stage idles/);
  assert.match(status.textContent, /Path: DSP active \(Tube Color\)/);
  // drag the slider: sends at most every 150 ms, then where it stops
  timers.length = 0;   // (the pictures' first request: tested below)
  calls.length = 0;
  strength.value = 150; strength.oninput(); strength.value = 160; strength.oninput();
  assert.strictEqual(calls.length, 0);
  timers.shift()();   // the 150 ms timer
  await tick();
  eq(calls[0], ['set', { colorDrive: 1.6 }]);
  strength.value = 170; strength.onchange();
  await tick();
  eq(calls.find(c => c[0] === 'set' && c[1].colorDrive === 1.7), ['set', { colorDrive: 1.7 }]);
  // bypass button toggles
  calls.length = 0;
  const byp = fieldByTitle(g, 'Bypass (A/B)').all().find(e => e.tag === 'button');
  assert.match(byp.textContent, /Bypassed/);
  await byp.onclick();
  eq(calls[0], ['set', { colorBypass: false }]);
  // level trim
  calls.length = 0;
  const lvl = fieldByTitle(g, 'Level').all().find(e => e.tag === 'input');
  assert.strictEqual(lvl.min, -6); assert.strictEqual(lvl.max, 6); assert.strictEqual(lvl.step, 0.5);
  lvl.value = -2.5; lvl.onchange(); await tick();
  eq(calls[0], ['set', { colorTrimDb: -2.5 }]);
  // one amplifier (v1): said as text, no menu of one
  assert.ok(!fieldByTitle(g, 'Amplifier').all().some(e => e.tag === 'select'), 'no menu for one amplifier');
  assert.match(fieldByTitle(g, 'Amplifier').textContent, /300B single-ended/);
  assert.match(fieldByTitle(g, 'Amplifier').textContent, /later versions/);
  // the load menu: the core's loads, the default where none is set; a choice is sent as colorLoad
  const loadSel = fieldByTitle(g, 'Load').all().find(e => e.tag === 'select');
  eq(loadSel.opts.map(x => x[0]), ['bookshelf-vented-smooth', 'resistor-8']);
  assert.strictEqual(loadSel.value, 'bookshelf-vented-smooth');
  calls.length = 0;
  loadSel.value = 'resistor-8'; loadSel.onchange(); await tick();
  eq(calls[0], ['set', { colorLoad: 'resistor-8' }]);
  console.log('PASS  available: fields, slider throttling, bypass, level, load, status');

  // 2. a later version with a push-pull amplifier: the amplifier menu and the feedback field (hidden in v1)
  api = apiFor(laterInfo, { playing: false, lastEvent: null }, { colorEnabled: true, colorPreset: 'pp-el34-ul', colorDrive: 1, colorFeedbackDb: 30, colorTrimDb: 0 });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  g = colorGroup(card);
  const fb = fieldByTitle(g, 'Feedback').all().find(e => e.tag === 'select');
  eq(fb.opts.map(x => x[0]), [0, 6, 12, 20, 30]);
  assert.strictEqual(fb.value, 30);
  eq(fieldByTitle(g, 'Amplifier').all().find(e => e.tag === 'select').opts.map(x => x[1]),
     ['300B single-ended', '845 single-ended', 'EL34 push-pull (ultralinear)']);   // no "(not yet)" marks
  assert.match(g.all().find(e => e.classList.contains('dp-color-status')).textContent, /nothing playing/);
  console.log('PASS  push-pull: feedback steps from the core');

  // 3. unavailable (a release build before M2): shown disabled, says so
  api = apiFor({ ...info, available: false, curve: 'none' }, null, { colorEnabled: true, colorPreset: 'set-300b', colorDrive: 1 });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  g = colorGroup(card);
  assert.ok(g.classList.contains('dp-off'));
  const ctls = g.querySelectorAll('input, select, button');
  assert.ok(ctls.length >= 5);
  assert.ok(ctls.every(e => e.disabled), 'every control disabled');
  assert.match(g.all().find(e => e.classList.contains('dp-color-status')).textContent, /unavailable in this build/);
  assert.match(fieldByTitle(g, 'Tube Color').textContent, /Unavailable in this build/);
  console.log('PASS  unavailable: disabled, "unavailable" shown');

  // 3c. the status line follows the core in and out of idle on the page's refresh (doc/N28)
  intervals.length = 0;
  api = apiFor(info, { playing: true, inChain: true, reason: 'DSP active (Tube Color)', report: [], lastEvent: null },
    { colorEnabled: true, colorPreset: 'set-300b', colorDrive: 1, colorBypass: false });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  g = colorGroup(card);
  const line = g.all().find(e => e.classList.contains('dp-color-status'));
  for (const idle of [false, true, false, true]) {
    state.colorNow = idle ? { playing: true, inChain: false, reason: '', report: ['Tube Color idle (drive 0)'], lastEvent: null }
                          : { playing: true, inChain: true, reason: 'DSP active (Tube Color)', report: [], lastEvent: null };
    for (const f of intervals) f();   // the page's timers: the refresh among them
    await tick(); await tick();
    if (idle) assert.match(line.textContent, /Idle/, 'idle shown');
    else { assert.match(line.textContent, /Active/, 'running shown'); assert.doesNotMatch(line.textContent, /Idle/); }
  }
  console.log('PASS  the status line: idle -> running -> idle -> running');

  // 3e. the level alignment (an M2 experiment): the menu from the core's modes (off where none is set), a choice sent as
  // colorAlign; the status line with the track's measures, the gain and the effective drive, or that it is scanning
  const alInfo = { ...info, default_align: 'conservative', align_refs: { lufs: -14.28, p999_db: -0.81 },
    aligns: [{ id: 'conservative', label: 'Conservative (the larger cut; default)' }, { id: 'loudness', label: 'Loudness (integrated LUFS)' },
             { id: 'near-peak', label: 'Near-peak (99.9th percentile)' }, { id: 'off', label: 'Off' }] };
  const alStatus = align => ({ playing: true, inChain: true, reason: 'DSP active (Tube Color)', report: [], lastEvent: null, align });
  api = apiFor(alInfo, alStatus({ status: 'ready', mode: 'loudness', lufs: -4.1, p999_db: -1.24, gain_db: -10.18, drive: 2, drive_eff: 0.6204 }),
    { colorEnabled: true, colorPreset: 'set-300b', colorDrive: 2, colorAlign: 'loudness' });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  g = colorGroup(card);
  const alSel = fieldByTitle(g, 'Level align').all().find(e => e.tag === 'select');
  eq(alSel.opts.map(x => x[0]), ['conservative', 'loudness', 'near-peak', 'off']);
  assert.strictEqual(alSel.value, 'loudness');
  let alLine = g.all().find(e => e.classList.contains('dp-color-status')).textContent;
  assert.ok(alLine.includes('Track -4.1 LUFS, 99.9 % -1.2 dBFS · align (loudness) -10.2 dB · drive 200 % → 62 % effective'), alLine);
  calls.length = 0;
  alSel.value = 'conservative'; alSel.onchange(); await tick();
  eq(calls[0], ['set', { colorAlign: 'conservative' }]);
  for (const [st, want] of [[{ status: 'scanning', mode: 'near-peak' }, 'Level align (near-peak): scanning the track; unaligned until it is done'],
                            [{ status: 'failed', mode: 'loudness' }, 'could not be scanned'],
                            [{ status: 'ready', mode: 'off', lufs: null, p999_db: null, gain_db: 0, drive: 1, drive_eff: 1 }, 'Track silence, 99.9 % silence']]) {
    api = apiFor(alInfo, alStatus(st), { colorEnabled: true, colorPreset: 'set-300b', colorDrive: 1 });
    await ctx.module.init(api);
    card = new El('card'); ui.section = () => card;
    api.settings.core(card, ui);
    g = colorGroup(card);
    alLine = g.all().find(e => e.classList.contains('dp-color-status')).textContent;
    assert.ok(alLine.includes(want), `${st.status}: ${alLine}`);
    if (st.status === 'ready') assert.strictEqual(fieldByTitle(g, 'Level align').all().find(e => e.tag === 'select').value, 'conservative');
  }
  console.log('PASS  the level alignment: menu and status line');

  // 4. WASAPI shared: the card shows Tube Color only
  ctx.App.settings.outputMode = 'shared';
  api = apiFor(info, null, { colorEnabled: false });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  assert.notStrictEqual(card.style.display, 'none');
  assert.ok(colorGroup(card));
  console.log('PASS  shared mode: Tube Color still offered');

  // 5. the pictures (doc/N28): asked from the core (colorPreview) at the slider's drive, H2..H7 as six bars and the
  // response as a line against the dashed bypass; the condition said, intermodulation said; "This track" asks at the
  // track's effective drive; a burst of changes asks once (250 ms); a core without the preview shows no pictures
  const chartsOf = c => colorGroup(c).all().find(e => e.classList.contains('dp-color-charts'));
  timers.length = 0; calls.length = 0;
  const trackNow = { playing: true, inChain: true, reason: 'DSP active (Tube Color)', report: [], lastEvent: null,
    align: { status: 'ready', mode: 'conservative', lufs: -4.1, p999_db: -0.3, gain_db: -10.18, drive: 1.0, drive_eff: 0.31 } };
  api = apiFor(info, trackNow, { colorEnabled: true, colorPreset: 'set-300b', colorDrive: 1.0, colorLoad: 'bookshelf-vented-smooth' });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  assert.strictEqual(timers.length, 1, 'one request scheduled');
  timers.shift()(); await tick(); await tick();
  eq(calls.filter(c => c[0] === 'colorPreview'), [['colorPreview', { drive: 1 }]]);
  let box = chartsOf(card);
  const svgs = box.all().filter(e => e.classList.contains('dp-chart')).map(e => e.html);
  assert.strictEqual(svgs.length, 2);
  assert.strictEqual((svgs[0].match(/class="dp-bar"/g) || []).length, 6, 'six bars');
  assert.match(svgs[0], />H2</); assert.match(svgs[0], />H7</); assert.match(svgs[0], /-49 dB/);
  assert.match(svgs[1], /class="dp-response"/); assert.match(svgs[1], /class="dp-bypass"/);
  assert.match(box.textContent, /1 kHz sine at the calibration level \(-18 dBFS RMS\) · drive 100 %/);
  assert.match(box.textContent, /intermodulation/);
  // "This track": the effective drive
  calls.length = 0;
  const tabs = box.all().filter(e => e.tag === 'button');
  eq(tabs.map(b => b.textContent), ['Calibration level', 'This track']);
  tabs[1].onclick();
  assert.strictEqual(timers.length, 1);
  timers.shift()(); await tick(); await tick();
  eq(calls.filter(c => c[0] === 'colorPreview'), [['colorPreview', { drive: 0.31 }]]);
  assert.match(chartsOf(card).textContent, /this track: 31 % effective/);
  // a burst of refreshes with nothing changed asks nothing; a change asks once
  calls.length = 0;
  for (const f of intervals) f();
  await tick();
  assert.strictEqual(timers.length, 0, 'nothing changed: no request');
  // a core without the preview: the tabs, no pictures
  previewAvailable = false;
  api = apiFor(info, trackNow, { colorEnabled: true, colorPreset: 'set-300b', colorDrive: 1.2 });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  timers.shift()(); await tick(); await tick();
  assert.strictEqual(chartsOf(card).all().filter(e => e.classList.contains('dp-chart')).length, 0);
  previewAvailable = true;
  console.log('PASS  the pictures: from the core, six bars and the response, this track, throttled');
})().catch(e => { console.error('FAIL ', e.stack || e.message); process.exit(1); });
