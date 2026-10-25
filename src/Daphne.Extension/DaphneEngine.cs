using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using NAudio.CoreAudioApi;
using Miku;
using Miku.Audio;

namespace DaphneMiku;

/// <summary>
/// The Daphne playback core for MIKU (DaphneExtension: IAudioCoreProvider): daphne_core.dll (C++) plays the file
/// itself — decoding, the processing chain (resampling filter, DSD modulator), WASAPI / ASIO output, DSD (native /
/// DoP / → PCM), gapless.
///
/// MIKU features on this core:
///   • output mode / device / ASIO driver → Daphne's output; DSD 播放方式 → output.dsd (native = ASIO native DSD, else DoP)
///   • DAC 最高 DSD → output.max_dsd; 緩衝大小 → output.buffer_ms (the WASAPI device buffer)
///   • digital volume, EQ, crossfeed → Daphne (changes glide in place)
///   • hardware volume → the WASAPI endpoint volume (also for DSD sent directly, like the MIKU core)
///   • ReplayGain → Daphne's gain stage (raised only as far as the peak tag allows)
///   • YouTube Music → the core's live input (MIKU's shared ring, path "live:"): the page's audio goes through the same
///     chain (upsampling, EQ, crossfeed, the final quantizer) to the same output
///   • not available on this core: balance, invert
/// </summary>
public sealed partial class DaphneEngine : IAudioEngine
{
    readonly Settings _s;
    readonly DaphneOptions _opts;
    readonly SemaphoreSlim _gate = new(1, 1);
    IntPtr _p;
    Thread _poll;
    volatile bool _disposed;

    // The core's calls that change what plays never run on MIKU's thread: dc_stop / dc_apply wait for a load in
    // progress or a driver that is slow to let go (MIKU froze on a track change while the Audiobyte driver hung).
    // A stop bumps _epoch and runs dc_stop in the background (it cancels a load in progress: DC_ERR_CANCELLED); a load
    // first waits for the stops before it, and is not taken as playing when a stop came while it ran.
    // A load also cancels the one before it (dc_cancel): a track change during a long first design does not wait for
    // the design of the track left; a load that a later one or a stop replaced while it waited is skipped.
    long _epoch, _loadSeq;
    readonly object _nativeLock = new();
    Task _stopping = Task.CompletedTask, _loading = Task.CompletedTask, _applying = Task.CompletedTask;

    Track _track, _next;
    double _pausedAt;
    volatile bool _loaded, _playing;
    double _position, _duration;
    double? _liveLatency;   // YouTube Music: seconds from the page to the DAC (the core's live stats)
    SignalInfo _signal;
    JsonElement _report;
    List<int> _asioRates;
    string _asioDriver;
    DateTime _diagAt;
    static readonly Dictionary<string, List<int>> s_asioRates = new(StringComparer.OrdinalIgnoreCase);
    DeviceCaps _caps;
    MMDevice _hwDevice;
    double? _hwRestoreDb;
    string _hwDeviceId;

    public Func<Track> PeekNext { get; set; }
    public Func<Task> ReleaseOthers { get; set; }
    public event Action<Track> TrackStarted;
    public event Action Ended;
    public event Action<string> Failed;
    public event Action Changed;
    public event Action<Track> Loading;

    readonly Miku.Extensibility.IExtensionHost _host;
    string _designText;
    bool _designing;
    DateTime _designPostAt;

    public DaphneEngine(Settings settings, Action<Action> ui, DaphneOptions options, Miku.Extensibility.IExtensionHost host = null)
    {
        _s = settings;
        _opts = options;
        _host = host;
        _p = DaphneNative.dc_create();
        _poll = new Thread(PollLoop) { IsBackground = true, Name = "daphne poll" };
        _poll.Start();
    }

    // ───────────────────────────── state ─────────────────────────────

    public SignalInfo Signal => _signal;
    public bool LastFailureWasDevice { get; private set; }
    public Track Track => _track;
    public bool IsPlaying => _loaded && _playing;
    public bool IsLoaded => _loaded;
    public DeviceCaps Caps => _caps;
    public double Position => _loaded ? _position : _pausedAt;
    /// <summary>YouTube Music: what the page wrote and the core has not played yet (the ring, the chain and its ring, the device).</summary>
    public double? LiveLatency => _track?.IsLive == true && _loaded ? _liveLatency : null;

