'use strict';
// Daphne's own settings (upsampler, filter, DSD modulator, filter design), kept with the module's settings in MIKU's
// settings file, and MIKU's settings + those → Daphne's settings JSON (core/include/daphne/engine/settings.h) for
// one track. The macOS side of src/Daphne.Extension/DaphneOptions.cs; the page (web/daphne.js) applies its menus'
// limits, the core only checks ranges.

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const int = (v, d) => (Number.isFinite(+v) ? Math.round(+v) : d);

class DaphneOptions {
  constructor() {
    this.upsampler = 'off';        // off | pcm | dsd
    this.target = 'native';        // upsampler pcm: native | 2x | max | fixed
    this.fixedRate = null;         // target fixed: Hz (null: MIKU's 固定取樣率)
    this.taps = 2048;
    this.phase = 'intermediate';   // linear | intermediate | minimum | custom
    this.phaseCustom = 0.5;        // phase custom: 0 (linear) .. 1 (minimum)
    this.apodize = false;
    this.doubleDouble = false;     // double-double convolution
    this.order = 8;                // DSD modulator: 5 | 8
    this.osr = 128;                // DSD64 .. DSD1024
    this.dsdPcmLowpass = 24;       // DSD → PCM low-pass, kHz: 24 | 30 | 50
    this.dsdPcmLimiter = true;     // DSD → PCM: limiter instead of clipping above full scale
    this.threads = 0;              // filter design threads, 0 = automatic
    this.cacheLimitMb = 1024;      // filter cache limit: 500 | 1024 | 2048 MB
    // Tube Color (doc/N28). Bias and load are the presets' reference ones until M3 (standard, 8 ohm: colorOf)
    this.colorEnabled = false;
    this.colorPreset = 'set-300b';  // v1: set-300b only (doc/N28)
    this.colorDrive = 1.0;          // 0 .. the core's drive_max (1.0 = 100 %)
    this.colorFeedbackDb = 6;       // pp-el34-ul: 0 | 6 | 12 | 20 | 30
    this.colorTrimDb = 0;           // makeup_gain's trim, -6 .. 6 dB on top of auto (0: auto)
    this.colorLoad = 'bookshelf-vented-smooth';   // what it drives: bookshelf-vented-smooth | resistor-8
    this.colorAlign = 'conservative';   // the level alignment: conservative | loudness | near-peak | off
    // the panel's bypass (A/B): the stage idles at drive 0, the slider keeps its value. Not saved
    this.colorBypass = false;
  }

  /** The options as the page reads them; save: as kept in MIKU's settings (no bypass). */
  toJson(save = false) {
    const { upsampler, target, fixedRate, taps, phase, phaseCustom, apodize, doubleDouble, order, osr, dsdPcmLowpass, dsdPcmLimiter, threads, cacheLimitMb,
      colorEnabled, colorPreset, colorDrive, colorFeedbackDb, colorTrimDb, colorBypass, colorLoad, colorAlign } = this;
    const j = { upsampler, target, fixedRate, taps, phase, phaseCustom, apodize, doubleDouble, order, osr, dsdPcmLowpass, dsdPcmLimiter, threads, cacheLimitMb,
      colorEnabled, colorPreset, colorDrive, colorFeedbackDb, colorTrimDb, colorLoad, colorAlign };
    if (!save) j.colorBypass = colorBypass;
    return j;
  }

