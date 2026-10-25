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

(async () => {
  await dsdModeTwice();
})().catch(err => {
  console.error('FAIL ', err.message);
  process.exit(1);
});