    /// <summary>Daphne's events and state, every 50 ms: gapless switches, the end, failures, position.</summary>
    void PollLoop()
    {
        while (!_disposed)
        {
            try
            {
                if (_p != IntPtr.Zero)
                {
                    // events and the design progress never wait for a load in progress (a first design can take minutes)
                    string ev;
                    while ((ev = DaphneNative.PollEvent(_p)) != null) OnEvent(ev);
                    PostDesign(DaphneNative.dc_design(_p));
                }
                if (_p != IntPtr.Zero && _loaded)
                {
                    var st = DaphneNative.State(_p);
                    if (st != null)
                    {
                        using var doc = JsonDocument.Parse(st);
                        var r = doc.RootElement;
                        _position = r.GetProperty("position").GetDouble();
                        _duration = r.GetProperty("duration").GetDouble();
                        _report = r.GetProperty("report").Clone();
                        if (r.TryGetProperty("live", out var lv))
                        {
                            DaphneNative.dc_live_rate(LiveBus.SourceRate);   // the page reports its rate once it runs
                            double fill = lv.GetProperty("fill").GetDouble();
                            double lat = _report.TryGetProperty("latency_ms", out var lm) ? lm.GetDouble() / 1000 : 0;
                            _liveLatency = Math.Max(0, fill + lv.GetProperty("read").GetDouble() - _position + lat);
                            LiveBus.Fill = fill;
                        }
                        else _liveLatency = null;
                        if (r.TryGetProperty("asio_rates", out var ar) && ar.GetArrayLength() > 0)
                        {
                            var list = ar.EnumerateArray().Select(x => x.GetInt32()).ToList();
                            _asioRates = list;
                            if (_asioDriver != null) lock (s_asioRates) s_asioRates[_asioDriver] = list;
                        }
                        // diagnostics in MIKU's log every 2 s while playing
                        if (_playing && DateTime.UtcNow - _diagAt > TimeSpan.FromSeconds(2) && r.TryGetProperty("debug", out var dbg))
                        {
                            _diagAt = DateTime.UtcNow;
                            Log.Info($"[daphne] pos {_position:0.00} state {r.GetProperty("state").GetString()} {dbg.GetRawText()}");
                        }
                    }
                }
            }
            catch (Exception ex) { Log.Error("daphne poll", ex); }
            Thread.Sleep(50);
        }
    }

    void OnEvent(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var e = doc.RootElement;
        switch (e.GetProperty("type").GetString())
        {
            case "track":
            {
                // gapless: the next track is heard now
                var t = _next;
                _next = null;
                if (t == null) break;
                _track = t;
                RefreshReport();
                _signal = BuildSignal();
                TrackStarted?.Invoke(t);
                QueueNext();
                Changed?.Invoke();
                break;
            }
            case "ended":
                _loaded = false;
                _playing = false;
                _pausedAt = 0;
                Ended?.Invoke();
                Changed?.Invoke();
                break;
            case "failed":
            {
                string m = e.TryGetProperty("error", out var x) ? x.GetString() : "playback failed";
                Log.Info("[daphne] " + m);
                _loaded = false;
                _playing = false;
                Failed?.Invoke("Daphne 內核：" + ReadableError(m));
                Changed?.Invoke();
                break;
            }
            case "started":
            case "changed":
                // playback (re)started, or a new filter / processing chain is playing: the signal path follows
                RefreshReport();
                _signal = BuildSignal();
                Changed?.Invoke();
                break;
            case "info":
                if (e.TryGetProperty("text", out var tx))
                {
                    string text = tx.GetString();
                    Log.Info("[daphne] " + text);
                    if (text.StartsWith("designing")) _designText = text;
                }
                break;
        }
    }

    /// <summary>
    /// A filter is being designed (first load, or a rebuild in the background): the page shows a progress popup
    /// (web/daphne.js, event "design").
    /// </summary>
    void PostDesign(double f)
    {
        if (_host == null) return;
        bool active = f < 1.0;
        if (active)
        {
            if (!_designing || DateTime.UtcNow - _designPostAt > TimeSpan.FromMilliseconds(200))
            {
                _designing = true;
                _designPostAt = DateTime.UtcNow;
                _host.Post("design", new { progress = f, text = Describe(_designText), done = false });
            }
        }
        else if (_designing)
        {
            _designing = false;
            _host.Post("design", new { progress = 1.0, text = Describe(_designText), done = true });
            _designText = null;   // the next popup names its own design, never this one
        }
    }