  merge(o) {
    if (!o || typeof o !== 'object') return;
    for (const [k, v] of Object.entries(o)) {
      switch (k) {
        case 'upsampler': this.upsampler = v === 'pcm' || v === 'dsd' ? v : 'off'; break;
        case 'target': this.target = ['2x', 'max', 'fixed'].includes(v) ? v : 'native'; break;
        case 'fixedRate': this.fixedRate = Number.isFinite(+v) && v !== null ? int(v, null) : null; break;
        case 'taps': this.taps = clamp(int(v, 2048), 16, 1 << 20); break;
        case 'phase': this.phase = ['linear', 'minimum', 'custom'].includes(v) ? v : 'intermediate'; break;
        case 'phaseCustom': this.phaseCustom = clamp(Number.isFinite(+v) ? +v : 0.5, 0, 1); break;
        case 'apodize': this.apodize = !!v; break;
        case 'doubleDouble': this.doubleDouble = !!v; break;
        case 'order': this.order = +v === 5 ? 5 : 8; break;
        case 'osr': this.osr = [64, 128, 256, 512, 1024].includes(+v) ? +v : 128; break;
        case 'dsdPcmLowpass': this.dsdPcmLowpass = [30, 50].includes(+v) ? +v : 24; break;
        case 'dsdPcmLimiter': this.dsdPcmLimiter = !!v; break;
        case 'threads': this.threads = clamp(int(v, 0), 0, 256); break;
        case 'cacheLimitMb': this.cacheLimitMb = [500, 1024, 2048].includes(+v) ? +v : 1024; break;
        case 'colorEnabled': this.colorEnabled = v === true; break;
        case 'colorPreset': this.colorPreset = COLOR_PRESETS.includes(v) ? v : 'set-300b'; break;
        // the core's own limit applies when the settings are built (colorDriveMax)
        case 'colorDrive': this.colorDrive = typeof v === 'number' && Number.isFinite(v) ? clamp(v, 0, 2) : 1.0; break;
        case 'colorFeedbackDb': this.colorFeedbackDb = [0, 6, 12, 20, 30].includes(v) ? v : 6; break;
        case 'colorTrimDb': this.colorTrimDb = typeof v === 'number' && Number.isFinite(v) ? Math.round(clamp(v, -6, 6) * 10) / 10 : 0; break;
        case 'colorBypass': this.colorBypass = v === true; break;
        case 'colorLoad': this.colorLoad = v === 'resistor-8' ? v : 'bookshelf-vented-smooth'; break;
        case 'colorAlign': this.colorAlign = ['loudness', 'near-peak', 'off'].includes(v) ? v : 'conservative'; break;
      }
    }
  }
}

const COLOR_PRESETS = ['set-300b'];   // v1 (the others come in later versions)

// Tube Color's controls from the core (daphne-helper color_info, doc/N28); null until the extension has asked
let colorInfo = null;
const setColorInfo = i => { colorInfo = i && typeof i === 'object' ? i : null; };
const getColorInfo = () => colorInfo;
/** The strength slider's limit (1.0 = 100 %), the core's own constant. */
const colorDriveMax = () => (colorInfo && Number.isFinite(colorInfo.drive_max) ? colorInfo.drive_max : 4.0);

/**
 * Tube Color (doc/N28): the panel's choices. The bypass is drive 0 (the stage idles in place, bit-identical, no
 * rebuild); the slider's own value stays in the options. Until M3 every preset plays at its reference operating point:
 * standard bias, 8 ohm tap. The trim 0 is "auto" (the trim is on top of the automatic makeup).
 */
function colorOf(o) {
  return {
    enabled: o.colorEnabled, model: 'fixed-point', preset: o.colorPreset,
    drive: o.colorBypass ? 0 : clamp(o.colorDrive, 0, colorDriveMax()),
    feedback_db: o.colorFeedbackDb, bias: 'standard', load_ohms: 8, load: o.colorLoad, align: o.colorAlign,
    makeup_gain: Math.abs(o.colorTrimDb) < 0.05 ? 'auto' : o.colorTrimDb,
  };
}

const exclusive = s => s.outputMode === 'coreaudio-exclusive';

/** The upsampler in effect: shared mode never upsamples (the system mixer converts everything to its rate). */
const upsamplerOf = (s, o) => (exclusive(s) ? o.upsampler : 'off');

/** output.rate: the target of the PCM upsampler; the source rate otherwise. */
function rateOf(s, o) {
  if (upsamplerOf(s, o) !== 'pcm') return 'native';
  switch (o.target) {
    case '2x': return '2x';   // twice each track's own rate (gapless across sample rates)
    case 'max': return 'auto';
    case 'fixed': return String(o.fixedRate || s.fixedRate || 192000);
    default: return 'native';
  }
}

/**
 * ReplayGain for Daphne's gain stage: MIKU's own rule (the tag's gain plus the preamp, raised only as far as the
 * peak allows). DSD sent directly has no gain stage.
 */
function replayGainDb(host, s, t) {
  if (!t || (host.isDsd && host.isDsd(t))) return 0;
  const db = host.replayGainDb ? host.replayGainDb(t) : null;
  return Number.isFinite(db) ? db : 0;
}

