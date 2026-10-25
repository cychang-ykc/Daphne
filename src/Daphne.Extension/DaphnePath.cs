using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Miku;

namespace DaphneMiku;

/// <summary>
/// Daphne's own signal path card (SignalInfo.Custom, drawn by web/daphne.js with the host's shared layout): sections
/// 來源 / DSP / 輸出, each row a one-line summary with its details under it (folded when it has details or does not fit).
/// The DSP rows follow the core's processing chain (report "chain"), in its order. Each stage that hands samples on
/// shows the format it hands on (a "→" pin: word length and rate), so the card reads as one continuous path from the
/// file's format to the device's.
/// </summary>
public sealed partial class DaphneEngine
{
    /// <summary>
    /// One row (the host's shared signal path layout, SignalPath): label, the one-line summary, whether it changes the
    /// samples (the pink dot), the details (folded), and pin: lines under the summary shown all the time.
    /// </summary>
    sealed record PathRow(string k, string v, bool mod, string[] more, PathPin[] pin = null);
    /// <summary>A pin: label and value, or mark: Daphne's mark in place of the value.</summary>
    sealed record PathPin(string k, string v = null, bool mark = false);

    static PathRow Row(string k, string v, bool mod, params string[] more) =>
        new(k, v, mod, more.Where(x => !string.IsNullOrEmpty(x)).ToArray());

    /// <summary>The "→" pin: the format a stage hands to the next one.</summary>
    static PathPin To(string format) => new("→", format);
    const string F64 = "64-bit 浮點";
    static string Pcm(string word, int rate) => $"{word} / {Khz(rate)} kHz";
    static string Dsd1(int clock) => $"1-bit DSD / {DsdName(clock)}（{Mhz(clock)} MHz）";

    /// <summary>The device's sample format in words: integer or floating point, and its container.</summary>
    static string DeviceWord(string type, int bits, int container) =>
        type.StartsWith("float") ? $"{container}-bit 浮點" : bits != container ? $"{bits}-bit 整數（{container}-bit 容器）" : $"{bits}-bit 整數";

    static string Khz(int r) => (r / 1000.0).ToString("0.###", CultureInfo.InvariantCulture);
    static string Mhz(int r) => (r / 1e6).ToString("0.####", CultureInfo.InvariantCulture);
    static string Db(double d) => (d > 0 ? "+" : "") + d.ToString("0.0", CultureInfo.InvariantCulture) + " dB";
    static string DsdName(int clock) => "DSD" + clock / (clock % 44100 == 0 ? 44100 : 48000);
    static string Hz(double f) => f >= 1000 ? (f / 1000).ToString("0.##", CultureInfo.InvariantCulture) + " kHz" : f.ToString("0", CultureInfo.InvariantCulture) + " Hz";

    string PhaseText() => _opts.Phase switch
    {
        "linear" => "線性相位",
        "minimum" => "最小相位",
        "custom" => $"自訂相位（α {_opts.PhaseCustom.ToString("0.00", CultureInfo.InvariantCulture)}）",
        _ => "中間相位",
    };

    /// <summary>The upsampling filter's details (PCM 升頻, and the pre-filter before the DSD modulator).</summary>
    string[] FilterLines(bool apodize) => new[]
    {
        $"濾波器：{_opts.Taps.ToString("N0", CultureInfo.InvariantCulture)} taps · {PhaseText()}",
        apodize ? "去除錄音振鈴（apodize）：開" : null,
        _opts.DoubleDouble ? "運算精度：double-double（約 106-bit），每個輸出捨入回 64-bit 浮點一次" : "運算精度：64-bit 浮點",
    };

    static string BandName(string type) => type switch
    {
        "LSC" => "低頻架", "HSC" => "高頻架", "LP" => "低通", "HP" => "高通", _ => "峰值",
    };

    /// <summary>
    /// The 音量 row when the core's gain stage does nothing (0 dB, fixed, hardware, or DSD sent directly). Digital 0 dB
    /// and fixed are the gain stage at the head of the DSP chain passing samples through (shown there); hardware and none
    /// happen in the DAC, after the device (shown under 輸出).
    /// </summary>
    PathRow VolumeRow(string volumeMode, bool dsdFile)
    {
        return volumeMode switch
        {
            "hardware" => Row("音量", "DAC 硬體音量 · " + Db(_s.VolumeDb), false, "音量由 DAC 調整，送出的樣本不變"),
            "none" => Row("音量", "無（請使用 DAC 旋鈕）", false, dsdFile ? "DSD 直送不經過數位音量" : null),
            "digital" => Row("音量", "數位音量 · 0 dB（不處理）", false),
            _ => Row("音量", "固定 0 dB", false, "不調整數位增益"),
        };
    }

