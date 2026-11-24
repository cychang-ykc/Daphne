/* Daphne for MIKU (extension module): Daphne's own settings, the filter design progress and its row in 關於.
   升頻 (upsampler off / PCM 升頻 / 轉成 DSD; PCM target 原取樣率 / 2 倍 / 最高; DSD rate and order), 濾波器
   (apodizing, then phase / length / double-double unless at the source rate). The core only checks ranges; the menus'
   limits are the page's. */
(() => {
  const Daphne = { api: null, state: { options: {} } };
  const o = () => Daphne.state.options || {};
  const set = async v => { Daphne.state = await Daphne.api.rpc('set', v) || Daphne.state; };
  // fields that depend on another: draw the page again
  const rerender = () => { try { Router.render(false, 'none'); } catch (e) { console.error('[daphne]', e); } };

  /* This page in MIKU's interface language (I18N.lang, i18n.js), the way MIKU's own T works: keyed by the Traditional
     Chinese text, a template's ${} values as {0} {1} …. Daphne carries its own words (简体中文, English, 日本語), so a
     new wording here needs no change in MIKU. Text with no entry is shown as written. */
  const WORDS = {
    "線性": ["线性", "Linear", "リニア"],
    "中間": ["中间", "Intermediate", "中間"],
    "最小": ["最小", "Minimum", "ミニマム"],
    "自訂": ["自定义", "Custom", "カスタム"],
    "0（線性）": ["0（线性）", "0 (linear)", "0（リニア）"],
    "1（最小）": ["1（最小）", "1 (minimum)", "1（ミニマム）"],
    "{0} 秒": ["{0} 秒", "{0} s", "{0} 秒"],
    "取樣率不變，只套用濾波器。": ["采样率不变，只应用滤波器。", "Keeps the sample rate; only the filter is applied.", "サンプルレートはそのままで、フィルターだけを適用します。"],
    "曲目取樣率的 2 倍。": ["曲目采样率的 2 倍。", "Twice the track's sample rate.", "曲のサンプルレートの 2 倍です。"],
    "盡量升高，最高到 705.6 / 768 kHz。": ["尽量升高，最高到 705.6 / 768 kHz。", "As high as possible, up to 705.6 / 768 kHz.", "できるだけ高く、最大 705.6 / 768 kHz まで上げます。"],
    "沒有相位偏移，前後都有振鈴，延遲最長。": ["没有相位偏移，前后都有振铃，延迟最长。", "No phase shift; ringing before and after; the longest delay.", "位相のずれがなく、前後にリンギングがあります。遅延は最も長くなります。"],
    "前振鈴很小，延遲較短。（預設）": ["前振铃很小，延迟较短。（默认）", "Very little pre-ringing, a shorter delay. (Default)", "Pre-ringing が小さく、遅延が短めです。（デフォルト）"],
    "沒有前振鈴，延遲最短，高頻有相位偏移。": ["没有前振铃，延迟最短，高频有相位偏移。", "No pre-ringing, the shortest delay; high frequencies are phase-shifted.", "Pre-ringing がなく、遅延が最も短くなります。高域の位相がずれます。"],
    "在線性（0）與最小（1）之間自訂": ["在线性（0）与最小（1）之间自定义", "Anywhere between linear (0) and minimum (1)", "リニア（0）とミニマム（1）の間で設定します"],
    "目前曲目：{0}，不升頻": ["当前曲目：{0}，不升频", "Now playing: {0}, not upsampled", "再生中の曲：{0}、アップサンプリングなし"],
    "維持曲目的取樣率": ["保持曲目的采样率", "Keeps the track's sample rate", "曲のサンプルレートを維持します"],
    "例：44.1 kHz → {0}（{1} 倍）": ["例：44.1 kHz → {0}（{1} 倍）", "e.g. 44.1 kHz → {0} ({1}×)", "例：44.1 kHz → {0}（{1} 倍）"],
    "目前曲目是 DSD，直接送到 DAC，不升頻": ["当前曲目是 DSD，直接送到 DAC，不升频", "Now playing DSD: sent to the DAC as is, not upsampled", "再生中の曲は DSD です。そのまま DAC に送り、アップサンプリングしません"],
    "目前曲目：{0}，不升頻（上限不到 2 倍）": ["当前曲目：{0}，不升频（上限不到 2 倍）", "Now playing: {0}, not upsampled (the limit is under 2×)", "再生中の曲：{0}、アップサンプリングなし（上限が 2 倍未満）"],
    "目前曲目：{0}，DAC 不支援 {1}，不升頻": ["当前曲目：{0}，DAC 不支持 {1}，不升频", "Now playing: {0}; the DAC doesn't support {1}, not upsampled", "再生中の曲：{0}、DAC が {1} に非対応のためアップサンプリングなし"],
    "目前曲目：{0} → {1}（{2} 倍）": ["当前曲目：{0} → {1}（{2} 倍）", "Now playing: {0} → {1} ({2}×)", "再生中の曲：{0} → {1}（{2} 倍）"],
    " · DAC 不支援 {0}，改用 {1}": [" · DAC 不支持 {0}，改用 {1}", " · the DAC doesn't support {0}, using {1}", " · DAC が {0} に非対応のため {1} を使用"],
    "實際長度 {0} 點（{1} × {2} 倍）": ["实际长度 {0} 点（{1} × {2} 倍）", "Actual length {0} taps ({1} × {2})", "実際の長さ {0} タップ（{1} × {2} 倍）"],
    "濾波器暫存": ["滤波器缓存", "Filter cache", "フィルターキャッシュ"],
    "已停用（DAPHNE_FILTER_CACHE=off）": ["已停用（DAPHNE_FILTER_CACHE=off）", "Off (DAPHNE_FILTER_CACHE=off)", "無効（DAPHNE_FILTER_CACHE=off）"],
    "超過時刪除最久沒用的濾波器，使用中的不會刪除。": ["超过时删除最久没用的滤波器，使用中的不会删除。", "Over the limit, the least recently used filters are deleted; filters in use are kept.", "上限を超えると、最も長く使われていないフィルターから削除します。使用中のものは削除しません。"],
    "開啟資料夾": ["打开文件夹", "Open folder", "フォルダーを開く"],
    "刪除暫存的濾波器，使用中的保留。": ["删除缓存的滤波器，使用中的保留。", "Deletes the cached filters; filters in use are kept.", "キャッシュしたフィルターを削除します。使用中のものは残します。"],
    "已清除濾波器暫存": ["已清除滤波器缓存", "Filter cache cleared", "フィルターキャッシュを消去しました"],
    "清除暫存": ["清除缓存", "Clear cache", "キャッシュを消去"],
    "已用 {0} · {1} 個濾波器": ["已用 {0} · {1} 个滤波器", "{0} used · {1} filters", "使用量 {0} · フィルター {1} 個"],
    "上限": ["上限", "Limit", "上限"],
    "自動（核心數 − 2）": ["自动（核心数 − 2）", "Auto (cores − 2)", "自動（コア数 − 2）"],
    "設計用執行緒": ["设计用线程", "Design threads", "設計用スレッド"],
    "設計濾波器時用的 CPU 執行緒。設計時聲音斷續的話請調低。": ["设计滤波器时使用的 CPU 线程。设计时声音断续的话请调低。", "CPU threads used to design filters. Lower it if the sound stutters during a design.", "フィルターの設計に使う CPU スレッド数です。設計中に音が途切れる場合は下げてください。"],
    "升頻": ["升频", "Upsampling", "アップサンプリング"],
    "播放 YouTube Music 時不升頻。": ["播放 YouTube Music 时不升频。", "No upsampling while YouTube Music plays.", "YouTube Music の再生中はアップサンプリングしません。"],
    "關閉": ["关闭", "Off", "オフ"],
    "PCM 升頻": ["PCM 升频", "PCM upsampling", "PCM アップサンプリング"],
    "轉成 DSD": ["转成 DSD", "Convert to DSD", "DSD に変換"],
    "目標取樣率": ["目标采样率", "Target sample rate", "目標サンプルレート"],
    "原取樣率": ["原采样率", "Original", "元のレート"],
    "2 倍": ["2 倍", "2×", "2 倍"],
    "最高": ["最高", "Highest", "最高"],
    "DSD 倍率": ["DSD 倍率", "DSD rate", "DSD レート"],
    "最高到上方的「DAC 最高 DSD 取樣倍率」（DSD{0}）。DAC 不支援時自動降低。": ["最高到上方的「DAC 最高 DSD 采样倍率」（DSD{0}）。DAC 不支持时自动降低。", "Up to \"DAC maximum DSD rate\" above (DSD{0}). Lowered automatically if the DAC doesn't support it.", "上の「DAC の最大 DSD レート」（DSD{0}）までです。DAC が対応していない場合は自動的に下げます。"],
    "請選 DAC 實際支援的倍率，或在上方設定「DAC 最高 DSD 取樣倍率」。DAC 不支援時自動降低。": ["请选择 DAC 实际支持的倍率，或在上方设置「DAC 最高 DSD 采样倍率」。DAC 不支持时自动降低。", "Choose a rate your DAC really supports, or set \"DAC maximum DSD rate\" above. Lowered automatically if the DAC doesn't support it.", "DAC が実際に対応するレートを選ぶか、上の「DAC の最大 DSD レート」を設定してください。DAC が対応していない場合は自動的に下げます。"],
    "階數": ["阶数", "Order", "次数"],
    "超音波雜訊較低": ["超声波噪声较低", "Less ultrasonic noise", "超音波ノイズが少ない"],
    "可聽頻段的雜訊最低（預設）": ["可听频段的噪声最低（默认）", "Least noise in the audible band (default)", "可聴帯域のノイズが最も少ない（デフォルト）"],
    "5 階": ["5 阶", "5th order", "5 次"],
    "8 階": ["8 阶", "8th order", "8 次"],
    "濾波器": ["滤波器", "Filter", "フィルター"],
    "播放 YouTube Music 時不使用。": ["播放 YouTube Music 时不使用。", "Not used while YouTube Music plays.", "YouTube Music の再生中は使用しません。"],
    "目前不經過濾波器。": ["当前不经过滤波器。", "The signal doesn't go through the filter now.", "現在、信号はフィルターを通りません。"],
    "去除錄音振鈴": ["去除录音振铃", "Apodizing", "Apodizing"],
    "去除錄音時留下的高頻振鈴（Apodizing）。20 kHz 以上會被濾掉。": ["去除录音时留下的高频振铃（Apodizing）。20 kHz 以上会被滤掉。", "Removes the high-frequency ringing left by the recording. Above 20 kHz is filtered out.", "録音時に残った高域のリンギングを取り除きます。20 kHz 以上はカットされます。"],
    "相位": ["相位", "Phase", "位相"],
    "自訂相位": ["自定义相位", "Custom phase", "カスタム位相"],
    "0 = 線性、0.5 = 中間、1 = 最小": ["0 = 线性、0.5 = 中间、1 = 最小", "0 = linear, 0.5 = intermediate, 1 = minimum", "0 = リニア、0.5 = 中間、1 = ミニマム"],
    "{0} taps · 延遲 {1}": ["{0} taps · 延迟 {1}", "{0} taps · delay {1}", "{0} taps · 遅延 {1}"],
    "長度": ["长度", "Length", "長さ"],
    "越長，高頻越完整，延遲也越長。": ["越长，高频越完整，延迟也越长。", "Longer keeps more of the highs, with a longer delay.", "長いほど高域が忠実になり、遅延も長くなります。"],
    "106bit 高精度運算": ["106bit 高精度运算", "106-bit high precision", "106bit 高精度演算"],
    "CPU 用量約 1.2 ~ 2 倍。": ["CPU 用量约 1.2 ~ 2 倍。", "Uses about 1.2 ~ 2× the CPU.", "CPU 使用量が約 1.2 ~ 2 倍になります。"],
    "DSD 轉 PCM 低通": ["DSD 转 PCM 低通", "DSD to PCM low-pass", "DSD → PCM ローパス"],
    "濾掉 DSD 的超音波雜訊。頻率越高，保留的高頻越多，雜訊也越多。": ["滤掉 DSD 的超声波噪声。频率越高，保留的高频越多，噪声也越多。", "Filters out DSD's ultrasonic noise. A higher frequency keeps more of the highs, and more noise.", "DSD の超音波ノイズを取り除きます。周波数が高いほど高域が多く残り、ノイズも多くなります。"],
    "24 kHz（預設）": ["24 kHz（默认）", "24 kHz (default)", "24 kHz（デフォルト）"],
    "DSD 轉 PCM 限幅器": ["DSD 转 PCM 限幅器", "DSD to PCM limiter", "DSD → PCM リミッター"],
    "轉換時音量提高 6 dB，和 PCM 一樣大聲。開啟後，過大的峰值會被壓回，不會破音。": ["转换时音量提高 6 dB，和 PCM 一样大声。开启后，过大的峰值会被压回，不会破音。", "Conversion raises the level by 6 dB, as loud as PCM. When on, peaks that are too high are brought back down instead of clipping.", "変換時に音量を 6 dB 上げ、PCM と同じ大きさにします。オンにすると、大きすぎるピークを抑え、音割れを防ぎます。"],
    "正在設計濾波器": ["正在设计滤波器", "Designing the filter", "フィルターを設計中"],
    "播放不會中斷，完成後自動切換。設計好的會存起來，下次不用再等。": ["播放不会中断，完成后自动切换。设计好的会保存，下次不用再等。", "Playback continues and switches over when it's done. The filter is saved, so next time there's no wait.", "再生は中断されず、完了すると自動的に切り替わります。設計したフィルターは保存されるので、次回は待ちません。"],
    "完成": ["完成", "Done", "完了"],
    "播放核心": ["播放核心", "Playback core", "再生コア"],
    "版本不明": ["版本不明", "Unknown version", "バージョン不明"],
  };
  const LANG = ['zh-Hans', 'en', 'ja'].indexOf(typeof I18N !== 'undefined' ? I18N.lang : '');
  const T = (s, ...v) => {
    if (Array.isArray(s) && s.raw) {
      let key = s[0];
      for (let i = 1; i < s.length; i++) key += '{' + (i - 1) + '}' + s[i];
      const w = LANG >= 0 && WORDS[key] ? WORDS[key][LANG] : key;
      return w.replace(/\{(\d+)\}/g, (m, i) => i < v.length ? String(v[i]) : m);
    }
    return LANG >= 0 && WORDS[s] ? WORDS[s][LANG] : s;
  };

  /** Menu entries for a value list, with the current value added when it is not one of them. */
  const withCurrent = (list, cur, label = String) => (list.includes(cur) ? list : [...list, cur].sort((x, y) => x - y)).map(v => [v, label(v)]);
  // the filter's taps, 1024 .. 2^17 (512 taps with apodizing stop only about 114 dB down at the Nyquist rate, 1024 about 210 dB)
  const TAPS = Array.from({ length: 8 }, (_, i) => 1024 << i);
  const PHASES = [['linear', T('線性')], ['intermediate', T('中間')], ['minimum', T('最小')], ['custom', T('自訂')]];
  const ALPHAS = Array.from({ length: 21 }, (_, i) => i * 5 / 100);
  const alphaLabel = a => a === 0 ? T('0（線性）') : a === 1 ? T('1（最小）') : a.toFixed(2);
  const msText = ms => ms >= 1000 ? T`${(ms / 1000).toFixed(2)} 秒` : (ms >= 10 ? Math.round(ms) : ms.toFixed(1)) + ' ms';

  /** Draws the page again when a setting of MIKU's own (read by `key`) changes while the page is open. */
  function watch(host, key) {
    const anchor = h('span', { style: { display: 'none' } });   // gone with the page: stop watching
    host.append(anchor);
    const seen = key();
    const timer = setInterval(() => {
      if (!document.contains(anchor)) { clearInterval(timer); return; }
      if (key() !== seen) { clearInterval(timer); rerender(); }
    }, 400);
  }

  /** MIKU's segmented buttons (views.js seg) where available, else a menu. */
  const choice = (ui, opts, value, onchange) => typeof seg === 'function' ? seg(opts, value, onchange) : ui.select(opts, value, onchange);

  const TARGET_TEXT = {
    native: T('取樣率不變，只套用濾波器。'),
    '2x': T('曲目取樣率的 2 倍。'),
    max: T('盡量升高，最高到 705.6 / 768 kHz。'),
  };
  const PHASE_TEXT = {
    linear: T('沒有相位偏移，前後都有振鈴，延遲最長。'),
    intermediate: T('前振鈴很小，延遲較短。（預設）'),
    minimum: T('沒有前振鈴，延遲最短，高頻有相位偏移。'),
    custom: T('在線性（0）與最小（1）之間自訂'),
  };
  // filter delay (its peak) in ms for a 44.1 kHz track, measured from the filter designs: up = upsampling (any ratio),
  // x1 = apodizing at the source rate; n / a = apodizing off / on; alpha 0, .25, .5, .75, 1
  const DELAY = {"up":{"n":{"512":[5.8,5.19,4.88,4.21,0.29],"1024":[11.6,10.38,9.82,8.45,0.29],"2048":[23.21,20.76,19.64,16.85,0.3],"4096":[46.43,41.56,39.04,33.61,0.31],"8192":[92.87,83.47,78.23,67.32,0.32],"16384":[185.75,167.43,155.99,135.44,0.32],"32768":[371.51,336.0,311.68,272.86,0.32],"65536":[743.03,673.22,625.04,544.33,0.33],"131072":[1486.07,1299.05,1248.71,1090.57,0.33]},"a":{"512":[5.8,4.64,4.44,4.09,0.4],"1024":[11.6,6.43,6.16,5.68,0.4],"2048":[23.21,8.95,8.54,7.86,0.41],"4096":[46.43,12.43,11.85,10.88,0.41],"8192":[92.87,17.24,16.44,14.99,0.41],"16384":[185.75,24.04,22.82,20.79,0.41],"32768":[371.51,33.32,31.64,28.72,0.41],"65536":[743.03,46.32,43.8,39.73,0.41],"131072":[1486.07,64.37,60.79,54.87,0.41]}},"x1":{"a":{"512":[5.78,4.08,5.42,5.78,0.73],"1024":[11.59,6.53,6.6,6.37,0.73],"2048":[23.2,9.14,9.0,8.78,0.75],"4096":[46.42,12.45,12.4,11.56,0.75],"8192":[92.86,17.51,16.85,15.35,0.75],"16384":[185.74,24.15,22.79,20.93,0.75],"32768":[371.5,33.63,31.52,28.87,0.75],"65536":[743.02,46.55,43.63,39.75,0.75],"131072":[1486.05,64.81,60.18,54.56,0.75]}}};
  /** Delay in ms of the filter (between the measured alphas: linear interpolation), for a track at `src` Hz. */
  function delayMs(taps, alpha, apod, x1, src) {
    const row = ((x1 ? DELAY.x1 : DELAY.up)[apod ? 'a' : 'n'] || {})[taps];
    if (!row) return null;
    const p = Math.min(1, Math.max(0, alpha)) * 4, i = Math.min(3, Math.floor(p));
    return (row[i] + (row[i + 1] - row[i]) * (p - i)) * 44100 / src;
  }
  const khz = f => (f / 1000).toFixed(f % 1000 === 0 ? 0 : 1) + ' kHz';
  const fixedOf = op => op.fixedRate || App.settings.fixedRate || 384000;
  /** The rate the core asks for (settings.cpp pcm_output_rate): the largest power-of-two ratio <= 32 within the target. */
  function pcmRate(src, target, fixedRate) {
    if (target === '2x') return src * 2;
    const cap = target === 'max' ? (src % 44100 === 0 ? 705600 : 768000) : target === 'fixed' ? fixedRate : src;
    if (!cap || 2 * src > cap) return src;
    let k = 1;
    while (k < 32 && k * 2 * src <= cap) k *= 2;
    return src * k;
  }
  /** The 升頻 group's result line: the loaded track (Daphne.state.now), or a 44.1 kHz example. */
  function resultText(target, fixedRate) {
    const now = Daphne.state.now;
    if (target === 'native') return now ? T`目前曲目：${khz(now.source)}，不升頻` : T('維持曲目的取樣率');
    if (!now) {
      const want = pcmRate(44100, target, fixedRate);
      return T`例：44.1 kHz → ${khz(want)}（${want / 44100} 倍）`;
    }
    const src = now.source, got = now.device, want = pcmRate(src, target, fixedRate);
    if (now.dsd > 0 && got % src !== 0) return T('目前曲目是 DSD，直接送到 DAC，不升頻');
    if (want === src) return T`目前曲目：${khz(src)}，不升頻（上限不到 2 倍）`;
    if (got === src) return T`目前曲目：${khz(src)}，DAC 不支援 ${khz(want)}，不升頻`;
    let t = T`目前曲目：${khz(src)} → ${khz(got)}（${got / src} 倍）`;
    if (got < want) t += T` · DAC 不支援 ${khz(want)}，改用 ${khz(got)}`;
    return t;
  }

  /** The filter length line under 長度: taps x the upsampling ratio (the loaded track's, or a 44.1 kHz example). */
  function lengthText(taps, target, fixedRate, dsd = false) {
    const now = Daphne.state.now;
    const real = !dsd && now && now.dsd === 0 && now.source > 0 && now.device % now.source === 0;
    const src = real ? now.source : 44100;
    const out = real ? now.device : pcmRate(44100, target, fixedRate);
    const ratio = out / src;
    const n = v => v.toLocaleString('en-US');
    return T`實際長度 ${n(taps * ratio)} 點（${n(taps)} × ${ratio} 倍）`;
  }

  /** A group of fields in Daphne's own card, titled unless `title` is null (升頻: its one field says it). */
  function group(host, ui, title, note) {
    const box = h('div', { class: 'dp-group' }, title ? h('div', { class: 'dp-group-h' }, h('b', null, title), note ? h('small', null, note) : null) : null);
    host.append(box);
    return {
      box,
      field: (t, s, c) => box.append(ui.field(t, s, c)),
      add: el => box.append(el),
    };
  }

  /** The filter cache (the last row of 濾波器): its size, the limit, open the folder, clear it. */
  const cacheField = (field, ui) => {
    const c = Daphne.state.cache;
    if (!c) return;
    if (!c.dir) { field(T('濾波器暫存'), T('已停用（DAPHNE_FILTER_CACHE=off）'), h('span')); return; }
    const size = b => b >= 1 << 30 ? (b / (1 << 30)).toFixed(2) + ' GB' : b >= 1 << 20 ? (b / (1 << 20)).toFixed(1) + ' MB' : Math.round(b / 1024) + ' KB';
    const limit = [500, 1024, 2048].includes(o().cacheLimitMb) ? o().cacheLimitMb : 1024;
    const limitTip = T('超過時刪除最久沒用的濾波器，使用中的不會刪除。');
    const limitSel = ui.select([[500, '500 MB'], [1024, '1 GB'], [2048, '2 GB']], limit, v => set({ cacheLimitMb: +v }).then(rerender));
    limitSel.title = limitTip;
    const open = h('button', { class: 'dp-btn', title: c.dir, onclick: () => Daphne.api.rpc('openCache') }, T('開啟資料夾'));
    const clear = h('button', { class: 'dp-btn', title: T('刪除暫存的濾波器，使用中的保留。'), onclick: async () => {
      clear.disabled = true;
      Daphne.state = await Daphne.api.rpc('clearCache') || Daphne.state;
      toast(T('已清除濾波器暫存'));
      rerender();
    } }, T('清除暫存'));
    // the size follows new filters (a design finishing, the limit pruning) while the page is open
    const text = k => T`已用 ${size((k && k.bytes) || 0)} · ${(k && k.files) || 0} 個濾波器`;
    const status = h('span', null, text(c));
    field(T('濾波器暫存'), status,
      h('div', { class: 'dp-cache-ctl' }, h('span', { class: 'dp-limit-t', title: limitTip }, T('上限')), limitSel, open, clear));
    const update = () => {
      if (!document.contains(status)) { clearInterval(timer); if (Daphne.cacheChanged === update) Daphne.cacheChanged = null; return; }
      Daphne.api.rpc('state').then(st => { if (st) { Daphne.state = st; status.textContent = text(st.cache); } }).catch(() => {});
    };
    const timer = setInterval(update, 2000);
    Daphne.cacheChanged = update;   // a design finishing writes the new filter into the cache: at once (Design.show)
  };

  const threadsField = (op, field, ui) => {
    const threads = [[0, T('自動（核心數 − 2）')], [1, '1'], [2, '2'], [4, '4'], [8, '8'], [16, '16']];
    field(T('設計用執行緒'), T('設計濾波器時用的 CPU 執行緒。設計時聲音斷續的話請調低。'), ui.select(threads, op.threads || 0, v => set({ threads: +v })));
  };

  /** 升頻 (the upsampler and its target, or the DSD rate and order) → 濾波器 (PCM 升頻 and 轉成 DSD; apodizing first). */
  function coreSettings(host, ui) {
    const op = o();
    // MIKU's 緩衝大小 is the WASAPI device buffer (output.buffer_ms); the core picks its rate in its own 升頻 group: no
    // 取樣率 card
    if (ui.hide) ui.hide('rate');
    watch(host, () => [App.settings.fixedRate, App.settings.maxDsd, App.settings.outputMode].join('|'));
    if (App.settings.outputMode === 'shared') {
      // WASAPI shared: Windows' mixer converts to the system format, so Daphne neither upsamples nor converts to DSD
      return;
    }
    const cur = ['pcm', 'dsd'].includes(op.upsampler) ? op.upsampler : 'off';
    // 上限 (fixed) is no longer offered: a setting still on it becomes 最高, the closest, so the page and playback agree
    if (op.target === 'fixed') { op.target = 'max'; set({ target: 'max' }).catch(() => {}); }
    const target = ['native', '2x', 'max'].includes(op.target) ? op.target : 'native';
    const fixedRate = fixedOf(op);
    let lenLine = null;   // the filter group's length line, refreshed with the 升頻 result line

    // YouTube Music plays through Daphne without upsampling (SettingsMap.Upsampler): the group is shown greyed out, and
    // the page is drawn again when the track changes between it and a file
    const live = !!Daphne.state.live;
    const g = group(host, ui, null);
    if (live) g.box.classList.add('dp-off');
    const liveTimer = setInterval(() => {
      if (!document.contains(g.box)) { clearInterval(liveTimer); return; }
      Daphne.api.rpc('state').then(s => {
        if (!s) return;
        Daphne.state = s;
        if (!!s.live !== live) { clearInterval(liveTimer); rerender(); }
      }).catch(() => {});
    }, 2000);
    // no group title and no description: the field is 升頻 itself (YouTube Music: why it is greyed out)
    g.field(T('升頻'), live ? T('播放 YouTube Music 時不升頻。') : null,
      choice(ui, [['off', T('關閉')], ['pcm', T('PCM 升頻')], ['dsd', T('轉成 DSD')]], cur, v => set({ upsampler: v }).then(rerender)));
    if (cur === 'pcm') {
      g.field(T('目標取樣率'), TARGET_TEXT[target],
        choice(ui, [['native', T('原取樣率')], ['2x', T('2 倍')], ['max', T('最高')]], target, v => set({ target: v }).then(rerender)));
      const line = h('div', { class: 'dp-result num' }, resultText(target, fixedRate));
      g.add(line);
      // the loaded track can change while the page is open
      const refresh = () => Daphne.api.rpc('state').then(s => {
        if (s) Daphne.state = s;
        line.textContent = resultText(target, fixedRate);
        if (lenLine) lenLine.textContent = lengthText(op.taps || 2048, target, fixedRate);
      }).catch(() => {});
      refresh();
      const timer = setInterval(() => { if (!document.contains(line)) clearInterval(timer); else refresh(); }, 3000);
    }
    if (cur === 'dsd') {
      // MIKU's DAC 最高 DSD (DSD section) caps the rate: only the multiples up to it are offered
      const lim = [64, 128, 256, 512].includes(App.settings.maxDsd) ? App.settings.maxDsd : 0;
      const osr = Math.min(op.osr || 128, lim || 1024), order = op.order === 5 ? 5 : 8;
      g.field(T('DSD 倍率'), lim ? T`最高到上方的「DAC 最高 DSD 取樣倍率」（DSD${lim}）。DAC 不支援時自動降低。`
        : T('請選 DAC 實際支援的倍率，或在上方設定「DAC 最高 DSD 取樣倍率」。DAC 不支援時自動降低。'),
        ui.select([64, 128, 256, 512, 1024].filter(v => !lim || v <= lim).map(v => [v, 'DSD' + v]), osr, v => set({ osr: +v }).then(rerender)));
      g.field(T('階數'), order === 5 ? T('超音波雜訊較低') : T('可聽頻段的雜訊最低（預設）'),
        choice(ui, [[5, T('5 階')], [8, T('8 階')]], order, v => set({ order: +v }).then(rerender)));
    }

    if (cur === 'pcm' || cur === 'dsd') {
      // the 濾波器 group: the PCM upsampler's filter, or the DSD pre-filter's (the same settings)
      const native = cur === 'pcm' && target === 'native';
      const lenTarget = cur === 'dsd' ? 'max' : target;   // the pre-filter goes to 705.6 / 768 kHz
      const apod = !!op.apodize;
      const f = group(host, ui, T('濾波器'), live ? T('播放 YouTube Music 時不使用。')
                                         : native && !apod ? T('目前不經過濾波器。')
                                         : null);
      if (live) f.box.classList.add('dp-off');   // greyed out like 升頻; the filter cache row stays usable (dp-keep)
      f.field(T('去除錄音振鈴'), T('去除錄音時留下的高頻振鈴（Apodizing）。20 kHz 以上會被濾掉。'),
        ui.sw(apod, v => set({ apodize: v }).then(rerender)));
      if (!native) {
        const src = (Daphne.state.now && Daphne.state.now.source) || 44100;
        const phase = PHASES.some(p => p[0] === op.phase) ? op.phase : 'intermediate';
        const alpha = phase === 'linear' ? 0 : phase === 'minimum' ? 1 : phase === 'custom' ? (op.phaseCustom == null ? 0.5 : op.phaseCustom) : 0.5;
        f.field(T('相位'), PHASE_TEXT[phase], choice(ui, PHASES, phase, v => set({ phase: v }).then(rerender)));
        if (phase === 'custom')
          f.field(T('自訂相位'), T('0 = 線性、0.5 = 中間、1 = 最小'), ui.select(withCurrent(ALPHAS, alpha, alphaLabel), alpha, v => set({ phaseCustom: +v }).then(rerender)));
        const taps = op.taps || 2048;
        const label = t => { const d = delayMs(t, alpha, apod, false, src); const n = t.toLocaleString('en-US'); return d == null ? n + ' taps' : T`${n} taps · 延遲 ${msText(d)}`; };
        f.field(T('長度'), T('越長，高頻越完整，延遲也越長。'),
          ui.select(withCurrent(TAPS, taps, label), taps, v => set({ taps: +v }).then(rerender)));
        lenLine = h('div', { class: 'dp-result num' }, lengthText(taps, lenTarget, fixedRate, cur === 'dsd'));
        f.add(lenLine);
        f.field(T('106bit 高精度運算'), T('CPU 用量約 1.2 ~ 2 倍。'),
          ui.sw(!!op.doubleDouble, v => set({ doubleDouble: v })));
        threadsField(op, f.field, ui);
      }
      const before = f.box.children.length;
      cacheField(f.field, ui);
      for (const el of [...f.box.children].slice(before)) el.classList.add('dp-keep');
    }
  }

  /** DSD section: the DSD → PCM options, shown while DSD files play as PCM (DSD 播放方式 PCM, or WASAPI shared). */
  function dsdSettings(host, ui) {
    const op = o(), st = App.settings;
    if (st.outputMode === 'shared' || st.dsdMode === 'pcm') {
      host.append(ui.field(T('DSD 轉 PCM 低通'), T('濾掉 DSD 的超音波雜訊。頻率越高，保留的高頻越多，雜訊也越多。'),
        ui.select([[24, T('24 kHz（預設）')], [30, '30 kHz'], [50, '50 kHz']], op.dsdPcmLowpass || 24, v => set({ dsdPcmLowpass: +v }))));
      host.append(ui.field(T('DSD 轉 PCM 限幅器'), T('轉換時音量提高 6 dB，和 PCM 一樣大聲。開啟後，過大的峰值會被壓回，不會破音。'),
        ui.sw(op.dsdPcmLimiter !== false, v => set({ dsdPcmLimiter: v }))));
    }
    // DSD 播放方式 / 輸出模式 are on the same page
    watch(host, () => App.settings.dsdMode + '|' + App.settings.outputMode);
  }

  // The tonos: the petal on the icon's α, the one standard for every petal. Traced from the 2048 px icon in GFS Didot
  // font units around the α (glyph origin and baseline at 0, y down); its box is 253.5 -769.0 121.9 225.8 (x y w h).
  const TONOS_D = 'M367.4 -769.0 L368.1 -769.0 L371.1 -758.3 L374.2 -742.2 L375.4 -729.1 L375.3 -708.3 L373.5 -692.8 L369.4 -673.8 L364.6 -658.9 L357.5 -642.2 L348.6 -625.5 L339.5 -611.9 L328.3 -597.6 L316.2 -584.7 L304.9 -574.3 L288.8 -561.7 L273.4 -551.6 L257.9 -543.2 L255.3 -555.3 L253.5 -571.4 L253.5 -590.4 L254.8 -604.1 L257.8 -621.4 L262.5 -639.2 L268.4 -655.9 L275.5 -672.0 L284.6 -688.6 L294.2 -703.5 L303.5 -716.0 L315.5 -729.7 L327.7 -741.6 L340.0 -751.8 L351.9 -760.2Z';
  const tonosSvg = cls => '<svg class="' + cls + '" viewBox="253.5 -769.0 121.9 225.8" aria-hidden="true"><path fill="#E58FB7" d="' + TONOS_D + '"/></svg>';
  const fromHtml = s => { const t = document.createElement('template'); t.innerHTML = s; return t.content.firstChild; };
  /** α with its petal (GFS Didot): the petal is placed on the α in em (daphne.css .dp-tonos). */
  const alpha = () => h('span', { class: 'dp-alpha' }, 'α', fromHtml(tonosSvg('dp-tonos')));
  /** Δάφνη in GFS Didot, its accent the petal. */
  const greek = () => h('span', { class: 'dp-greek', role: 'img', 'aria-label': 'Δάφνη' }, 'Δ', alpha(), 'φνη');
  /** The app icon: the tile and its α with the petal. */
  const icon = () => h('span', { class: 'dp-icon', role: 'img', 'aria-label': 'Daphne' }, h('span', { class: 'dp-icon-a' }, alpha()));
  // the petal before DAPHNE: the same petal, as tall as DAPHNE's capitals (daphne.css .dp-petal-svg)
  const PETAL = tonosSvg('');
  const mark = () => h('span', { class: 'dp-mark' }, h('span', { class: 'dp-petal-svg', html: PETAL }), 'DAPHNE');

  /**
   * Daphne's signal path card (SignalInfo.Custom from DaphneEngine.BuildPath): drawn with the host's shared layout
   * (sections 來源 / DSP / 輸出, one-line stages that fold open), Daphne's mark on 解碼 and after the DSP title. A host
   * without the shared layout gets the stages as plain lines.
   */
  function drawPath(box, sg, ctx) {
    const p = sg.custom || {};
    if (ctx.drawPath) return ctx.drawPath(p, { mark });
    for (const sect of p.sections || []) {
      box.append(h('div', { class: 'sig-sect' }, sect.title));
      for (const r of sect.rows || []) box.append(ctx.stage(r.k, r.v, r.mod));
    }
    for (const n of p.notes || []) box.append(h('div', { class: 'note' }, n));
  }

  /** Progress popup while the core designs a filter (first load of a filter, or a rebuild in the background). */
  const Design = {
    el: null, hideTimer: null,
    show(d) {
      if (!this.el) {
        this.el = h('div', { class: 'dp-design' },
          // the title starts with the wordmark (petal + DAPHNE), as in the signal path card
          h('div', { class: 'dp-design-t' }, h('b', { class: 'dp-design-h' }, mark(), T('正在設計濾波器')), h('span', { class: 'dp-design-pct' }, '')),
          h('div', { class: 'dp-design-f' }, ''),
          h('div', { class: 'dp-design-bar' }, h('i', null)),
          h('small', null, T('播放不會中斷，完成後自動切換。設計好的會存起來，下次不用再等。')));
        document.body.append(this.el);
      }
      clearTimeout(this.hideTimer);
      const pct = Math.max(0, Math.min(100, Math.round((d.progress || 0) * 100)));
      this.el.querySelector('.dp-design-f').textContent = d.text || '';
      this.el.querySelector('.dp-design-pct').textContent = d.done ? T('完成') : pct + '%';
      this.el.querySelector('.dp-design-bar i').style.width = (d.done ? 100 : pct) + '%';
      this.el.classList.add('on');
      if (d.done) {
        this.hideTimer = setTimeout(() => this.el && this.el.classList.remove('on'), 1200);
        if (Daphne.cacheChanged) Daphne.cacheChanged();   // the settings page's 濾波器暫存 row, when it is open
      }
    },
  };

  MikuExt.register({
    id: 'daphne',
    async init(api) {
      Daphne.api = api;
      Daphne.state = await api.rpc('state') || Daphne.state;
      api.on('design', d => Design.show(d || {}));
      // the core stepped down to what the output device takes (DSD -> PCM, DSD128 -> DSD64): once per device and session
      api.on('notice', d => { if (d && d.text) toast(d.text, { ms: 9000 }); });
      api.signalPath(drawPath);
      // the wordmark in place of "Daphne" in MIKU's 播放方案 menu (MIKU builds without coreLabel show the name)
      if (api.coreLabel) api.coreLabel('daphne', mark);
      // Daphne's own card after the output card, titled with the wordmark: groups of the same rank, DSD (MIKU's DSD
      // card moved in) → 升頻 → 濾波器 …
      let dsdGroup = null;
      api.addSettings('core', (host, ui) => {
        dsdGroup = null;
        if (ui.core.id !== 'daphne') return;
        const card = ui.section(null);
        card.classList.add('dp-sect');
        // the title: Δάφνη (GFS Didot, its petal from the icon) and DAPHNE under it as text; the divider and 再生設置 are artwork
        card.append(h('div', { class: 'dp-sect-h' }, h('span', { class: 'dp-title', role: 'img', 'aria-label': 'Daphne 再生設置' },
          greek(), h('span', { class: 'dp-latin' }, h('span', { class: 'dp-petal-svg', html: PETAL }), 'DAPHNE'))));
        // WASAPI shared: nothing of it applies (Windows' mixer converts everything; MIKU hides its DSD card too)
        if (App.settings.outputMode === 'shared') card.style.display = 'none';
        else dsdGroup = group(card, ui, 'DSD');
        coreSettings(card, ui);
      });
      api.addSettings('dsd', (host, ui) => {
        if (ui.core.id !== 'daphne') return;
        dsdSettings(host, ui);
        // MIKU's DSD card (this host): its fields into the DSD group, the empty card hidden
        if (!dsdGroup || !dsdGroup.box.isConnected) return;
        for (const el of [...host.children]) if (!el.matches('h2, .hint')) dsdGroup.add(el);
        host.classList.add('dp-moved');
      });
      api.addSettings('about', (host, ui) => {
        api.rpc('state').then(s => {   // newer numbers for the next time the page is drawn
          if (s) Daphne.state = s;
        });
        // the row in 關於: the brand (icon; Δάφνη, under it petal DAPHNE | 播放核心 [commit]); the filter cache is in the
        // audio settings (濾波器 → 濾波器暫存)
        const brand = h('div', { class: 'dp-brand' },
          icon(),
          h('div', { class: 'lbl' },
            h('b', { class: 'dp-name' }, h('span', { class: 'dp-wordmark' }, greek())),
            h('small', { class: 'dp-meta' },
              h('span', { class: 'dp-latin' }, h('span', { class: 'dp-petal-svg', html: PETAL }), 'DAPHNE'),
              h('span', { class: 'dp-sep' }),
              h('span', null, T('播放核心')),
              Daphne.state.commit ? h('code', { class: 'dp-commit' }, Daphne.state.commit) : h('span', null, T('版本不明')))));
        host.append(h('div', { class: 'field dp-about' }, h('div', { class: 'dp-row' }, brand)));
      });
    },
  });
})();
