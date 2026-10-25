'use strict';
// The signal path for MIKU's card: the summary fields MIKU's page reads (signal.*) and Daphne's own path card
// (signal.custom, drawn by web/daphne.js with the host's shared layout): sections 來源 / DSP / 輸出, each row a one-line
// summary with its details under it. The DSP rows follow the core's processing chain (report "chain"), in its order;
// each stage shows the format it hands on (a "→" pin). The macOS side of src/Daphne.Extension/DaphnePath.cs.
const SM = require('./settings-map');

const khz = r => String(+(r / 1000).toFixed(3));
const mhz = r => String(+(r / 1e6).toFixed(4));
const db = d => (d > 0 ? '+' : '') + d.toFixed(1) + ' dB';
const dsdName = clock => 'DSD' + Math.round(clock / (clock % 44100 === 0 ? 44100 : 48000));
const hz = f => (f >= 1000 ? String(+(f / 1000).toFixed(2)) + ' kHz' : Math.round(f) + ' Hz');
const F64 = '64-bit 浮點';
const pcm = (word, rate) => `${word} / ${khz(rate)} kHz`;
const dsd1 = clock => `1-bit DSD / ${dsdName(clock)}（${mhz(clock)} MHz）`;
const to = format => ({ k: '→', v: format });
const row = (k, v, mod, ...more) => ({ k, v, mod, more: more.filter(x => x) });
const isFloat = type => String(type || '').startsWith('float');
const deviceWord = (type, bits, container) =>
  isFloat(type) ? `${container}-bit 浮點` : bits !== container ? `${bits}-bit 整數（${container}-bit 容器）` : `${bits}-bit 整數`;
const formatText = (type, bits, container) =>
  isFloat(type) ? `${container}-bit 浮點` : bits !== container ? `${bits}-bit（${container}-bit 容器）` : `${bits}-bit`;
const bandName = t => ({ LSC: '低頻架', HSC: '高頻架', LP: '低通', LPQ: '低通', HP: '高通', HPQ: '高通' })[String(t || '').toUpperCase()] || '峰值';

function phaseText(o) {
  switch (o.phase) {
    case 'linear': return '線性相位';
    case 'minimum': return '最小相位';
    case 'custom': return `自訂相位（α ${o.phaseCustom.toFixed(2)}）`;
    default: return '中間相位';
  }
}

const filterLines = (o, apodize) => [
  `濾波器：${o.taps.toLocaleString('en-US')} taps · ${phaseText(o)}`,
  apodize ? '去除錄音振鈴（apodize）：開' : null,
  o.doubleDouble ? '運算精度：double-double（約 106-bit），每個輸出捨入回 64-bit 浮點一次' : '運算精度：64-bit 浮點',
];

/** The DSD file is sent as DSD (DoP), not converted. */
const dsdDirect = r => !!r && r.source && r.source.dsd_rate > 0 && String(r.filter || '').includes('DSD file sent directly');

/** The loaded track's PCM rate (DSD files: after conversion) and the device rate, for the settings page. */
function nowPlaying(r) {
  if (!r || !r.source || !r.device) return null;
  return { source: r.source.rate, dsd: dsdDirect(r) ? r.source.dsd_rate : 0, device: r.device.rate };
}

function volumeRow(s, volumeMode, dsdFile) {
  switch (volumeMode) {
    case 'none': return row('音量', '無（請使用 DAC 旋鈕）', false, dsdFile ? 'DSD 直送不經過數位音量' : null);
    case 'digital': return row('音量', '數位音量 · 0 dB（不處理）', false);
    default: return row('音量', '固定 0 dB', false, '不調整數位增益');
  }
}

