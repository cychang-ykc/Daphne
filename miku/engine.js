'use strict';
// The Daphne playback core for macOS MIKU: the backend its AudioEngine facade drives (mac/app/main/audio.js), on the
// daphne-helper process (helper/main.cpp), which plays the file itself: decoding, the processing chain (resampling
// filter, DSD modulator), Core Audio output (exclusive in hog mode, or shared), DoP, gapless. The macOS side of
// src/Daphne.Extension/DaphneEngine.cs.
//
// MIKU features on this core:
//   • output mode / device → Daphne's output (coreaudio_exclusive / coreaudio_shared); DSD 播放方式 → output.dsd (DoP
//     in exclusive mode, else PCM); DAC 最高 DSD → output.max_dsd; 緩衝大小 → output.buffer_ms
//   • digital volume, EQ, crossfeed → Daphne (changes glide in place); ReplayGain → Daphne's gain stage
//   • YouTube Music: played by its page through the system output (supportsLive false); stop({ release: true }) /
//     release() let go of the device (the core puts the rate and DAC format back and releases hog mode). A plain
//     stop() and the end of the last track keep it in exclusive mode, as MIKU's own output does
//   • not available on this core: balance, invert, the logo's sound in exclusive mode
const { EventEmitter } = require('events');
const { HelperClient, helperPath } = require('./helper-client');
const SM = require('./settings-map');
const { buildSignal, nowPlaying } = require('./signal');

const DC_ERR_DEVICE = -4;
const DC_ERR_CANCELLED = -7;   // the load was cancelled by a stop: not a failure
// The core's messages are English (they go to the log as they are); the common ones are shown to the user in MIKU's
// language, anything else as it came
const Readable = [
  [/cannot open (FLAC|WAV\/AIFF|MP3) file|FFmpeg: (cannot open file|no stream info|no audio stream)|not a DSF or DFF file|file too short/, () => '檔案損毀，或不是支援的音訊格式'],
  [/cannot map (\d+) source channels to (\d+)/, (m, a, b) => `裝置有 ${b} 個聲道，無法播放 ${a} 聲道的檔案`],
  [/is in exclusive use by another application/, () => '另一個程式正在獨佔這個裝置'],
  [/does not offer exclusive \(hog\) mode/, () => '這個裝置不提供獨佔模式'],
];
const readable = err => { const s = String(err || ''); for (const [re, f] of Readable) { const m = s.match(re); if (m) return f(...m); } return s; };

/** The core's "designing ..." message in the page's words. */
function describeDesign(t) {
  if (!t) return '設計濾波器';
  let m = /designing the filter for (\d+) -> (\d+) Hz/.exec(t);
  if (m) return `${+(m[1] / 1000).toFixed(1)} → ${+(m[2] / 1000).toFixed(1)} kHz 濾波器`;
  m = /designing (.+?) in the background/.exec(t);
  return m ? m[1] : '設計濾波器';
}

class DaphneEngine extends EventEmitter {
  constructor(settings, options, host, moduleDir) {
    super();
    this.s = settings; this.opts = options; this.host = host || {}; this.dir = moduleDir;
    this.client = null; this.timer = null; this.disposed = false;
    this.track = null; this.next = null; this.pausedAt = null;
    this.loaded = false; this.playing = false; this.loading = false; this.loadFailed = false; this.lastFailureWasDevice = false;
    this.held = false;   // exclusive: the device kept with nothing loaded (stop without release, the end of the last track)
    this.pos = 0; this.duration = 0; this.report = null; this.signal = null;
    this.st = { rate: 0, playing: false, pos: 0, underruns: 0, clips: 0 };
    this.levels = { l: 0, r: 0 };
    this.devices = [];
    this.peekNext = null;
    this.loadEpoch = 0; this.restarts = [];
    this.design = { active: false, text: null, postedAt: 0 };
  }

  /** YouTube Music plays through its own page on the Mac (main.js: ytThroughCore off). */
  get supportsLive() { return false; }
  get isPlaying() { return this.loaded && this.playing; }
  get isLoaded() { return this.loaded; }
  get position() { return this.loaded ? this.pos : (this.pausedAt || 0); }
  /** The device is held (exclusive) while a track is loaded, playing or paused, and kept after it (stop() without
   *  release, the end of the last track) until release(): as MIKU's own Core Audio output (native-audio.js). */
  get holdsDevice() { return this.held || ((this.loaded || this.loading) && SM.exclusive(this.s)); }