    /// <summary>The core's "designing ..." message in the page's words.</summary>
    static string Describe(string t)
    {
        if (string.IsNullOrEmpty(t)) return "設計濾波器";
        var m = System.Text.RegularExpressions.Regex.Match(t, @"designing the filter for (\d+) -> (\d+) Hz");
        if (m.Success) return $"{int.Parse(m.Groups[1].Value) / 1000.0:0.#} → {int.Parse(m.Groups[2].Value) / 1000.0:0.#} kHz 濾波器";
        m = System.Text.RegularExpressions.Regex.Match(t, @"designing (.+?) in the background");
        return m.Success ? m.Groups[1].Value : "設計濾波器";
    }

    void RefreshReport()
    {
        var st = DaphneNative.State(_p);
        if (st == null) return;
        using var doc = JsonDocument.Parse(st);
        _report = doc.RootElement.GetProperty("report").Clone();
        _duration = doc.RootElement.GetProperty("duration").GetDouble();
    }

    // ───────────────────────────── transport ─────────────────────────────

    bool UseHardwareVolume(bool dsdDirect) =>
        _s.OutputMode != "asio" && (_s.VolumeMode == "hardware" || (dsdDirect && _s.VolumeMode == "digital"));

    string SettingsFor(Track t) => SettingsMap.Build(_s, _opts, t, UseHardwareVolume(false) && _s.VolumeMode == "hardware");

    public async Task LoadAsync(Track t, double seek, bool play)
    {
        LastFailureWasDevice = false;
        if (t == null) return;
        long mine = Interlocked.Increment(ref _loadSeq);
        long epoch = Interlocked.Read(ref _epoch);   // a stop from here on: this load does not play
        Loading?.Invoke(t);
        if (t.IsLive)
        {
            if (!LiveBus.Ready)
            {
                _track = t;
                Failed?.Invoke("YouTube Music 的聲音還沒有接上（頁面尚未載入完成）。");
                return;
            }
            // the core reads the page's audio from MIKU's shared ring; a live track has no position to start from
            DaphneNative.dc_live_bus(LiveBus.Ptr, LiveBus.CapFrames, LiveBus.SourceRate);
            seek = 0;
        }
        else if (play && ReleaseOthers != null)
        {
            try { await Task.WhenAny(ReleaseOthers(), Task.Delay(1500)); } catch (Exception ex) { Log.Error("ReleaseOthers", ex); }
        }
        await EnterReplacing(mine);
        try
        {
            if (Replaced(mine, epoch)) return;   // a later track or a stop came while this load waited
            _track = t;
            _next = null;
            _pausedAt = seek;
            _loaded = false;
            _asioDriver = _s.OutputMode == "asio" ? _s.AsioDriver : null;
            ProbeCaps();
            string json = SettingsFor(t);
            string path = t.IsLive ? "live:" : t.Path;
            await StopsBefore();   // a stop issued before this load must not cancel it
            string err = null;
            Task<int> load;
            lock (_nativeLock)
                _loading = load = Task.Run(() => Replaced(mine, epoch) ? DaphneNative.ErrCancelled : DaphneNative.Load(_p, path, json, seek, play, out err));
            int r = await load;
            if (epoch != Interlocked.Read(ref _epoch))
            {
                // stopped while it loaded: dc_stop cancelled it (DC_ERR_CANCELLED) or stops it now; not playing
                if (r == DaphneNative.Ok) _ = StopNative();
                return;
            }
            // a later track came while it loaded: cancelled (dc_cancel), or started just before; the later load stops it
            if (mine != Interlocked.Read(ref _loadSeq)) return;
            if (r != DaphneNative.Ok) { Fail(r, err); return; }
            _loaded = true;
            _playing = play;
            _position = seek;
            RefreshReport();
            _signal = BuildSignal();
            ApplyHardwareVolume();
            QueueNext();
        }
        finally { _gate.Release(); Changed?.Invoke(); }
    }

    bool Replaced(long mine, long epoch) => mine != Interlocked.Read(ref _loadSeq) || epoch != Interlocked.Read(ref _epoch);