/** The card's sections. */
function buildPath(ctx) {
  const { s, o, t, r, chain, isDsd, sentDirect, dop, srcRate, devRate, sysRate, mode, format, volumeMode, rg, notes, host } = ctx;
  const dsdFile = sentDirect, dsdPcm = isDsd && !sentDirect;
  const src = r.source, dev = r.device;
  const pcmRate = src.rate, channels = src.channels;
  const clock = dop ? devRate * 16 : 0;
  const upsampler = SM.upsamplerOf(s, o) !== 'off';
  const lossy = host.isLossy ? host.isLossy(t) : false;
  const sections = [];

  // ── 來源
  const srcText = isDsd ? `${t.codec} • ${dsdName(srcRate)} • ${mhz(srcRate)} MHz` : `${t.codec} • ${t.bits > 0 ? t.bits + '-bit / ' : ''}${khz(srcRate)} kHz`;
  const srcLossless = src.lossless === true, srcBits = Number.isFinite(src.bits) ? src.bits : 0;
  const decode = isDsd
    ? { ...row('解碼', '讀取 1-bit DSD 串流', false), pin: [to(dsd1(srcRate)), { k: '解碼器', mark: true }] }
    : {
      ...row('解碼', lossy ? '有損解碼' : srcLossless && srcBits > 0 ? `${srcBits}-bit 整數轉成浮點` : '浮點樣本', false,
        lossy ? '解碼器輸出浮點樣本，轉成 64-bit 浮點' : srcLossless ? '整數樣本放進 64-bit 浮點的 53-bit 尾數，數值完全不變' : '浮點樣本放進 64-bit 浮點，數值不變'),
      pin: [to(pcm(F64, pcmRate)), { k: '解碼器', mark: true }],
    };
  sections.push({
    title: '來源',
    rows: [row('檔案', srcText, false, channels !== 2 ? `聲道：${channels}` : null, lossy ? '有損壓縮來源：壓縮時已丟棄部分聲音資訊，無法還原' : null), decode],
  });

  // ── DSP (the core's chain, in its order)
  const proc = [];
  let rateNow = pcmRate;
  if (dsdPcm)
    proc.push({
      ...row('DSD 轉 PCM', `${dsdName(srcRate)} → PCM`, true, `低通濾波：${o.dsdPcmLowpass} kHz（濾掉 DSD 的超音波噪聲）`,
        '增益：+6 dB（SACD 0 dB = 0 dBFS，與 PCM 同音量）', o.dsdPcmLimiter ? '限幅器：開（超過滿刻度的峰值壓回，不削波）' : '限幅器：關（超過滿刻度時削波）'),
      pin: [to(pcm(F64, pcmRate))],
    });
  if (chain.includes('volume')) {
    const digital = volumeMode === 'digital';
    const parts = [];
    if (digital) parts.push(s.muted ? '靜音' : '數位音量 ' + db(+s.volumeDb || 0));
    if (Math.abs(rg) > 1e-9) parts.push('ReplayGain ' + db(rg));
    proc.push({
      ...row('音量', parts.length ? parts.join(' · ') : '增益', true, digital ? '數位音量：' + (s.muted ? '靜音' : db(+s.volumeDb || 0)) : null,
        Math.abs(rg) > 1e-9 ? `ReplayGain（${s.replayGain === 'album' ? '依專輯' : '依曲目'}）：${db(rg)}，只在峰值允許的範圍內提高` : null, '調整時平滑過渡'),
      pin: [to(pcm(F64, rateNow))],
    });
  }
  const volPass = !chain.includes('volume') && !dsdFile && volumeMode !== 'none';
  if (volPass) proc.push({ ...volumeRow(s, volumeMode, dsdFile), pin: [to(pcm(F64, rateNow))] });
  const d = s.dsp || {};
  if (chain.includes('crossfeed') && d.crossfeed) {
    const cf = d.crossfeed, feed = Number.isFinite(+cf.feed) ? +cf.feed : 4.5;
    proc.push({
      ...row('Crossfeed', `${hz(+cf.fc || 700)} · ${feed.toFixed(1)} dB`, true, `截止頻率：${hz(+cf.fc || 700)}（低於此頻率的聲音也送到另一耳）`, `交叉量：${feed.toFixed(1)} dB（直接音比交叉音大的幅度）`),
      pin: [to(pcm(F64, rateNow))],
    });
  }
  if (chain.includes('eq')) {
    const on = (d.bands || []).filter(b => b.on !== false);
    const q = b => String(+(+b.q || 0).toFixed(2));
    const lines = ['前級：' + (d.autoPreamp ? '自動' : db(Math.min(0, +d.preampDb || 0)))];
    for (const b of on) {
      const t = String(b.type || 'PK').toUpperCase();
      lines.push(['LP', 'LPQ', 'HP', 'HPQ'].includes(t) ? `${bandName(t)} ${hz(+b.fc)} · Q ${q(b)}` : `${bandName(t)} ${hz(+b.fc)} · ${db(+b.gain || 0)} · Q ${q(b)}`);
    }
    proc.push({ ...row('EQ', `${on.length} 段` + (d.presetName ? ' · ' + d.presetName : ''), true, ...lines), pin: [to(pcm(F64, rateNow))] });
  }
  if (chain.includes('resampler')) {
    rateNow = devRate;
    proc.push({
      ...row('升頻', devRate === pcmRate ? '1 倍 · 只去除錄音振鈴' : `${khz(pcmRate)} → ${khz(devRate)} kHz（×${Math.round(devRate / pcmRate)}）`, true, ...filterLines(o, o.apodize && upsampler)),
      pin: [to(pcm(F64, rateNow))],
    });
  }
  if (chain.includes('modulator pre-filter') || chain.includes('modulator limiter')) {
    const modIn = pcmRate % 44100 === 0 ? 705600 : 768000;
    const lines = [`前級升頻：${khz(pcmRate)} → ${khz(modIn)} kHz`, ...filterLines(o, o.apodize).filter(x => x), '限幅器：峰值保持在調變器的穩定範圍內'];
    if (clock > 0) lines.push(`調變器：${o.order} 階 Δ-Σ → ${dsdName(clock)}（${mhz(clock)} MHz）`);
    proc.push({ ...row('轉成 DSD', `${o.order} 階 Δ-Σ 調變器`, true, ...lines), pin: clock > 0 ? [to(dsd1(clock))] : null });
  }
  if (chain.includes('quantizer')) {
    const word = deviceWord(dev.type, dev.bits, dev.container);
    proc.push({
      ...row('量化', 'TPDF 抖動', true, isFloat(dev.type) ? `量化到 ${dev.bits}-bit 精度（浮點裝置的尾數），加 TPDF 抖動（±1 LSB），不做噪聲整形` : `量化到裝置的 ${dev.bits}-bit，加 TPDF 抖動（±1 LSB），不做噪聲整形`),
      pin: [to(pcm(word, devRate))],
    });
  }
  if (proc.every(x => !x.mod)) {
    const vol = volPass ? (volumeMode === 'digital' ? '數位音量：0 dB，不處理' : '音量：固定 0 dB，不調整數位增益') : null;
    proc.length = 0;
    proc.push({ ...row('DSP', dsdFile ? '無：DSD 原樣送出' : '無：樣本數值不變', false, vol), pin: [to(dsdFile ? dsd1(srcRate) : pcm(F64, pcmRate))] });
  }
  sections.push({ title: 'DSP', mark: true, rows: proc });

  // ── 輸出
  const outRows = [];
  if (!volPass && (volumeMode === 'none' || !chain.includes('volume'))) outRows.push(volumeRow(s, volumeMode, dsdFile));
  const latency = +r.latency_ms || 0, underruns = +r.underruns || 0;
  outRows.push(row('裝置', `${mode} · ${r.device_name}`, false, latency > 0 ? `輸出延遲：約 ${Math.round(latency)} ms` : null, underruns > 0 ? `欠載：${underruns} 次` : null));
  // Core Audio takes 32-bit float: the 24-bit integers convert to it exactly, and in exclusive mode the HAL converts
  // them exactly again to the DAC's integer format (N24)
  const ca = SM.exclusive(s) ? 'Core Audio：24-bit 整數精確轉成 32-bit 浮點送進 HAL，獨佔模式下 HAL 再精確轉成 DAC 的整數格式'
    : 'Core Audio 共享：經過系統混音器';
  if (dop)
    outRows.push(row('格式', `DoP · ${format} / ${khz(devRate)} kHz`, false, `${dsdName(clock)} 包在 PCM 裡送出（DoP 標記），DAC 解開後以 DSD 播放`, ca));
  else {
    const word = deviceWord(dev.type, dev.bits, dev.container);
    const conv = chain.includes('quantizer') ? null : !isFloat(dev.type) ? '64-bit 浮點轉回裝置的整數格式，數值不變' : '64-bit 浮點轉成裝置的浮點格式';
    outRows.push(row('格式', pcm(word, devRate), false, conv, ca));
    // Core Audio resamples what Daphne hands it (shared: the system's rate; exclusive: a DAC without the track's rate)
    if (sysRate > 0) outRows.push(row('系統轉換', `${khz(devRate)} → ${khz(sysRate)} kHz`, true,
      SM.exclusive(s) ? 'Core Audio 的取樣率轉換器：裝置沒有這首歌的取樣率' : '系統混音器重新取樣到裝置目前的取樣率'));
  }
  sections.push({ title: '輸出', rows: outRows });
  return { ext: 'daphne', sections, notes };
}