/** MIKU's shelf Q → the RBJ shelf slope S that Daphne's LS / HS take (1/Q² = (A + 1/A)(1/S − 1) + 2). */
function shelfSlope(q, gainDb) {
  const a = Math.pow(10, gainDb / 40), k = a + 1 / a;
  const s = 1 / ((1 / (q * q) - 2) / k + 1);
  return Number.isFinite(s) && s > 0 ? Math.min(s, 1) : 1;
}

const bandType = t => ({ LSC: 'LS', HSC: 'HS', LP: 'LP', LPQ: 'LP', HP: 'HP', HPQ: 'HP' })[String(t || 'PK').toUpperCase()] || 'PK';

/** MIKU's volume mode on the Mac: there is no DAC hardware volume, "hardware" plays as digital (as MIKU's own core). */
const volumeModeOf = s => (s.volumeMode === 'fixed' ? 'fixed' : 'digital');

function build(host, s, o, t) {
  const j = {};
  const dsd = exclusive(s) && (s.dsdMode === 'dop' || s.dop === true) ? 'dop' : 'pcm';   // the Mac has no native DSD
  j.output = {
    api: exclusive(s) ? 'coreaudio_exclusive' : 'coreaudio_shared',
    device: s.deviceId || '',
    rate: rateOf(s, o),
    dsd,
    dsd_pcm_lowpass: o.dsdPcmLowpass,
    dsd_pcm_limiter: o.dsdPcmLimiter,
    max_dsd: [64, 128, 256, 512].includes(s.maxDsd) ? s.maxDsd : 0,   // MIKU's DAC 最高 DSD
    buffer_ms: clamp(int(s.bufferMs, 100), 40, 200),                  // MIKU's 緩衝大小 (the HAL's IO buffer)
  };
  j.core = upsamplerOf(s, o) === 'dsd' ? 'dsd' : 'pcm';
  j.filter = {
    taps: o.taps, phase: o.phase, phase_custom: o.phaseCustom,
    // apodizing belongs to the upsampling filter (the PCM upsampler or the DSD pre-filter; off with the upsampler off)
    apodize: o.apodize && upsamplerOf(s, o) !== 'off',
    double_double: o.doubleDouble,
  };

  // MIKU's DSP: EQ and crossfeed run in Daphne; balance / invert have no stage in the core
  const d = s.dsp || {};
  const dsp = !!d.enabled;
  const bands = [];
  if (dsp && d.eqOn)
    for (const b of d.bands || []) {
      const type = bandType(b.type);
      const q = type === 'LS' || type === 'HS' ? shelfSlope(+b.q || 0.707, +b.gain || 0) : +b.q || 0.707;
      bands.push({ type, f: +b.fc || 1000, q: clamp(q, 0.05, 30), gain_db: +b.gain || 0, on: b.on !== false });
    }
  j.eq = { enabled: dsp && !!d.eqOn && bands.length > 0, bands, preamp_db: dsp && !d.autoPreamp ? Math.min(0, +d.preampDb || 0) : 'auto' };
  // MIKU's crossfeed feed (direct vs crossfed level in dB) → the mix amount k: k / (1 - k) = 10^(-feed / 20)
  const cf = d.crossfeed || {};
  const r = Math.pow(10, -(Number.isFinite(+cf.feed) ? +cf.feed : 4.5) / 20);
  j.crossfeed = { enabled: dsp && !!cf.on, fc: clamp(+cf.fc || 700, 100, 3000), amount: clamp(r / (1 + r), 0, 1) };

  const digital = volumeModeOf(s) === 'digital';
  const db = s.muted ? -120 : clamp(+s.volumeDb || 0, -120, 0);
  j.volume = { mode: digital ? 'digital' : 'fixed', db: digital ? db : 0 };
  j.headroom_db = clamp(replayGainDb(host, s, t), -24, 24);
  j.dsd_modulator = { order: o.order, osr: o.osr };
  j.design = { threads: o.threads, cache_limit_mb: o.cacheLimitMb };
  j.color = colorOf(o);
  return j;
}

module.exports = { DaphneOptions, build, upsamplerOf, replayGainDb, volumeModeOf, exclusive, colorOf, setColorInfo, getColorInfo, colorDriveMax, COLOR_PRESETS };