    /// <summary>
    /// Waits for the gate while the load holding it (an earlier track) is cancelled. Repeated every 50 ms while this
    /// is the latest load: the earlier one may not have reached dc_load yet, where a cancel takes effect. dc_cancel
    /// returns at once (it sets a flag) and only touches a load in progress, which can only be the earlier one while
    /// this one waits; it is called here, not queued, so it never lands after this load has entered.
    /// </summary>
    async Task EnterReplacing(long mine)
    {
        var enter = _gate.WaitAsync();
        while (!enter.IsCompleted && !_disposed)
        {
            var p = _p;
            if (mine == Interlocked.Read(ref _loadSeq) && p != IntPtr.Zero) DaphneNative.dc_cancel(p);
            await Task.WhenAny(enter, Task.Delay(50));
        }
        await enter;
    }

    // The core's messages are English (they go to the log as they are); the common ones are shown to the user in
    // MIKU's language, anything else as it came. The same table as miku/engine.js, plus WASAPI's HRESULTs.
    static readonly (Regex re, Func<Match, string> text)[] Readable =
    {
        (new Regex(@"cannot open (FLAC|WAV/AIFF|MP3) file|FFmpeg: (cannot open file|no stream info|no audio stream)|not a DSF or DFF file|file too short"),
            _ => "檔案損毀，或不是支援的音訊格式"),
        (new Regex(@"cannot map (\d+) source channels to (\d+)"), m => $"裝置有 {m.Groups[2].Value} 個聲道，無法播放 {m.Groups[1].Value} 聲道的檔案"),
        (new Regex(@"is in exclusive use by another application|HRESULT 0x8889000A"), _ => "另一個程式正在獨佔這個裝置"),
        (new Regex(@"does not offer exclusive \(hog\) mode|HRESULT 0x8889000E"), _ => "這個裝置不提供獨佔模式"),
    };

    static string ReadableError(string err)
    {
        string s = err ?? "";
        foreach (var r in Readable)
        {
            var m = r.re.Match(s);
            if (m.Success) return r.text(m);
        }
        return s;
    }

    int Fail(int code, string err)
    {
        if (code == DaphneNative.ErrCancelled)   // stopped while it loaded: nothing to report
        {
            Log.Info("[daphne] load cancelled (stop)");
            return code;
        }
        LastFailureWasDevice = code == DaphneNative.ErrDevice;
        _signal = null;
        Log.Info($"[daphne] load failed ({code}): {err}");
        Failed?.Invoke(code == DaphneNative.ErrDevice ? "Daphne 內核無法開啟輸出裝置：" + ReadableError(err) : "Daphne 內核無法播放：" + ReadableError(err));
        return code;
    }

    /// <summary>Gapless: tell the core what follows (it continues without a gap when the format matches).</summary>
    void QueueNext()
    {
        if (!_loaded || !_s.Gapless || _track?.IsLive == true) { DaphneNative.dc_set_next(_p, null); return; }   // a live stream has no end
        var n = PeekNext?.Invoke();
        if (n == null || n.IsLive || n.Codec == "CD" || string.IsNullOrEmpty(n.Path)) { _next = null; DaphneNative.dc_set_next(_p, null); return; }
        _next = n;
        // its own ReplayGain goes with it: in track mode a gapless album gets each track's gain at the boundary
        DaphneNative.dc_set_next_gain(_p, n.Path, SettingsMap.ReplayGainDb(_s, n));
    }

    public void Pause()
    {
        if (_loaded) DaphneNative.dc_pause(_p, 1);
        _playing = false;
        _pausedAt = Position;
        Changed?.Invoke();
    }

    public void Resume()
    {
        if (_loaded) { DaphneNative.dc_pause(_p, 0); _playing = true; Changed?.Invoke(); return; }
        var t = _track;
        if (t != null) _ = LoadAsync(t, _pausedAt, true);
    }

    public Task SeekAsync(double pos)
    {
        if (_track?.IsLive == true) return Task.CompletedTask;   // YouTube Music seeks in its own page
        if (!_loaded) { _pausedAt = pos; return Task.CompletedTask; }
        DaphneNative.dc_seek(_p, pos);
        _position = pos;
        Changed?.Invoke();
        return Task.CompletedTask;
    }

    /// <summary>dc_stop in the background (never on MIKU's thread); a load in progress is cancelled by it.</summary>
    Task StopNative()
    {
        var p = _p;
        lock (_nativeLock)
        {
            var before = _stopping;
            _stopping = Task.Run(async () =>
            {
                await before;
                try { if (p != IntPtr.Zero) DaphneNative.dc_stop(p); }
                catch (Exception ex) { Log.Error("daphne stop", ex); }
            });
            return _stopping;
        }
    }