/** MIKU's signal fields for the loaded track, with Daphne's card in custom. */
function buildSignal(host, s, o, t, r) {
  if (!t || !r || !r.source || !r.device) return null;
  const src = r.source, dev = r.device;
  const filter = String(r.filter || ''), chain = Array.isArray(r.chain) ? r.chain : [];
  const dsdRate = src.dsd_rate || 0;
  const isDsd = dsdRate > 0 || (host.isDsd ? host.isDsd(t) : false);
  const dop = filter.includes('DoP');
  // the core's output rate; Core Audio may convert it to the device's own rate (system_rate) after that
  const devRate = dev.rate, sysRate = +r.system_rate || 0, srcRate = dsdRate > 0 ? dsdRate : src.rate;
  const mode = SM.exclusive(s) ? 'Core Audio 獨佔' : 'Core Audio 共享';
  const sentDirect = isDsd && filter.startsWith('none (DSD file sent directly)');
  const dsp = chain.filter(x => x === 'eq' || x === 'crossfeed').map(x => (x === 'eq' ? 'EQ' : 'Crossfeed'));
  const rg = SM.replayGainDb(host, s, t);
  const format = formatText(dev.type, dev.bits, dev.container);
  const vm = SM.volumeModeOf(s);
  const volumeMode = sentDirect && vm === 'digital' ? 'none' : vm;
  const lossy = host.isLossy ? host.isLossy(t) : false;
  const info = {
    codec: t.codec, sourceRate: srcRate, sourceBits: isDsd ? 1 : t.bits, dsd: isDsd,
    dsdLabel: isDsd && srcRate > 0 ? dsdName(srcRate) : null, lossy, dop, dsdDirect: dop, dsdTransport: dop ? 'Dop' : null,
    resampled: isDsd ? !sentDirect : dop || devRate !== srcRate || sysRate > 0,
    outputRate: sysRate || devRate, outputFormat: format, outputBits: dev.bits, mode, device: r.device_name,
    dspActive: dsp.length > 0, dspSummary: dsp.length ? dsp.join(' · ') : null, replayGainDb: Math.abs(rg) > 1e-9 ? rg : null,
    decoder: 'Daphne', sourceChannels: src.channels, volumeMode,
  };
  const notes = [];
  const d = s.dsp || {};
  if (d.enabled && (Math.abs(+d.balance || 0) > 0.001 || d.invert)) notes.push('Daphne 內核沒有平衡 / 反相，這兩項設定不會作用。');
  if (sysRate > 0) notes.push(`裝置目前是 ${khz(sysRate)} kHz，由 macOS 把 ${khz(devRate)} kHz 轉成裝置的取樣率。`);
  if (sentDirect && volumeMode === 'none') notes.push('DSD 直送時無法使用數位音量，DAC 會以原始音量輸出，請用 DAC 的音量旋鈕調整。');
  const clipped = +r.clipped || 0;
  if (clipped > 0) notes.push(`已有 ${clipped.toLocaleString('en-US')} 個樣本超過滿刻度被削波；請調低數位音量或 EQ 前級。`);
  info.note = notes.length ? notes.join('\n') : null;
  info.custom = buildPath({ s, o, t, r, chain, isDsd, sentDirect, dop, srcRate, devRate, sysRate, mode, format, volumeMode, rg, notes, host });
  info.quality = lossy ? 'low' : !SM.exclusive(s) ? 'high' : dop ? (sentDirect ? 'bitperfect' : 'enhanced') : r.bit_perfect ? 'bitperfect' : 'enhanced';
  return info;
}

module.exports = { buildSignal, nowPlaying, dsdDirect };
