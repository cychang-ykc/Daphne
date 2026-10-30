using System;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using System.Threading.Tasks;
using Miku;
using Miku.Audio;
using Miku.Extensibility;

namespace DaphneMiku;

/// <summary>
/// Daphne as a MIKU extension module: the Daphne playback core (Settings → 播放方案 → Daphne, daphne_core.dll) and its
/// page parts (web/daphne.js: Daphne's own settings, the filter design progress, its row in 關於).
/// </summary>
public sealed class DaphneExtension : IMikuExtension, IAudioCoreProvider
{
    public string Id => "daphne";
    public string CoreId => "daphne";
    public string CoreName => "Daphne";

    public AudioCoreInfo Info => new()
    {
        AsioDsd = true,          // native DSD or DoP on ASIO
        DsdPcmRate = false,      // DSD → PCM at 176.4 / 192 kHz (or the shared mixer's rate), picked by the core
        UpsamplingTitle = "Daphne 升頻",
        UpsamplingText = new()
        {
            ["off"] = "Daphne 不使用這個設定：升頻在上方 Daphne 設定的「升頻」中選擇",
            ["2x"] = "Daphne 不使用這個設定：升頻在上方 Daphne 設定的「升頻」中選擇",
            ["max"] = "Daphne 不使用這個設定：升頻在上方 Daphne 設定的「升頻」中選擇",
            ["fixed"] = "Daphne 不使用這個設定：升頻在上方 Daphne 設定的「升頻」中選擇",
        },
    };

    IExtensionHost _host;
    readonly DaphneOptions _opts = new();
    readonly WeakReference<DaphneEngine> _engine = new(null);
    Settings _settings;   // MIKU's settings (the filters they use are kept when the cache is pruned)

    public void Start(IExtensionHost host)
    {
        _host = host;
        if (host.GetSetting("options") is { ValueKind: JsonValueKind.Object } o) _opts.Load(o);
    }

    public IAudioEngine CreateEngine(Settings settings, Action<Action> ui)
    {
        _settings = settings;
        var e = new DaphneEngine(settings, ui, _opts, _host);
        _engine.SetTarget(e);
        return e;
    }

    public async Task<object> HandleRpc(string method, JsonElement a)
    {
        switch (method)
        {
            case "state":
                return State();
            case "openCache":
            {
                // the filter cache folder in Explorer (created if it isn't there yet)
                string dir = DaphneNative.FilterCacheDir();
                if (dir == null) return false;
                System.IO.Directory.CreateDirectory(dir);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", "\"" + dir + "\"") { UseShellExecute = true });
                return true;
            }
            case "clearCache":
                // everything but the filters the current settings use
                await Task.Run(() => Prune(0));
                return State();
            case "set":
            {
                // Daphne's own settings changed: in place / rebuild / restart as the core decides
                if (a.ValueKind == JsonValueKind.Object)
                {
                    _opts.Merge(a);
                    _host.SetSetting("options", JsonDocument.Parse(_opts.ToJson().ToJsonString()).RootElement);
                    if (_engine.TryGetTarget(out var e)) await e.ApplyOptionsAsync();
                    if (a.TryGetProperty("cacheLimitMb", out _)) await Task.Run(() => Prune(_opts.CacheLimitMb));
                }
                return State();
            }
            default:
                return null;
        }
    }

    object State()
    {
        string ver = null;
        try { ver = DaphneNative.Version; } catch (Exception ex) { _host?.Log("[daphne] daphne_core.dll: " + ex.Message); }
        bool engine = _engine.TryGetTarget(out var e);
        object now = engine ? e.NowPlaying() : null;
        bool live = engine && e.LiveTrack;   // YouTube Music: the page greys out 升頻
        return new { commit = Commit, version = ver, cache = CacheInfo(), now, live, options = JsonDocument.Parse(_opts.ToJson().ToJsonString()).RootElement };
    }

    void Prune(int limitMb)
    {
        try
        {
            string json = _settings != null ? SettingsMap.Build(_settings, _opts, null, false) : "{}";
            if (DaphneNative.PruneFilterCache(json, limitMb) < 0) _host?.Log("[daphne] cache prune: settings did not parse");
        }
        catch (Exception ex) { _host?.Log("[daphne] cache prune: " + ex.Message); }
    }

    /// <summary>The filter cache folder with the number of designs in it and their size (關於).</summary>
    object CacheInfo()
    {
        try
        {
            string dir = DaphneNative.FilterCacheDir();
            if (dir == null) return new { dir = (string)null, files = 0, bytes = 0L };
            var di = new System.IO.DirectoryInfo(dir);
            if (!di.Exists) return new { dir, files = 0, bytes = 0L };
            var files = di.GetFiles();
            return new { dir, files = files.Length, bytes = files.Sum(f => f.Length) };
        }
        catch (Exception ex) { _host?.Log("[daphne] cache info: " + ex.Message); return null; }
    }

    /// <summary>The Daphne commit this module was built from (Daphne.Extension.csproj), shown in 關於.</summary>
    static string Commit => typeof(DaphneExtension).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
        .FirstOrDefault(x => x.Key == "DaphneCommit")?.Value;

    public void Stop() { }
}