    Task StopsBefore() { lock (_nativeLock) return _stopping; }

    public void Stop()
    {
        Interlocked.Increment(ref _epoch);
        _ = StopNative();
        _loaded = false;
        _playing = false;
        _track = null; _next = null; _signal = null; _pausedAt = 0;
        RestoreHardwareVolume();
        Changed?.Invoke();
    }

    public async Task ReconfigureAsync()
    {
        var t = _track; double pos = Position; bool play = IsPlaying;
        await _gate.WaitAsync();
        try { Interlocked.Increment(ref _epoch); await StopNative(); _loaded = false; }
        finally { _gate.Release(); }
        if (t != null) await LoadAsync(t, pos, play);
    }

    /// <summary>Daphne's own settings changed (the page): the core applies them in place / rebuilds / restarts the device.</summary>
    public Task ApplyOptionsAsync()
    {
        if (_loaded && _track != null) Apply();
        return Task.CompletedTask;
    }

    void Apply()
    {
        if (!_loaded || _track == null) return;
        // in the background, in order: dc_apply waits for a load or a stop in progress
        string json = SettingsFor(_track);
        var p = _p;
        lock (_nativeLock)
        {
            var before = _applying;
            _applying = Task.Run(async () =>
            {
                await before;
                if (p == IntPtr.Zero || _disposed) return;
                int r = DaphneNative.Apply(p, json, out var err);
                if (r != DaphneNative.Ok) Log.Info("[daphne] settings: " + err);
            });
        }
    }

    public void InvalidateNext() => QueueNext();

    /// <summary>
    /// PCM rates of the ASIO driver this core has open (the core probes them while opening it), so MIKU's settings page
    /// doesn't open the driver a second time: ASIO drivers often allow one client, and a second open stops this core's stream.
    /// </summary>
    public List<int> AsioRates(string driver)
    {
        if (_s.OutputMode != "asio" || driver == null) return null;
        bool mine = string.Equals(driver, _asioDriver ?? _s.AsioDriver, StringComparison.OrdinalIgnoreCase);
        if (!mine) return null;
        if (_asioRates != null) return _asioRates;
        lock (s_asioRates) if (s_asioRates.TryGetValue(driver, out var cached)) return cached;
        // open in DSD mode (no PCM list): never let the page open the driver while we stream on it
        return _loaded ? new List<int> { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 } : null;
    }

    // ───────────────────────────── volume & dsp ─────────────────────────────

    bool DsdDirect => _report.ValueKind == JsonValueKind.Object && _report.GetProperty("source").GetProperty("dsd_rate").GetInt32() > 0 &&
                      (_report.GetProperty("filter").GetString() ?? "").Contains("DSD file sent directly");

    public void ApplyVolume()
    {
        Apply();
        ApplyHardwareVolume();
        if (_loaded) _signal = BuildSignal();
    }

    public void ApplyDsp()
    {
        Apply();
        if (_loaded) _signal = BuildSignal();
        Changed?.Invoke();
    }

    void ProbeCaps()
    {
        if (_s.OutputMode == "asio") { _caps = null; _hwDeviceId = null; return; }
        try { using var d = Devices.Open(_s.DeviceId); _hwDeviceId = d.ID; _caps = Devices.Probe(d); }
        catch { _caps = null; _hwDeviceId = null; }
    }

    /// <summary>Hardware volume: the hardware mode, or the digital mode while DSD is sent directly (no digital volume on DSD).</summary>
    void ApplyHardwareVolume()
    {
        bool dsd = DsdDirect;
        bool hw = _s.OutputMode != "asio" && (_s.VolumeMode == "hardware" || (dsd && _s.VolumeMode == "digital"));
        if (!hw || _hwDeviceId == null) { if (!dsd) RestoreHardwareVolume(); return; }
        try
        {
            _hwDevice ??= Devices.Open(_hwDeviceId);
            var v = _hwDevice.AudioEndpointVolume;
            if (dsd && _s.VolumeMode == "digital" && _hwRestoreDb == null) _hwRestoreDb = v.MasterVolumeLevel;
            v.MasterVolumeLevel = (float)Math.Clamp(_s.VolumeDb, v.VolumeRange.MinDecibels, v.VolumeRange.MaxDecibels);
            v.Mute = _s.Muted;
        }
        catch (Exception ex) { Log.Error("daphne HW volume", ex); }
    }

