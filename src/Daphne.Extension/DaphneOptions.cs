using System;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using Miku;

namespace DaphneMiku;

/// <summary>
/// Daphne's own settings (upsampler, filter, DSD modulator, filter design), kept with the module's settings in MIKU's
/// settings file. The page (web/daphne.js) applies its menus' limits; the core only checks ranges.
/// </summary>
public sealed class DaphneOptions
{
    public string Upsampler { get; set; } = "off";       // off | pcm | dsd
    public string Target { get; set; } = "native";       // upsampler pcm: native | 2x | max | fixed
    public int? FixedRate { get; set; }                  // target fixed: Hz (null: MIKU's 固定取樣率)
    public int Taps { get; set; } = 2048;
    public string Phase { get; set; } = "intermediate";  // linear | intermediate | minimum | custom
    public double PhaseCustom { get; set; } = 0.5;       // phase custom: 0 (linear) .. 1 (minimum)
    public bool Apodize { get; set; }
    public bool DoubleDouble { get; set; }               // double-double convolution
    public int Order { get; set; } = 8;                  // DSD modulator: 5 | 8
    public int Osr { get; set; } = 128;                  // DSD64 .. DSD1024
    public int DsdPcmLowpass { get; set; } = 24;         // DSD → PCM low-pass, kHz: 24 | 30 | 50
    public bool DsdPcmLimiter { get; set; } = true;      // DSD → PCM: limiter instead of clipping above full scale
    public int Threads { get; set; }                     // filter design threads, 0 = automatic
    public int CacheLimitMb { get; set; } = 1024;        // filter cache limit: 500 | 1024 | 2048 MB

    public JsonObject ToJson() => new()
    {
        ["upsampler"] = Upsampler, ["target"] = Target, ["fixedRate"] = FixedRate, ["taps"] = Taps, ["phase"] = Phase,
        ["phaseCustom"] = PhaseCustom, ["apodize"] = Apodize, ["doubleDouble"] = DoubleDouble, ["order"] = Order, ["osr"] = Osr,
        ["dsdPcmLowpass"] = DsdPcmLowpass, ["dsdPcmLimiter"] = DsdPcmLimiter, ["threads"] = Threads, ["cacheLimitMb"] = CacheLimitMb,
    };

    public void Load(JsonElement o) => Merge(o);

    public void Merge(JsonElement o)
    {
        foreach (var p in o.EnumerateObject())
        {
            var v = p.Value;
            switch (p.Name)
            {
                case "upsampler": Upsampler = v.GetString() is "pcm" or "dsd" ? v.GetString() : "off"; break;
                case "target": Target = v.GetString() is "2x" or "max" or "fixed" ? v.GetString() : "native"; break;
                case "fixedRate": FixedRate = v.ValueKind == JsonValueKind.Number ? v.GetInt32() : null; break;
                case "taps": Taps = Math.Clamp(v.GetInt32(), 16, 1 << 20); break;
                case "phase": Phase = v.GetString() is "linear" or "minimum" or "custom" ? v.GetString() : "intermediate"; break;
                case "phaseCustom": PhaseCustom = Math.Clamp(v.GetDouble(), 0, 1); break;
                case "apodize": Apodize = v.GetBoolean(); break;
                case "doubleDouble": DoubleDouble = v.GetBoolean(); break;
                case "order": Order = v.GetInt32() == 5 ? 5 : 8; break;
                case "osr": Osr = v.GetInt32() is 64 or 128 or 256 or 512 or 1024 ? v.GetInt32() : 128; break;
                case "dsdPcmLowpass": DsdPcmLowpass = v.GetInt32() is 30 or 50 ? v.GetInt32() : 24; break;
                case "dsdPcmLimiter": DsdPcmLimiter = v.GetBoolean(); break;
                case "threads": Threads = Math.Clamp(v.GetInt32(), 0, 256); break;
                case "cacheLimitMb": CacheLimitMb = v.GetInt32() is 500 or 1024 or 2048 ? v.GetInt32() : 1024; break;
            }
        }
    }
}

/// <summary>MIKU's settings + Daphne's options → Daphne's settings JSON (core/include/daphne/engine/settings.h) for one track.</summary>
static class SettingsMap
{
    /// <summary>
    /// The upsampler in effect: shared mode never upsamples (Windows' mixer converts everything), and neither does YouTube
    /// Music (a live source): upsampled on ASIO at 352.8 / 384 kHz it left the player hanging in the driver (2026-10-29).
    /// </summary>
    public static string Upsampler(Settings s, DaphneOptions o, Track t) => s.OutputMode == "shared" || t?.IsLive == true ? "off" : o.Upsampler;

    /// <summary>output.rate: the target of the PCM upsampler; the source rate otherwise.</summary>
    static string Rate(Settings s, DaphneOptions o, Track t)
    {
        if (Upsampler(s, o, t) != "pcm") return "native";
        return o.Target switch
        {
            "2x" => "2x",   // twice each track's own rate (gapless across sample rates)
            "max" => "auto",
            "fixed" => (o.FixedRate ?? s.FixedRate).ToString(CultureInfo.InvariantCulture),
            _ => "native",
        };
    }