    /// <summary>The card's sections and its notes (warnings).</summary>
    object BuildPath(Track t, JsonElement src, JsonElement dev, List<string> chain, bool isDsd, bool sentDirect, bool nativeDsd, bool dop,
                     int srcRate, int devRate, string mode, string format, string volumeMode, double rg, List<string> notes)
    {
        var o = _opts;
        bool dsdFile = sentDirect;                 // a DSD file sent as DSD
        bool dsdPcm = isDsd && !sentDirect;        // a DSD file converted to PCM (it may leave as DSD again: the modulator)
        int pcmRate = src.GetProperty("rate").GetInt32();   // where the PCM chain starts (DSD → PCM: the converted rate)
        int channels = src.GetProperty("channels").GetInt32();
        int clock = nativeDsd ? devRate : dop ? devRate * 16 : 0;   // the DSD rate sent
        bool upsampler = SettingsMap.Upsampler(_s, o) != "off";
        var sections = new List<object>();

        // ── 來源
        string srcText = t.IsLive ? $"YouTube Music • {Khz(srcRate)} kHz"
                       : isDsd ? $"{t.Codec} • {DsdName(srcRate)} • {Mhz(srcRate)} MHz"
                               : $"{t.Codec} • {(t.Bits > 0 ? t.Bits + "-bit / " : "")}{Khz(srcRate)} kHz";
        bool srcLossless = src.TryGetProperty("lossless", out var sl) && sl.ValueKind == JsonValueKind.True;
        int srcBits = src.TryGetProperty("bits", out var sb) && sb.ValueKind == JsonValueKind.Number ? sb.GetInt32() : 0;
        // 解碼: what the decoder hands on. PCM: the integers become 64-bit floating point, exactly (24-bit fits the
        // 53-bit mantissa); a DSD file is read as its 1-bit stream (sent as it is, or converted to PCM below)
        // YouTube Music: the browser decodes; its 32-bit float samples come through MIKU's shared ring, read at a
        // slightly adjusted rate so the browser's clock and the DAC's stay together (the interpolation changes samples)
        PathRow decode = t.IsLive
            ? Row("解碼", "瀏覽器解碼 · 32-bit 浮點", true,
                  "YouTube Music 網頁的聲音由瀏覽器（WebView2）解碼，經共用記憶體送進 Daphne",
                  "時鐘校正：瀏覽器與 DAC 的時鐘不同，讀取速率微調（最多 ±0.5%）讓緩衝維持約 0.15 秒（網頁送得不穩、出現斷流時自動加大，最多 0.3 秒，穩定後再降回；學到的值這次執行期間都記得），以 4 點 Hermite 內插",
                  "不丟也不重複樣本，所以沒有爆音")
              with { pin = new[] { To(Pcm(F64, pcmRate)), new PathPin("解碼器", "瀏覽器（WebView2）") } }
            : isDsd
            ? Row("解碼", "讀取 1-bit DSD 串流", false) with { pin = new[] { To(Dsd1(srcRate)), new PathPin("解碼器", mark: true) } }
            // the core's report says whether the samples are exact integers (MIKU's track bits also count float files)
            : Row("解碼", t.IsLossy ? "有損解碼" : srcLossless && srcBits > 0 ? $"{srcBits}-bit 整數轉成浮點" : "浮點樣本", false,
                  t.IsLossy ? "解碼器輸出浮點樣本，轉成 64-bit 浮點"
                  : srcLossless ? "整數樣本放進 64-bit 浮點的 53-bit 尾數，數值完全不變"
                  : "浮點樣本放進 64-bit 浮點，數值不變")
              with { pin = new[] { To(Pcm(F64, pcmRate)), new PathPin("解碼器", mark: true) } };
        sections.Add(new
        {
            title = "來源",
            rows = new[]
            {
                Row("檔案", srcText, false, channels != 2 ? $"聲道：{channels}" : null,
                    t.IsLossy ? "有損壓縮來源：壓縮時已丟棄部分聲音資訊，無法還原" : null),
                decode,
            },
        });

        // ── DSP (the core's chain, in its order)
        var proc = new List<PathRow>();
        // every stage up to the quantizer hands on 64-bit floating point; the rate changes only at the resampler
        int rateNow = pcmRate;
        if (dsdPcm)
            proc.Add(Row("DSD 轉 PCM", $"{DsdName(srcRate)} → PCM", true,
                $"低通濾波：{o.DsdPcmLowpass} kHz（濾掉 DSD 的超音波噪聲）",
                "增益：+6 dB（SACD 0 dB = 0 dBFS，與 PCM 同音量）",
                o.DsdPcmLimiter ? "限幅器：開（超過滿刻度的峰值壓回，不削波）" : "限幅器：關（超過滿刻度時削波）") with { pin = new[] { To(Pcm(F64, pcmRate)) } });
        if (chain.Contains("volume"))
        {
            bool digital = _s.VolumeMode == "digital";
            var parts = new List<string>();
            if (digital) parts.Add(_s.Muted ? "靜音" : "數位音量 " + Db(_s.VolumeDb));
            if (Math.Abs(rg) > 1e-9) parts.Add("ReplayGain " + Db(rg));
            proc.Add(Row("音量", parts.Count > 0 ? string.Join(" · ", parts) : "增益", true,
                digital ? "數位音量：" + (_s.Muted ? "靜音" : Db(_s.VolumeDb)) : null,
                Math.Abs(rg) > 1e-9 ? $"ReplayGain（{(_s.ReplayGain == "album" ? "依專輯" : "依曲目")}）：{Db(rg)}，只在峰值允許的範圍內提高" : null,
                "調整時平滑過渡") with { pin = new[] { To(Pcm(F64, rateNow)) } });
        }
        // digital 0 dB / fixed: the gain stage is there but passes samples through (not reported in the chain)
        bool volPass = !chain.Contains("volume") && !dsdFile && volumeMode is not ("hardware" or "none");
        if (volPass) proc.Add(VolumeRow(volumeMode, dsdFile) with { pin = new[] { To(Pcm(F64, rateNow)) } });
        var d = _s.Dsp;
        if (chain.Contains("crossfeed") && d?.Crossfeed != null)
            proc.Add(Row("Crossfeed", $"{Hz(d.Crossfeed.Fc)} · {d.Crossfeed.Feed.ToString("0.0", CultureInfo.InvariantCulture)} dB", true,
                $"截止頻率：{Hz(d.Crossfeed.Fc)}（低於此頻率的聲音也送到另一耳）",
                $"交叉量：{d.Crossfeed.Feed.ToString("0.0", CultureInfo.InvariantCulture)} dB（直接音比交叉音大的幅度）") with { pin = new[] { To(Pcm(F64, rateNow)) } });
        if (chain.Contains("eq") && d != null)
        {
            var on = d.Bands.Where(b => b.On).ToList();
            var lines = new List<string> { "前級：" + (d.AutoPreamp ? "自動" : Db(Math.Min(0, d.PreampDb))) };
            lines.AddRange(on.Select(b => b.Type is "LP" or "HP"
                ? $"{BandName(b.Type)} {Hz(b.Fc)} · Q {b.Q.ToString("0.##", CultureInfo.InvariantCulture)}"
                : $"{BandName(b.Type)} {Hz(b.Fc)} · {Db(b.Gain)} · Q {b.Q.ToString("0.##", CultureInfo.InvariantCulture)}"));
            proc.Add(Row("EQ", $"{on.Count} 段" + (string.IsNullOrEmpty(d.PresetName) ? "" : " · " + d.PresetName), true, lines.ToArray())
                with { pin = new[] { To(Pcm(F64, rateNow)) } });
        }
        if (chain.Contains("resampler"))
        {
            rateNow = devRate;
            proc.Add(Row("升頻", devRate == pcmRate ? "1 倍 · 只去除錄音振鈴" : $"{Khz(pcmRate)} → {Khz(devRate)} kHz（×{devRate / pcmRate}）", true,
                FilterLines(o.Apodize && upsampler)) with { pin = new[] { To(Pcm(F64, rateNow)) } });
        }
        if (chain.Contains("modulator pre-filter") || chain.Contains("modulator limiter"))
        {
            int modIn = pcmRate % 44100 == 0 ? 705600 : 768000;
            var lines = new List<string> { $"前級升頻：{Khz(pcmRate)} → {Khz(modIn)} kHz" };
            lines.AddRange(FilterLines(o.Apodize).Where(x => x != null));
            lines.Add("限幅器：峰值保持在調變器的穩定範圍內");
            if (clock > 0) lines.Add($"調變器：{o.Order} 階 Δ-Σ → {DsdName(clock)}（{Mhz(clock)} MHz）");
            proc.Add(Row("轉成 DSD", $"{o.Order} 階 Δ-Σ 調變器", true, lines.ToArray()) with { pin = clock > 0 ? new[] { To(Dsd1(clock)) } : null });
        }
        if (chain.Contains("quantizer"))
        {
            int bits = dev.GetProperty("bits").GetInt32();
            string type = dev.GetProperty("type").GetString() ?? "";
            string word = DeviceWord(type, bits, dev.GetProperty("container").GetInt32());
            proc.Add(Row("量化", "TPDF 抖動", true,
                type.StartsWith("float") ? $"量化到 {bits}-bit 精度（浮點裝置的尾數），加 TPDF 抖動（±1 LSB），不做噪聲整形"
                                         : $"量化到裝置的 {bits}-bit，加 TPDF 抖動（±1 LSB），不做噪聲整形")
                with { pin = new[] { To(Pcm(word, devRate)) } });
        }
        if (proc.All(r => !r.mod))
        {
            string vol = volPass ? (volumeMode == "digital" ? "數位音量：0 dB，不處理" : "音量：固定 0 dB，不調整數位增益") : null;
            proc.Clear();
            proc.Add(Row("DSP", dsdFile ? "無：DSD 原樣送出" : "無：樣本數值不變", false, vol)
                with { pin = new[] { To(dsdFile ? Dsd1(srcRate) : Pcm(F64, pcmRate)) } });
        }
        sections.Add(new { title = "DSP", mark = true, rows = proc });   // Daphne's own processing: its mark after the title

        // ── 輸出
        var outRows = new List<PathRow>();
        // the volume that happens in the DAC (hardware, none), also when the gain stage carries only ReplayGain; DSD sent
        // directly has no gain stage at all. The digital volume is always in DSP above.
        if (!volPass && (volumeMode is "hardware" or "none" || !chain.Contains("volume"))) outRows.Add(VolumeRow(volumeMode, dsdFile));
        double latency = _report.TryGetProperty("latency_ms", out var lm) ? lm.GetDouble() : 0;
        long underruns = _report.TryGetProperty("underruns", out var un) ? un.GetInt64() : 0;
        outRows.Add(Row("裝置", $"{mode} · {_report.GetProperty("device_name").GetString()}", false,
            latency > 0 ? $"輸出延遲：約 {latency:0} ms" : null, underruns > 0 ? $"欠載：{underruns} 次" : null));
        if (nativeDsd)
            outRows.Add(Row("格式", $"原生 DSD · {DsdName(clock)}（{Mhz(clock)} MHz）", false, "以 ASIO 原生 DSD 送到 DAC"));
        else if (dop)
            outRows.Add(Row("格式", $"DoP · {format} / {Khz(devRate)} kHz", false,
                $"{DsdName(clock)} 包在 PCM 裡送出（DoP 標記），DAC 解開後以 DSD 播放"));
        else
        {
            string type = dev.GetProperty("type").GetString() ?? "";
            int container = dev.GetProperty("container").GetInt32();
            string word = DeviceWord(type, dev.GetProperty("bits").GetInt32(), container);
            // without a quantizer: an integer device gets bit-perfect samples (the core leaves the quantizer out only
            // then); a float device never has one, and float32 rounds unless the samples are a source's integers
            // (shared mode is never bit-perfect, but what Daphne hands the mixer is the same)
            string why = _report.TryGetProperty("reason", out var wy) ? wy.GetString() ?? "" : "";
            bool bitPerfect = _report.TryGetProperty("bit_perfect", out var bp) && bp.ValueKind == JsonValueKind.True
                || why == "shared mode (system mixer)" || why.StartsWith("the system resamples");
            string conv = chain.Contains("quantizer") ? null
                : !type.StartsWith("float") ? "64-bit 浮點轉回裝置的整數格式，數值不變"
                : container >= 64 || bitPerfect ? "64-bit 浮點轉成裝置的浮點格式，數值不變"
                : $"64-bit 浮點捨入成 {container}-bit 浮點（24-bit 尾數），不加抖動";
            outRows.Add(Row("格式", Pcm(word, devRate), false, conv));
            // the mixer at another rate (WASAPI shared): Windows resamples what Daphne hands it
            int sysRate = _report.TryGetProperty("system_rate", out var sr) && sr.ValueKind == JsonValueKind.Number ? sr.GetInt32() : 0;
            if (sysRate > 0)
                outRows.Add(Row("系統轉換", $"{Khz(devRate)} → {Khz(sysRate)} kHz", true, "Windows 混音器重新取樣到裝置目前的取樣率"));
        }
        sections.Add(new { title = "輸出", rows = outRows });

        return new { ext = "daphne", sections, notes };
    }
}