    void RestoreHardwareVolume()
    {
        if (_hwRestoreDb == null || _hwDevice == null) return;
        try { var v = _hwDevice.AudioEndpointVolume; v.MasterVolumeLevel = (float)_hwRestoreDb.Value; v.Mute = false; } catch { }
        _hwRestoreDb = null;
    }

    public (double l, double r, long clips, long underruns) Meter()
    {
        if (!_loaded) return (0, 0, 0, 0);
        DaphneNative.dc_meter(_p, out double l, out double r);
        long u = _report.ValueKind == JsonValueKind.Object ? _report.GetProperty("underruns").GetInt64() : 0;
        long c = _report.ValueKind == JsonValueKind.Object ? _report.GetProperty("clipped").GetInt64() : 0;
        return (l, r, c, u);
    }

    // ───────────────────────────── signal path ─────────────────────────────

    /// <summary>The loaded track's PCM rate (DSD files: after conversion) and the device rate, for the settings page; null when nothing is loaded.</summary>
    public object NowPlaying()
    {
        if (!_loaded || _report.ValueKind != JsonValueKind.Object) return null;
        try
        {
            var src = _report.GetProperty("source");
            var dev = _report.GetProperty("device");
            // dsd: the DSD rate of a DSD file sent directly (0 when it is converted to PCM)
            return new { source = src.GetProperty("rate").GetInt32(), dsd = DsdDirect ? src.GetProperty("dsd_rate").GetInt32() : 0, device = dev.GetProperty("rate").GetInt32() };
        }
        catch (Exception) { return null; }
    }

    SignalInfo BuildSignal()
    {
        var t = _track;
        if (t == null || _report.ValueKind != JsonValueKind.Object) return null;
        var src = _report.GetProperty("source");
        var dev = _report.GetProperty("device");
        string filter = _report.GetProperty("filter").GetString() ?? "";
        var chain = _report.GetProperty("chain").EnumerateArray().Select(x => x.GetString()).ToList();
        int dsdRate = src.GetProperty("dsd_rate").GetInt32();
        bool isDsd = dsdRate > 0 || t.IsDsd;
        string devType = dev.GetProperty("type").GetString() ?? "";
        bool nativeDsd = devType.StartsWith("DSD");
        bool dop = filter.Contains("DoP");
        bool dsdOut = nativeDsd || dop;
        int devRate = dev.GetProperty("rate").GetInt32();
        // the core's output rate; WASAPI shared may convert it to the mixer's rate (system_rate) after that
        int sysRate = _report.TryGetProperty("system_rate", out var sr) && sr.ValueKind == JsonValueKind.Number ? sr.GetInt32() : 0;
        int srcRate = dsdRate > 0 ? dsdRate : src.GetProperty("rate").GetInt32();
        string mode = _s.OutputMode switch { "asio" => "ASIO", "shared" => "WASAPI 共享", _ => "WASAPI 獨佔" };
        // a DSD file sent as DSD: the core's own word for it. A DSD file converted to PCM may still leave as DSD again
        // (the DSD modulator), which is processed (and PCM → DSD keeps the core's digital volume)
        bool sentDirect = isDsd && filter.StartsWith("none (DSD file sent directly)");
        bool dsdFile = sentDirect;
        // DSP: what changes the sound (MIKU's EQ, crossfeed); the volume and ReplayGain have their own rows
        var dsp = chain.Where(x => x is "eq" or "crossfeed").Select(x => x == "eq" ? "EQ" : "Crossfeed").ToList();
        double rg = SettingsMap.ReplayGainDb(_s, t);
        string format = nativeDsd ? "DSD 原生" : FormatText(devType, dev.GetProperty("bits").GetInt32(), dev.GetProperty("container").GetInt32());
        string volumeMode = dsdFile && _s.VolumeMode == "digital" ? (_caps != null && _caps.HardwareVolume ? "hardware" : "none") : _s.VolumeMode;
        var info = new SignalInfo
        {
            Codec = t.Codec,
            SourceRate = srcRate,
            SourceBits = isDsd ? 1 : t.Bits,
            Dsd = isDsd,
            DsdLabel = isDsd && srcRate > 0 ? "DSD" + (srcRate / (srcRate % 44100 == 0 ? 44100 : 48000)) : null,
            Lossy = t.IsLossy,
            Dop = dop,
            DsdDirect = dsdOut,
            DsdTransport = nativeDsd ? "Native" : dop ? "Dop" : null,
            Resampled = isDsd ? !sentDirect : dsdOut || devRate != srcRate || sysRate > 0,   // DSD → PCM (→ DSD), PCM → DSD, another rate
            OutputRate = sysRate > 0 ? sysRate : devRate,
            OutputFormat = format,
            OutputBits = nativeDsd ? 1 : dev.GetProperty("bits").GetInt32(),
            Mode = mode,
            Device = _report.GetProperty("device_name").GetString(),
            DspActive = dsp.Count > 0,
            DspSummary = dsp.Count > 0 ? string.Join(" · ", dsp) : null,
            ReplayGainDb = Math.Abs(rg) > 1e-9 ? rg : null,
            Decoder = t.IsLive ? "WebView 音訊" : "Daphne",
            SourceChannels = src.GetProperty("channels").GetInt32(),
            VolumeMode = volumeMode,
        };
        // warnings under the card
        var notes = new List<string>();
        if (_s.Dsp != null && _s.Dsp.Enabled && (Math.Abs(_s.Dsp.Balance) > 0.001 || _s.Dsp.Invert))
            notes.Add("Daphne 內核沒有平衡 / 反相，這兩項設定不會作用。");
        if (sysRate > 0)
            notes.Add($"裝置目前是 {Khz(sysRate)} kHz，由 Windows 把 {Khz(devRate)} kHz 轉成裝置的取樣率。");
        if (dsdFile && volumeMode == "none")
            notes.Add("DSD 直送時無法使用數位音量，DAC 會以原始音量輸出，請用 DAC 的音量旋鈕調整。");
        long clipped = _report.TryGetProperty("clipped", out var cl) ? cl.GetInt64() : 0;
        if (clipped > 0) notes.Add($"已有 {clipped:N0} 個樣本超過滿刻度被削波；請調低數位音量或 EQ 前級。");
        info.Note = notes.Count > 0 ? string.Join("\n", notes) : null;
        info.Custom = BuildPath(t, src, dev, chain, isDsd, sentDirect, nativeDsd, dop, srcRate, devRate, mode, format, volumeMode, rg, notes);
        info.Quality = Quality(info, _report.GetProperty("bit_perfect").GetBoolean(), sentDirect);
        return info;
    }

