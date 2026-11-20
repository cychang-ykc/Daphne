// The Mac shell (miku/engine.js) without a helper: a fake client answers its requests. Run by CI (build.yml, linux):
//   node tests/miku_engine.js
'use strict';
const path = require('path');
const assert = require('assert');
const root = path.join(__dirname, '..');
const { DaphneEngine } = require(path.join(root, 'miku', 'engine.js'));
const SM = require(path.join(root, 'miku', 'settings-map.js'));

const sleep = ms => new Promise(r => setTimeout(r, ms));

// N26 R13: two changes of the DSD mode on a playing DSF file, the second while the first reload runs. The file must end
// loaded with the second choice, and loadedDsd must say what was sent.
async function dsdModeTwice() {
  const sent = [];
  const client = {
    running: true,
    start() {},
    request(cmd, args) {
      if (cmd === 'load') {
        sent.push(args.settings.output.dsd);
        return sleep(200).then(() => ({ ok: true, code: 0 }));   // a reload takes a while
      }
      if (cmd === 'state') return Promise.resolve({ ok: true, state: { duration: 10, report: { source: { dsd_rate: 2822400 } } } });
      return Promise.resolve({ ok: true });
    },
  };
  const s = { outputMode: 'coreaudio-exclusive', dsdMode: 'dop' };
  const e = new DaphneEngine(s, new SM.DaphneOptions(), {}, root);
  e.client = client;
  e.applyReport = () => {};
  e.buildSignal = () => null;
  e.queueNext = () => {};
  const t = { path: '/music/a.dsf' };
  await e.load(t, 0, true);   // DoP
  s.dsdMode = 'pcm';
  const first = e.applyOutput();   // reload as PCM ...
  await sleep(50);
  s.dsdMode = 'dop';
  await e.applyOutput();           // ... and back to DoP while it runs
  await first;
  await sleep(600);
  assert.strictEqual(sent[sent.length - 1], 'dop', `loads sent: ${sent.join(' -> ')}`);
  assert.strictEqual(e.loadedDsd, e.dsdKey(t));
  console.log(`PASS  two DSD mode changes in a row: loads sent ${sent.join(' -> ')}`);
}

// Tube Color's panel (doc/N28): the options -> Daphne's color settings. One invalid field falls back alone; the bypass
// is drive 0 with the slider's value kept (and not saved); the drive stops at the core's limit; bias and load are the
// reference ones until M3; the trim 0 is "auto".
function colorMapping() {
  SM.setColorInfo({ available: true, drive_max: 2.0, presets: [] });
  const o = new SM.DaphneOptions();
  let c = SM.build({}, { outputMode: 'coreaudio-exclusive' }, o, null).color;
  assert.deepStrictEqual(c, { enabled: false, model: 'fixed-point', preset: 'set-300b', drive: 1, feedback_db: 6, bias: 'standard', load_ohms: 8, makeup_gain: 'auto' });
  o.merge({ colorEnabled: true, colorPreset: 'pp-el34-ul', colorDrive: 1.35, colorFeedbackDb: 7, colorTrimDb: 1.54 });
  assert.strictEqual(o.colorFeedbackDb, 6);   // 7 is no step: its default, the others as given
  assert.strictEqual(o.colorPreset, 'pp-el34-ul');
  assert.strictEqual(o.colorTrimDb, 1.5);
  c = SM.colorOf(o);
  assert.strictEqual(c.drive, 1.35);
  assert.strictEqual(c.makeup_gain, 1.5);
  o.merge({ colorBypass: true });
  assert.strictEqual(SM.colorOf(o).drive, 0);          // bypass: idle ...
  assert.strictEqual(o.colorDrive, 1.35);              // ... the slider keeps its value
  assert.strictEqual(o.toJson().colorBypass, true);
  assert.ok(!('colorBypass' in o.toJson(true)));       // not saved
  o.merge({ colorBypass: false, colorDrive: 3.9 });
  assert.strictEqual(SM.colorOf(o).drive, 2.0);        // the core's drive_max
  o.merge({ colorDrive: 'x', colorPreset: 'set-1000', colorEnabled: 'yes' });
  assert.strictEqual(o.colorDrive, 1.0);
  assert.strictEqual(o.colorPreset, 'set-300b');
  assert.strictEqual(o.colorEnabled, false);
  SM.setColorInfo(null);
  console.log('PASS  Tube Color options -> settings (bypass, drive limit, single-field fallback)');
}

// The panel's status line: the core's path report and the chain
function colorStatus() {
  const e = new DaphneEngine({ outputMode: 'coreaudio-exclusive' }, new SM.DaphneOptions(), {}, root);
  assert.deepStrictEqual(e.colorStatus(), { playing: false, lastEvent: null });
  e.loaded = true;
  e.report = { chain: ['resampler', 'tube color', 'quantizer'], reason: 'DSP active (Tube Color)', filter: 'x | Tube Color idle (drive 0)' };
  e.colorEvent = 'DSD remodulated (Tube Color)';
  assert.deepStrictEqual(e.colorStatus(), { playing: true, inChain: true, reason: 'DSP active (Tube Color)', report: ['Tube Color idle (drive 0)'], lastEvent: 'DSD remodulated (Tube Color)' });
  console.log('PASS  Tube Color status for the panel');
}

(async () => {
  await dsdModeTwice();
  colorMapping();
  colorStatus();
})().catch(err => {
  console.error('FAIL ', err.message);
  process.exit(1);
});