  log(m) { try { this.host.log && this.host.log(m); } catch { } }

  // ───────────── the helper process ─────────────
  start() {
    const exe = helperPath(this.dir);
    if (!exe) { this.log('daphne-helper not found next to the module or in a build folder'); return; }
    this.client = new HelperClient(exe, m => this.log(m));
    this.client.on('event', m => this.onEvent(m.event || {}));
    this.client.on('design', m => this.onDesign(+m.progress));
    this.client.on('exit', why => this.onExit(why));
    this.client.start();
    this.timer = setInterval(() => this.poll(), 100);
  }

  /** The helper ended without being asked: a new one, and the loaded track again from where it was (3 times a minute at most). */
  onExit(why) {
    if (this.disposed) return;
    this.log('helper stopped: ' + why);
    const now = Date.now();
    this.restarts = this.restarts.filter(t => now - t < 60000);
    const t = this.track, pos = this.position, play = this.isPlaying, wasLoaded = this.loaded || this.loading;
    this.loaded = false; this.loading = false; this.playing = false;
    if (this.restarts.length >= 3) {
      this.emit('failed', 'Daphne 內核一再停止，請查看記錄檔');
      this.emit('changed');
      return;
    }
    this.restarts.push(now);
    this.held = false;   // the system released hog mode with the process
    this.client.start();
    if (t && wasLoaded) this.load(t, pos, play);
    else this.emit('changed');
  }

  /** State every 100 ms while a track is loaded: position, the report, the meter. */
  async poll() {
    if (!this.client || !this.client.running || !this.loaded || this.polling) return;
    this.polling = true;
    try {
      const r = await this.client.request('state', {}, 2000);
      const st = r && r.ok ? r.state : null;
      if (st && this.loaded) {
        this.pos = +st.position || 0;
        this.duration = +st.duration || 0;
        if (st.report) this.applyReport(st.report);
      }
      const m = await this.client.request('meter', {}, 2000);
      if (m && m.ok) this.levels = { l: Math.max(this.levels.l, m.l || 0), r: Math.max(this.levels.r, m.r || 0) };
    } finally { this.polling = false; }
  }

  applyReport(rep) {
    this.report = rep;
    if (rep.device_name) this.lastDevice = rep.device_name;
    this.st.rate = rep.device ? rep.device.rate : this.st.rate;
    this.st.playing = this.isPlaying; this.st.pos = this.pos;
    this.st.underruns = +rep.underruns || 0; this.st.clips = +rep.clipped || 0;
  }

  async refreshReport() {
    if (!this.client) return;
    const r = await this.client.request('state', {}, 5000);
    if (r && r.ok && r.state) {
      this.duration = +r.state.duration || 0;
      if (r.state.report) this.applyReport(r.state.report);
    }
  }

  onEvent(e) {
    switch (e.type) {
      case 'track': {   // gapless: the next track is heard now
        const t = this.next;
        this.next = null;
        if (!t) break;
        this.track = t;
        this.refreshReport().then(() => {
          this.signal = this.buildSignal();
          this.emit('trackStarted', t);
          this.queueNext();
          this.emit('changed');
        });
        break;
      }
      case 'ended':
        this.loaded = false; this.playing = false; this.pausedAt = 0;
        this.held = SM.exclusive(this.s);   // the core keeps the device once the buffer has played out (a moment later)
        this.emit('ended');
        this.emit('changed');
        break;
      case 'failed': {
        const m = e.error || 'playback failed';
        this.log(m);
        this.loaded = false; this.playing = false;
        this.syncHeld();
        this.emit('failed', 'Daphne 內核：' + readable(m));
        this.emit('changed');
        break;
      }
      case 'started':
      case 'changed':   // playback (re)started, or a new filter / processing chain plays: the signal path follows
        this.refreshReport().then(() => { this.signal = this.buildSignal(); this.emit('changed'); });
        break;
      case 'info':
        if (e.text) {
          this.log(e.text);
          if (e.text.startsWith('designing')) this.design.text = e.text;
        }
        break;
    }
  }

  /** A filter is being designed (first load, or a rebuild in the background): the page shows a progress popup (web/daphne.js). */
  onDesign(f) {
    if (!this.host.post) return;
    if (f < 1) {
      if (!this.design.active || Date.now() - this.design.postedAt > 200) {
        this.design.active = true; this.design.postedAt = Date.now();
        this.host.post('design', { progress: f, text: describeDesign(this.design.text), done: false });
      }
    } else if (this.design.active) {
      this.design.active = false;
      this.host.post('design', { progress: 1, text: describeDesign(this.design.text), done: true });
      this.design.text = null;
    }
  }