    /// <summary>The device sample format the way MIKU writes it (24-bit, 24-bit（32-bit 容器）, 32-bit 浮點).</summary>
    static string FormatText(string type, int bits, int container) =>
        type.StartsWith("float") ? container + "-bit 浮點" : bits != container ? $"{bits}-bit（{container}-bit 容器）" : $"{bits}-bit";

    string Quality(SignalInfo i, bool bitPerfect, bool sentDirect)
    {
        if (i.Lossy) return "low";
        if (i.Mode == "WASAPI 共享") return "high";
        if (i.DsdDirect) return sentDirect ? "bitperfect" : "enhanced";   // a DSD file sent as DSD; DSD → PCM → DSD, PCM → DSD are processed
        return bitPerfect ? "bitperfect" : "enhanced";
    }

    public void RefreshSignal()
    {
        if (_loaded) _signal = BuildSignal();
    }

    public void Dispose()
    {
        _disposed = true;
        Interlocked.Increment(ref _epoch);
        var stop = StopNative();   // cancels a load in progress
        RestoreHardwareVolume();
        _hwDevice?.Dispose();
        _hwDevice = null;
        // dc_destroy only when no call is left on the player (the poll thread, a load, a stop, a settings change):
        // freeing it under one would use freed memory. A driver that hangs keeps a call in it: then the player is left
        // as it is (the process is ending) rather than freed under that call.
        bool idle = false;
        try
        {
            bool pollDone = _poll == null || _poll.Join(2000);
            Task[] calls;
            lock (_nativeLock) calls = new[] { stop, _loading, _applying };
            idle = pollDone && Task.WaitAll(calls, 5000);
        }
        catch (Exception ex) { Log.Error("daphne dispose", ex); }
        if (!idle) { Log.Info("[daphne] a call is still in the core at exit (a driver not letting go): the player is not freed"); return; }
        if (_p != IntPtr.Zero) { DaphneNative.dc_destroy(_p); _p = IntPtr.Zero; }
    }
}