    /// <summary>
    /// ReplayGain for Daphne's gain stage: the tag's gain plus the preamp, raised only as far as the tag's peak allows.
    /// Without a peak tag the gain only lowers the level. DSD sent directly has no gain stage.
    /// </summary>
    public static double ReplayGainDb(Settings s, Track t) => t == null || t.IsDsd ? 0 : s.ReplayGainDb(t) ?? 0;

    public static string Build(Settings s, DaphneOptions o, Track t, bool hardwareVolume)
    {
        var j = new JsonObject();
        string api = s.OutputMode switch { "asio" => "asio", "shared" => "wasapi_shared", _ => "wasapi_exclusive" };
        string device = s.OutputMode == "asio" ? s.AsioDriver ?? "" : s.DeviceId ?? "";
        string dsd = s.DsdFor(s.OutputMode) switch { "dop" => "dop", "pcm" => "pcm", _ => "auto" };   // native: ASIO native, else DoP
        j["output"] = new JsonObject
        {
            ["api"] = api, ["device"] = device, ["rate"] = Rate(s, o, t), ["dsd"] = dsd,
            ["dsd_pcm_lowpass"] = o.DsdPcmLowpass, ["dsd_pcm_limiter"] = o.DsdPcmLimiter,
            ["max_dsd"] = s.DsdLimit(),   // MIKU's DAC 最高 DSD
            ["buffer_ms"] = Math.Clamp(s.BufferMs, 40, 200),   // MIKU's 緩衝大小, 40 .. 200 ms (WASAPI; ASIO: the driver's)
        };
        j["core"] = Upsampler(s, o, t) == "dsd" ? "dsd" : "pcm";
        j["filter"] = new JsonObject
        {
            ["taps"] = o.Taps, ["phase"] = o.Phase, ["phase_custom"] = o.PhaseCustom,
            // apodizing belongs to the upsampling filter (the PCM upsampler or the DSD pre-filter; off with the upsampler off)
            ["apodize"] = o.Apodize && Upsampler(s, o, t) != "off",
            ["double_double"] = o.DoubleDouble,
        };

        // MIKU's DSP: EQ and crossfeed run in Daphne; balance / invert have no stage in the core
        var d = s.Dsp;
        bool dsp = d != null && d.Enabled;
        var bands = new JsonArray();
        if (dsp && d.EqOn)
            foreach (var b in d.Bands)
            {
                string type = b.Type switch { "LSC" => "LS", "HSC" => "HS", "LP" => "LP", "HP" => "HP", _ => "PK" };
                double q = type is "LS" or "HS" ? ShelfSlope(b.Q, b.Gain) : b.Q;
                bands.Add(new JsonObject { ["type"] = type, ["f"] = b.Fc, ["q"] = Math.Clamp(q, 0.05, 30), ["gain_db"] = b.Gain, ["on"] = b.On });
            }
        var eq = new JsonObject { ["enabled"] = dsp && d.EqOn && bands.Count > 0, ["bands"] = bands };
        eq["preamp_db"] = dsp && !d.AutoPreamp ? JsonValue.Create(Math.Min(0, d.PreampDb)) : JsonValue.Create("auto");
        j["eq"] = eq;
        // MIKU's crossfeed feed (direct vs crossfed level in dB) → the mix amount k: k / (1 - k) = 10^(-feed / 20)
        double r = Math.Pow(10, -(d?.Crossfeed?.Feed ?? 4.5) / 20);
        j["crossfeed"] = new JsonObject
        {
            ["enabled"] = dsp && d.Crossfeed.On, ["fc"] = Math.Clamp(d?.Crossfeed?.Fc ?? 700, 100, 3000), ["amount"] = Math.Clamp(r / (1 + r), 0, 1),
        };

        // volume: digital in the core (also on ASIO); hardware volume is set on the endpoint by the extension (fixed here)
        bool digital = s.VolumeMode == "digital" && !hardwareVolume;
        double db = s.Muted ? -120 : Math.Clamp(s.VolumeDb, -120, 0);
        j["volume"] = new JsonObject { ["mode"] = digital ? "digital" : "fixed", ["db"] = digital ? db : 0 };
        j["headroom_db"] = Math.Clamp(ReplayGainDb(s, t), -24, 24);
        j["dsd_modulator"] = new JsonObject { ["order"] = o.Order, ["osr"] = o.Osr };
        j["design"] = new JsonObject { ["threads"] = o.Threads, ["cache_limit_mb"] = o.CacheLimitMb };
        return j.ToJsonString();
    }

    /// <summary>MIKU's shelf Q → the RBJ shelf slope S that Daphne's LS / HS take (1/Q² = (A + 1/A)(1/S − 1) + 2).</summary>
    static double ShelfSlope(double q, double gainDb)
    {
        double a = Math.Pow(10, gainDb / 40), k = a + 1 / a;
        double s = 1 / ((1 / (q * q) - 2) / k + 1);
        return double.IsFinite(s) && s > 0 ? Math.Min(s, 1.0) : 1.0;
    }
}
