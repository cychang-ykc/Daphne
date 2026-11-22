'use strict';
// Daphne as a macOS MIKU extension module (mac/app/main/extensions.js loads <repo>/Extensions/Daphne/miku/ in a
// checkout, <MIKU.app>/Contents/Resources/ext/daphne/ installed): the Daphne playback core (設定 → 播放方案 → Daphne,
// engine.js on the daphne-helper process) and its page parts (web/: Daphne's own settings, the filter design
// progress, its row in 關於; the same files as the Windows extension, src/Daphne.Extension/web). The macOS side of
// src/Daphne.Extension/DaphneExtension.cs.
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { DaphneEngine } = require('./engine');
const { HelperClient, helperPath } = require('./helper-client');
const SM = require('./settings-map');

const opts = new SM.DaphneOptions();
let host = null, engine = null, settings = null, version = null;

/** A helper of its own for a call that needs no player (the filter cache), when no engine runs one. */
async function withHelper(fn) {
  if (engine && engine.client && engine.client.running) return fn(engine.client);
  const exe = helperPath(__dirname);
  if (!exe) return null;
  const c = new HelperClient(exe, m => host && host.log(m));
  c.on('hello', m => { version = m.version; });
  c.start();
  try { return await fn(c); } finally { await c.stop(); }
}

/** The Daphne commit this module comes from (commit.txt in an installed module, else the checkout's HEAD), for 關於. */
const commit = (() => {
  try { return fs.readFileSync(path.join(__dirname, 'commit.txt'), 'utf8').trim() || null; } catch { }
  try { return execFileSync('git', ['-C', path.join(__dirname, '..'), 'rev-parse', '--short', 'HEAD'], { timeout: 3000, encoding: 'utf8' }).trim() || null; } catch { }
  return null;
})();

async function cacheDir() {
  const r = await withHelper(c => c.request('cache_dir', {}, 5000));
  return r && r.ok && r.dir ? r.dir : null;
}

/** The filter cache folder with the number of designs in it and their size (關於). */
async function cacheInfo() {
  try {
    const dir = await cacheDir();
    if (!dir) return { dir: null, files: 0, bytes: 0 };
    let files = 0, bytes = 0;
    try {
      for (const f of fs.readdirSync(dir)) {
        const st = fs.statSync(path.join(dir, f));
        if (st.isFile()) files++, bytes += st.size;
      }
    } catch { }
    return { dir, files, bytes };
  } catch (e) { host && host.log('cache info: ' + e.message); return null; }
}

/** Everything but the filters the current settings use, down to limitMb. */
async function prune(limitMb) {
  const s = settings ? SM.build(host, settings, opts, null) : {};
  const r = await withHelper(c => c.request('cache_prune', { settings: s, limit_mb: limitMb }, 60000));
  if (r && r.ok && r.bytes < 0) host && host.log('cache prune: settings did not parse');
}

/** Tube Color's controls from the core (doc/N28), asked once; null from a helper without color_info. */
let colorAsked = false;
async function colorInfo() {
  if (!colorAsked) {
    colorAsked = true;
    const r = await withHelper(c => c.request('color_info', {}, 5000)).catch(() => null);
    SM.setColorInfo(r && r.ok && r.info ? r.info : null);
  }
  return SM.getColorInfo();
}

async function state() {
  if (!version) await withHelper(c => c.request('hello', {}, 5000).then(r => { if (r && r.ok) version = r.version; }));
  const live = engine && !engine.disposed;
  return {
    commit, version, cache: await cacheInfo(), now: live ? engine.nowPlaying() : null,
    color: await colorInfo(), colorNow: live ? engine.colorStatus() : null, options: opts.toJson(),
  };
}

function save() { host.setSetting('options', opts.toJson(true)); }

module.exports = {
  id: 'daphne',

  start(h) {
    host = h;
    const o = host.getSetting('options');
    if (o && typeof o === 'object') opts.merge(o);
  },

  stop() { },

  core: {
    id: 'daphne',
    name: 'Daphne',
    info: {
      autoSampleRate: false,   // the device follows each track (or the upsampler's rate) in exclusive mode; shared keeps the mixer's
      dsdModes: true,          // DoP in exclusive mode, else DSD → PCM
      maxDsd: true,            // DAC 最高 DSD
      dsdPcmRate: false,       // DSD → PCM at the rate the core picks
    },
    createEngine(s) {
      settings = s;
      colorInfo().catch(() => {});   // the drive limit for the settings the engine builds
      engine = new DaphneEngine(s, opts, host, __dirname);
      return engine;
    },
  },

  async handleRpc(method, a) {
    switch (method) {
      case 'state':
        return state();
      case 'colorPreview': {
        // the Tube Color panel's pictures (doc/N28): the core measures a stage of its own (the helper's color_preview)
        const drive = a && typeof a.drive === 'number' ? a.drive : opts.colorDrive;
        const r = await withHelper(c => c.request('color_preview', { settings: { color: { ...SM.colorOf(opts), enabled: true } }, drive }, 5000))
          .catch(() => null);
        return r && r.ok ? r.preview : null;
      }
      case 'openCache': {
        // the filter cache folder in Finder (created if it isn't there yet)
        const dir = await cacheDir();
        if (!dir) return false;
        fs.mkdirSync(dir, { recursive: true });
        if (host.openPath) await host.openPath(dir);
        return true;
      }
      case 'clearCache':
        await prune(0);
        return state();
      case 'set':
        // Daphne's own settings changed: in place / rebuild / restart as the core decides
        if (a && typeof a === 'object') {
          opts.merge(a);
          save();
          if (engine && !engine.disposed) await engine.applyOptions();
          if ('cacheLimitMb' in a) await prune(opts.cacheLimitMb);
        }
        return state();
      case 'bitPerfect':
        // MIKU's 套用 Bit-perfect 設定: Daphne's upsampler goes off, and with it apodizing, double-double and Tube Color
        opts.merge({ upsampler: 'off', apodize: false, doubleDouble: false, colorEnabled: false, colorBypass: false });
        save();
        if (engine && !engine.disposed) await engine.applyOptions();
        return state();
      default:
        return null;
    }
  },
};
