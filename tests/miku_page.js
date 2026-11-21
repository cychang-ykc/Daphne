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
const ctx = {
  console, h, setTimeout: (f, ms) => { timers.push(f); return timers.length; }, clearTimeout: () => {}, setInterval: () => 1, clearInterval: () => {},
  document: { contains: () => true, createElement: () => ({ content: { firstChild: null } }), body: new El('body') },
  I18N: { lang: 'en' }, Router: { render: () => { ctx.rerendered = (ctx.rerendered || 0) + 1; } }, toast: () => {},
  App: { settings: { outputMode: 'exclusive', fixedRate: 0, maxDsd: 0 } },
  MikuExt: { register: m => { ctx.module = m; } },
};
vm.createContext(ctx);
vm.runInContext(src, ctx);

function apiFor(info, colorNow, options) {
  state = { options, color: info, colorNow };
  const settings = {};
  return {
    settings,
    rpc: async (m, a) => {
      calls.push([m, a]);
      if (m === 'set') Object.assign(state.options, a);
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
  operating_point_adjustable: false,
  presets: [{ id: 'set-300b', label: '300B single-ended', push_pull: false }, { id: 'set-2a3', label: '2A3 single-ended', push_pull: false },
            { id: 'set-845', label: '845 single-ended', push_pull: false }, { id: 'pp-el34-ul', label: 'EL34 push-pull (ultralinear)', push_pull: true }],
};
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
  assert.ok(!g.all().some(e => e.tag === 'b' && /bias|load/i.test(e.textContent)), 'no bias / load (M3)');
  const strength = fieldByTitle(g, 'Strength').all().find(e => e.tag === 'input');
  assert.strictEqual(strength.max, 200);
  assert.strictEqual(strength.value, 135);
  const status = g.all().find(e => e.classList.contains('dp-color-status'));
  assert.match(status.textContent, /Bypassed: the stage idles/);
  assert.match(status.textContent, /Path: DSP active \(Tube Color\)/);
  // drag the slider: sends at most every 150 ms, then where it stops
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
  console.log('PASS  available: fields, slider throttling, bypass, level, status');

  // 2. push-pull: the feedback field
  api = apiFor(info, { playing: false, lastEvent: null }, { colorEnabled: true, colorPreset: 'pp-el34-ul', colorDrive: 1, colorFeedbackDb: 30, colorTrimDb: 0 });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  g = colorGroup(card);
  const fb = fieldByTitle(g, 'Feedback').all().find(e => e.tag === 'select');
  eq(fb.opts.map(x => x[0]), [0, 6, 12, 20, 30]);
  assert.strictEqual(fb.value, 30);
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

  // 3b. a build with set-300b's curve only: another preset is marked and said to play as off
  const partial = { ...info, curve: 'tubes', presets: info.presets.map(p => ({ ...p, available: p.id === 'set-300b' })) };
  api = apiFor(partial, { playing: true, inChain: false, reason: '', report: ['Tube Color unavailable for this preset (its curve comes later in M2)'] },
    { colorEnabled: true, colorPreset: 'set-845', colorDrive: 1 });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  g = colorGroup(card);
  assert.ok(!g.classList.contains('dp-off'));
  const amp = fieldByTitle(g, 'Amplifier').all().find(e => e.tag === 'select');
  eq(amp.opts.map(x => x[1]), ['300B single-ended', '2A3 single-ended (not yet)', '845 single-ended (not yet)', 'EL34 push-pull (ultralinear) (not yet)']);
  assert.match(fieldByTitle(g, 'Amplifier').textContent, /not in this build yet/);
  assert.match(g.all().find(e => e.classList.contains('dp-color-status')).textContent, /not in this build yet/);
  console.log('PASS  a preset without its curve: marked, said');

  // 4. WASAPI shared: the card shows Tube Color only
  ctx.App.settings.outputMode = 'shared';
  api = apiFor(info, null, { colorEnabled: false });
  await ctx.module.init(api);
  card = new El('card'); ui.section = () => card;
  api.settings.core(card, ui);
  assert.notStrictEqual(card.style.display, 'none');
  assert.ok(colorGroup(card));
  console.log('PASS  shared mode: Tube Color still offered');
})().catch(e => { console.error('FAIL ', e.stack || e.message); process.exit(1); });