  // ───────────── transport ─────────────
  settingsFor(t) { return SM.build(this.host, this.s, this.opts, t); }
  /** How a DSD file goes out: DoP or PCM, and MIKU's DAC 最高 DSD. */
  dsdKey(t) { const o = this.settingsFor(t).output; return o.dsd + '|' + o.max_dsd; }
  isDsdFile(t) { return !!(this.report && this.report.source && this.report.source.dsd_rate > 0) || /\.(dsf|dff)$/i.test(t.path || ''); }

  async load(t, pos, play) {
    this.loadFailed = false; this.lastFailureWasDevice = false;
    if (!t) return;
    const epoch = ++this.loadEpoch;
    this.emit('loading', t);
    if (!this.client) return this.fail(0, '找不到 Daphne 的 helper 程序（daphne-helper）');
    if (!this.client.running) this.client.start();
    this.track = t; this.next = null; this.pausedAt = pos || 0;
    this.loaded = false; this.loading = true;
    const r = await this.client.request('load', { path: t.path, settings: this.settingsFor(t), seek: pos || 0, play: !!play });
    if (epoch !== this.loadEpoch) return;   // a newer load took its place
    this.loading = false;
    if (!r.ok && r.code === DC_ERR_CANCELLED) { this.log('load cancelled (stop)'); this.emit('changed'); return; }
    if (!r.ok) return this.fail(r.code, r.error || '');
    this.loaded = true; this.playing = !!play; this.pos = pos || 0; this.loadedDsd = this.dsdKey(t);
    await this.refreshReport();
    this.signal = this.buildSignal();
    this.queueNext();
    this.emit('changed');
  }

  /** Whether the core keeps a device now (the end of the last track keeps it in exclusive mode, a failure may not). */
  syncHeld() {
    if (!this.client || !this.client.running) { this.held = false; return; }
    this.client.request('state', {}, 2000).then(r => {
      const kept = !!(r && r.ok && r.kept);
      if (kept !== this.held && !this.loaded && !this.loading) { this.held = kept; this.emit('changed'); }
    });
  }

  fail(code, err) {
    this.lastFailureWasDevice = code === DC_ERR_DEVICE;
    this.syncHeld();
    this.loadFailed = true; this.loading = false; this.signal = null;
    this.log(`load failed (${code}): ${err}`);
    this.emit('failed', code === DC_ERR_DEVICE ? 'Daphne 內核無法開啟輸出裝置：' + readable(err) : 'Daphne 內核無法播放：' + readable(err));
    this.emit('changed');
  }

  /** Gapless: tell the core what follows (it continues without a gap when the format matches). */
  queueNext() {
    if (!this.client) return;
    if (!this.loaded || this.s.gapless === false) { this.next = null; this.client.request('next', { path: '' }); return; }
    const n = this.peekNext ? this.peekNext() : null;
    const live = n && this.host.isLive && this.host.isLive(n);
    if (!n || live || n.codec === 'CD' || !n.path) { this.next = null; this.client.request('next', { path: '' }); return; }
    this.next = n;
    // its own ReplayGain goes with it: in track mode a gapless album gets each track's gain at the boundary
    this.client.request('next', { path: n.path, gain: SM.replayGainDb(this.host, this.s, n) });
  }

  pause() {
    if (this.loaded) this.client.request('pause', { paused: true });
    this.playing = false;
    this.pausedAt = this.position;
    this.emit('changed');
  }

  resume() {
    if (this.loaded) { this.client.request('pause', { paused: false }); this.playing = true; this.emit('changed'); return; }
    if (this.track) return this.load(this.track, this.pausedAt || 0, true);
  }

  async seek(pos) {
    if (!this.loaded) { this.pausedAt = pos; return; }
    this.client.request('seek', { seconds: pos });
    this.pos = pos;
    this.emit('changed');
  }

  /**
   * Stops playback. In exclusive mode the DAC stays Daphne's (hog mode, rate, DAC format) unless `release` is asked
   * for: the next track starts on the same device (a new rate is set on it directly), and macOS does not move the
   * system output away and back in between. MIKU asks for the release when the listener leaves it with nothing
   * playing, plays YouTube, or quits (audio.js); else Daphne closes its output (rate and DAC format put back, hog
   * mode released).
   */
  stop({ release = false } = {}) {
    this.pausedAt = this.loaded ? this.position : null;
    ++this.loadEpoch;
    const hold = !release && SM.exclusive(this.s) && (this.loaded || this.loading || this.held);
    if (this.client) this.client.request('stop', { hold });
    this.held = hold && !!this.client;
    this.loaded = false; this.playing = false; this.loading = false;
    this.track = null; this.next = null; this.signal = null;
    this.emit('changed');
  }

