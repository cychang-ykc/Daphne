/* Daphne for MIKU (extension module): Daphne's own settings, the filter design progress and its row in 關於.
   升頻 (upsampler off / PCM 升頻 / 轉成 DSD; PCM target 原取樣率 / 2 倍 / 最高 / 固定; DSD rate and order), 濾波器
   (apodizing, then phase / length / double-double unless at the source rate). The core only checks ranges; the menus'
   limits are the page's. */
(() => {
  const Daphne = { api: null, state: { options: {} } };
  const o = () => Daphne.state.options || {};
  const set = async v => { Daphne.state = await Daphne.api.rpc('set', v) || Daphne.state; };
  // fields that depend on another: draw the page again
  const rerender = () => { try { Router.render(false, 'none'); } catch (e) { console.error('[daphne]', e); } };

  /** Menu entries for a value list, with the current value added when it is not one of them. */
  const withCurrent = (list, cur, label = String) => (list.includes(cur) ? list : [...list, cur].sort((x, y) => x - y)).map(v => [v, label(v)]);
  // the filter's taps, 1024 .. 2^17 (512 taps with apodizing stop only about 114 dB down at the Nyquist rate, 1024 about 210 dB)
  const TAPS = Array.from({ length: 8 }, (_, i) => 1024 << i);
  const PHASES = [['linear', '線性'], ['intermediate', '中間'], ['minimum', '最小'], ['custom', '自訂']];
  const ALPHAS = Array.from({ length: 21 }, (_, i) => i * 5 / 100);
  const alphaLabel = a => a === 0 ? '0（線性）' : a === 1 ? '1（最小）' : a.toFixed(2);
  const msText = ms => ms >= 1000 ? (ms / 1000).toFixed(2) + ' 秒' : (ms >= 10 ? Math.round(ms) : ms.toFixed(1)) + ' ms';

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

  const UPSAMPLER_TEXT = {
    off: '維持原始取樣率播放；沒有任何處理時為 Bit-perfect（見訊號路徑）',
    pcm: 'PCM 升頻，經過下方的濾波器',
    dsd: '轉成 DSD 輸出（DoP / ASIO 原生 DSD），DSD 倍率在下方選擇',
  };
  const TARGET_TEXT = {
    native: '維持曲目的取樣率，只經過濾波器（例如去除錄音振鈴）',
    '2x': '升到曲目取樣率的 2 倍',
    max: '升到同一族中最高的取樣率（705.6 / 768 kHz 以下）',
  };
  const FIXED_RATES = [88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000];
  const PHASE_TEXT = {
    linear: '前後振鈴對稱、沒有相位失真；延遲最長',
    intermediate: '前振鈴很小、延遲較短（預設）',
    minimum: '沒有前振鈴、延遲最短，高頻有相位偏移',
    custom: '在線性（0）與最小（1）之間自訂',
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
    if (target === 'native') return now ? `目前曲目：${khz(now.source)}，維持原始取樣率` : '維持曲目的取樣率';
    if (!now) {
      const want = pcmRate(44100, target, fixedRate);
      return `例：44.1 kHz 曲目 → ${khz(want)}（${want / 44100} 倍）· 播放時改為顯示目前曲目`;
    }
    const src = now.source, got = now.device, want = pcmRate(src, target, fixedRate);
    if (now.dsd > 0 && got % src !== 0) return '目前曲目是 DSD，直接送到 DAC，不經過 PCM 升頻';
    if (want === src) return `目前曲目：${khz(src)}，目標不到 2 倍，維持原始取樣率`;
    if (got === src) return `目前曲目：${khz(src)} · DAC 不支援 ${khz(want)}，維持原始取樣率`;
    let t = `目前曲目：${khz(src)} → ${khz(got)}（${got / src} 倍）`;
    if (got < want) t += ` · DAC 不支援 ${khz(want)}，已退到 ${khz(got)}`;
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
    return `實際長度：${n(taps)} taps × ${ratio} 倍升頻 = ${n(taps * ratio)} 點` +
           `（${real ? '目前曲目 ' : '例：'}${khz(src)} → ${khz(out)}）`;
  }

  /** A titled group of fields in Daphne's own card. */
  function group(host, ui, title, note) {
    const box = h('div', { class: 'dp-group' }, h('div', { class: 'dp-group-h' }, h('b', null, title), note ? h('small', null, note) : null));
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
    if (!c.dir) { field('濾波器暫存', '已停用（DAPHNE_FILTER_CACHE=off）', h('span')); return; }
    const size = b => b >= 1 << 30 ? (b / (1 << 30)).toFixed(2) + ' GB' : b >= 1 << 20 ? (b / (1 << 20)).toFixed(1) + ' MB' : Math.round(b / 1024) + ' KB';
    const limit = [500, 1024, 2048].includes(o().cacheLimitMb) ? o().cacheLimitMb : 1024;
    const limitTip = '濾波器暫存上限：超過時從最久沒用到的濾波器開始刪除，目前設定使用中的濾波器一律保留';
    const limitSel = ui.select([[500, '500 MB'], [1024, '1 GB'], [2048, '2 GB']], limit, v => set({ cacheLimitMb: +v }).then(rerender));
    limitSel.title = limitTip;
    const open = h('button', { class: 'dp-btn', title: c.dir, onclick: () => Daphne.api.rpc('openCache') }, '開啟資料夾');
    const clear = h('button', { class: 'dp-btn', title: '清除濾波器暫存：只保留目前設定使用中的濾波器', onclick: async () => {
      clear.disabled = true;
      Daphne.state = await Daphne.api.rpc('clearCache') || Daphne.state;
      toast('已清除濾波器暫存（保留目前設定使用的濾波器）');
      rerender();
    } }, '清除暫存');
    // the size follows new filters (a design finishing, the limit pruning) while the page is open
    const text = k => `目前 ${size((k && k.bytes) || 0)}・${(k && k.files) || 0} 個`;
    const status = h('span', null, text(c));
    field('濾波器暫存', status,
      h('div', { class: 'dp-cache-ctl' }, h('span', { class: 'dp-limit-t', title: limitTip }, '上限'), limitSel, open, clear));
    const update = () => {
      if (!document.contains(status)) { clearInterval(timer); if (Daphne.cacheChanged === update) Daphne.cacheChanged = null; return; }
      Daphne.api.rpc('state').then(st => { if (st) { Daphne.state = st; status.textContent = text(st.cache); } }).catch(() => {});
    };
    const timer = setInterval(update, 2000);
    Daphne.cacheChanged = update;   // a design finishing writes the new filter into the cache: at once (Design.show)
  };

  const threadsField = (op, field, ui) => {
    const threads = [[0, '自動（核心數 − 2）'], [1, '1'], [2, '2'], [4, '4'], [8, '8'], [16, '16']];
    field('濾波器設計執行緒', '設計大型濾波器時佔用的 CPU 執行緒；播放時會影響聲音的話請調低', ui.select(threads, op.threads || 0, v => set({ threads: +v })));
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
    const target = ['native', '2x', 'max', 'fixed'].includes(op.target) ? op.target : 'native';
    const fixedRate = fixedOf(op);
    let lenLine = null;   // the filter group's length line, refreshed with the 升頻 result line

    // YouTube Music plays through Daphne without upsampling (SettingsMap.Upsampler): the group is shown greyed out, and
    // the page is drawn again when the track changes between it and a file
    const live = !!Daphne.state.live;
    const g = group(host, ui, '升頻', live ? '使用 Daphne 播放 YT Music 時禁用升頻' : null);
    if (live) g.box.classList.add('dp-off');
    const liveTimer = setInterval(() => {
      if (!document.contains(g.box)) { clearInterval(liveTimer); return; }
      Daphne.api.rpc('state').then(s => {
        if (!s) return;
        Daphne.state = s;
        if (!!s.live !== live) { clearInterval(liveTimer); rerender(); }
      }).catch(() => {});
    }, 2000);
    g.field('升頻器', UPSAMPLER_TEXT[cur],
      choice(ui, [['off', '關閉'], ['pcm', 'PCM 升頻'], ['dsd', '轉成 DSD']], cur, v => set({ upsampler: v }).then(rerender)));
    if (cur === 'pcm') {
      g.field('目標取樣率', target === 'fixed' ? '升到不超過這個取樣率的最高 2 的冪倍率' : TARGET_TEXT[target],
        choice(ui, [['native', '原取樣率'], ['2x', '2 倍'], ['max', '最高'], ['fixed', '固定']], target, v => set({ target: v }).then(rerender)));
      if (target === 'fixed')
        g.field('固定取樣率', 'DAC 不支援時自動往下退',
          ui.select(withCurrent(FIXED_RATES, fixedRate, khz), fixedRate, v => set({ fixedRate: +v }).then(rerender)));
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
      g.field('DSD 取樣率', lim ? `最高到 DSD 區設定的「DAC 最高 DSD」（DSD${lim}）；DAC 拒絕時自動往下退`
        : '請依 DAC 實際支援的格式選擇（驅動程式宣稱的上限不一定可靠，可在 DSD 區設定「DAC 最高 DSD」）；DAC 拒絕時自動往下退',
        ui.select([64, 128, 256, 512, 1024].filter(v => !lim || v <= lim).map(v => [v, 'DSD' + v]), osr, v => set({ osr: +v }).then(rerender)));
      g.field('階數', order === 5 ? '超音波雜訊最低' : '可聽頻段最安靜（預設）',
        choice(ui, [[5, '5 階'], [8, '8 階']], order, v => set({ order: +v }).then(rerender)));
      g.add(h('div', { class: 'dp-result num' }, `PCM 先以下方的濾波器升到 705.6 / 768 kHz，再轉成 DSD${osr}`));
    }

    if (cur === 'pcm' || cur === 'dsd') {
      // the 濾波器 group: the PCM upsampler's filter, or the DSD pre-filter's (the same settings)
      const native = cur === 'pcm' && target === 'native';
      const lenTarget = cur === 'dsd' ? 'max' : target;   // the pre-filter goes to 705.6 / 768 kHz
      const apod = !!op.apodize;
      const f = group(host, ui, '濾波器', live ? '使用 Daphne 播放 YT Music 時禁用升頻，濾波器不使用'
                                         : native && !apod ? '原取樣率且沒有去除錄音振鈴：訊號不經過濾波器'
                                         : cur === 'dsd' ? '轉成 DSD 前，升頻到 705.6 / 768 kHz 用的濾波器' : null);
      if (live) f.box.classList.add('dp-off');   // greyed out like 升頻; the filter cache row stays usable (dp-keep)
      f.field('去除錄音振鈴（Apodizing）', native
          ? '在原始取樣率套用：20 kHz 開始柔和衰減，去掉錄音時留下的高頻振鈴；20–22 kHz 會被濾掉'
          : '通帶改到 20 kHz、Nyquist 完全衰減，去掉錄音時留下的高頻振鈴；20–22 kHz 會被濾掉',
        ui.sw(apod, v => set({ apodize: v }).then(rerender)));
      if (!native) {
        const src = (Daphne.state.now && Daphne.state.now.source) || 44100;
        const phase = PHASES.some(p => p[0] === op.phase) ? op.phase : 'intermediate';
        const alpha = phase === 'linear' ? 0 : phase === 'minimum' ? 1 : phase === 'custom' ? (op.phaseCustom == null ? 0.5 : op.phaseCustom) : 0.5;
        f.field('相位', PHASE_TEXT[phase], choice(ui, PHASES, phase, v => set({ phase: v }).then(rerender)));
        if (phase === 'custom')
          f.field('α', '0 = 線性、0.5 = 中間、1 = 最小', ui.select(withCurrent(ALPHAS, alpha, alphaLabel), alpha, v => set({ phaseCustom: +v }).then(rerender)));
        const taps = op.taps || 2048;
        const label = t => { const d = delayMs(t, alpha, apod, false, src); return t.toLocaleString('en-US') + ' taps' + (d == null ? '' : ' · 延遲 ' + msText(d)); };
        f.field('長度', `越長頻寬越接近 Nyquist（去除錄音振鈴時為 20 kHz），延遲也越長；延遲以${Daphne.state.now ? '目前曲目' : ' 44.1 kHz 曲目'}計算`,
          ui.select(withCurrent(TAPS, taps, label), taps, v => set({ taps: +v }).then(rerender)));
        lenLine = h('div', { class: 'dp-result num' }, lengthText(taps, lenTarget, fixedRate, cur === 'dsd'));
        f.add(lenLine);
        f.field('雙倍精度摺積', '以 double-double（約 106 bit）計算摺積；量測上沒有可聽差異，CPU 約 1.2–2 倍',
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
      host.append(ui.field('DSD 轉 PCM 低通', '濾掉 DSD 的超音波噪聲；頻率越高保留越多高頻，也留下越多超音波噪聲',
        ui.select([[24, '24 kHz（預設）'], [30, '30 kHz'], [50, '50 kHz']], op.dsdPcmLowpass || 24, v => set({ dsdPcmLowpass: +v }))));
      host.append(ui.field('DSD 轉 PCM 限幅器', 'DSD 轉 PCM 固定 +6 dB（SACD 0 dB = 0 dBFS，與 PCM 同音量）；超過滿刻度的峰值由限幅器壓回，而不是削波',
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
          h('div', { class: 'dp-design-t' }, h('b', { class: 'dp-design-h' }, mark(), '正在設計濾波器'), h('span', { class: 'dp-design-pct' }, '')),
          h('div', { class: 'dp-design-f' }, ''),
          h('div', { class: 'dp-design-bar' }, h('i', null)),
          h('small', null, '播放不中斷，設計完成後會交叉淡入；結果會快取，下次切換就不用再等'));
        document.body.append(this.el);
      }
      clearTimeout(this.hideTimer);
      const pct = Math.max(0, Math.min(100, Math.round((d.progress || 0) * 100)));
      this.el.querySelector('.dp-design-f').textContent = d.text || '';
      this.el.querySelector('.dp-design-pct').textContent = d.done ? '完成' : pct + '%';
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
              h('span', null, '播放核心'),
              Daphne.state.commit ? h('code', { class: 'dp-commit' }, Daphne.state.commit) : h('span', null, '版本不明'))));
        host.append(h('div', { class: 'field dp-about' }, h('div', { class: 'dp-row' }, brand)));
      });
    },
  });
})();