  /** Lets go of a device kept with nothing loaded. */
  release() {
    if (!this.client || this.loaded || this.loading) return;
    this.held = false;
    this.client.request('stop');
    this.emit('changed');
  }

  invalidateNext() { this.queueNext(); }

  /** MIKU's or Daphne's settings changed: the core applies them in place / rebuilds / restarts the device. */
  apply() {
    if (!this.loaded || !this.track || !this.client) return Promise.resolve();
    return this.client.request('apply', { settings: this.settingsFor(this.track) }).then(r => { if (!r.ok) this.log('settings: ' + r.error); });
  }

  /** Output mode / device changed (AudioEngine.reconfigure): the core reopens the device where it was. */
  async applyOutput() {
    // a DSD file: the core picks DoP or PCM (and the DSD limit) only when it loads the file, so a new choice loads it
    // again where it was (an in-place apply would reopen the device on the same path)
    const t = this.track;
    if (this.loaded && t && this.isDsdFile(t) && this.loadedDsd !== this.dsdKey(t)) return this.load(t, this.position, this.isPlaying);
    // nothing loaded: the next load opens the new output
    if (this.loaded) { await this.apply(); await this.refreshReport(); this.signal = this.buildSignal(); }
    this.emit('changed');
  }

  applyOptions() { return this.applyOutput(); }
  applyVolume() { this.apply().then(() => { if (this.loaded) this.signal = this.buildSignal(); }); }
  applyDsp() { this.apply().then(() => { if (this.loaded) this.signal = this.buildSignal(); this.emit('changed'); }); }
  applyReplayGain() { this.applyDsp(); }
  async setDevice() { await this.applyOutput(); }
  refreshSignal() { if (this.loaded) this.signal = this.buildSignal(); }

  meter() {
    if (!this.loaded) return { l: 0, r: 0, clips: 0, underruns: 0 };
    const v = { l: this.levels.l, r: this.levels.r, clips: this.st.clips, underruns: this.st.underruns };
    this.levels = { l: 0, r: 0 };
    return v;
  }

  // ───────────── devices ─────────────
  async coreAudioDevices() {
    if (!this.client || !this.client.running) return [];
    const r = await this.client.request('devices', {}, 5000);
    return (r && r.ok && r.devices && r.devices.coreaudio) || [];
  }

  async listDevices() {
    const list = await this.coreAudioDevices();
    this.devices = list.map(d => ({ id: d.uid, name: d.name, isDefault: !!d.default }));
    return this.devices;
  }

  deviceName() {
    if (this.report && this.report.device_name) return this.report.device_name;
    const d = this.devices.find(x => x.id === this.s.deviceId) || this.devices.find(x => x.isDefault);
    return d ? d.name : '系統預設輸出';
  }

  /** The chosen device now, for MIKU's settings page (rate, channels, what holds it). */
  async probeDevice(id) {
    const list = await this.coreAudioDevices();
    // right after hog mode is released macOS has no system output for a moment: the device just played, else the first
    const d = list.find(x => x.uid === id) || list.find(x => x.default) || list.find(x => this.lastDevice && x.name === this.lastDevice) || list[0];
    if (!d) return null;
    const mine = this.client && this.client.proc ? this.client.proc.pid : -1;
    return {
      id: d.uid, name: d.name, rate: d.rate, channels: d.channels, rates: d.rates || [],
      physicalFormat: (d.rates || []).length ? '可用 ' + d.rates.map(r => +(r / 1000).toFixed(1)).join(' / ') + ' kHz' : '',
      ownerName: d.hog_pid >= 0 && d.hog_pid !== mine ? `pid ${d.hog_pid}` : '',
    };
  }

  // ───────────── signal path ─────────────
  buildSignal() { return buildSignal(this.host, this.s, this.opts, this.track, this.report); }
  nowPlaying() { return this.loaded ? nowPlaying(this.report) : null; }

  async dispose() {
    this.disposed = true;
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    if (this.client) await this.client.stop();
    this.loaded = false; this.track = null;
  }
}

module.exports = { DaphneEngine };
