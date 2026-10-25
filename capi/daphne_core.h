/* daphne_core C ABI: the player for host applications (daphne_core.dll / libdaphne_core.so / .dylib).
 * Calls only go host -> library; the library never calls back into the host. Events and state are
 * polled as JSON (UTF-8). Errors are return codes plus an optional message buffer; no exception
 * crosses this boundary. Every function may be called from any thread. dc_load, dc_apply, dc_stop and dc_destroy
 * run one at a time (a load can block for minutes: a first long filter design, or a device that is slow to open);
 * dc_state, dc_meter, dc_design, dc_poll_event, dc_pause, dc_seek and dc_set_next* never wait for them. */
#ifndef DAPHNE_CORE_H
#define DAPHNE_CORE_H

#include <stdint.h>
#include <wchar.h>

#ifdef _WIN32
#  ifdef DC_BUILDING_DLL
#    define DC_API __declspec(dllexport)
#  else
#    define DC_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define DC_API __attribute__((visibility("default")))
#else
#  define DC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DC_OK 0
#define DC_ERR_ARG -1
#define DC_ERR_SETTINGS -2   /* the settings JSON has errors (message lists them) */
#define DC_ERR_FILE -3       /* the file could not be opened / decoded */
#define DC_ERR_DEVICE -4     /* the output device could not be opened / started */
#define DC_ERR_BUFFER -5     /* the output buffer is too small (the return value of *_json functions) */
#define DC_ERR_INTERNAL -6   /* the engine failed inside (out of memory, an unexpected error; message says what) */
#define DC_ERR_CANCELLED -7  /* dc_load: a dc_stop, dc_cancel or dc_destroy came while it was starting (ABI 4) */

typedef struct dc_player dc_player;

/* 1: the first version; 2: the UTF-8 path functions (*_utf8); 3: dc_keep_device, dc_release_device, dc_device_kept;
 * 4: dc_cancel, a dc_stop that cancels a dc_load in progress (DC_ERR_CANCELLED), dc_state's "loading". */
DC_API int dc_abi_version(void);
DC_API const char* dc_version(void);

DC_API dc_player* dc_create(void);
/* Stops playback and frees the player. A dc_load in progress on another thread is cancelled (as by dc_stop) and
 * waited for. No other call on this player (a host's poll thread: state, events, meter) may be in progress, and none
 * may start afterwards: wait for them first. */
DC_API void dc_destroy(dc_player* p);

/* Paths: the functions taking wchar_t paths are the original ones (UTF-16 on Windows; on other systems wchar_t
 * is UTF-32); the *_utf8 variants take UTF-8 and are the portable choice for new hosts (N25). */

/* Opens path (UTF-16) with the settings (settings JSON, see settings.h; output.api / output.device
 * choose the device) and starts at `seconds`, playing or paused. Blocks until playback has started or
 * failed (a long filter design is reported through dc_state "design" meanwhile). A playing track is
 * stopped first. */
DC_API int dc_load(dc_player* p, const wchar_t* path, const char* settings_json, double seconds, int play, char* err, int err_len);
DC_API int dc_load_utf8(dc_player* p, const char* path, const char* settings_json, double seconds, int play, char* err, int err_len);
/* New settings for the playing track (in place, rebuild + crossfade, or device restart as needed).
 * DC_ERR_SETTINGS when the JSON has errors, DC_ERR_INTERNAL when applying them failed (err says why). */
DC_API int dc_apply(dc_player* p, const char* settings_json, char* err, int err_len);
/* Pause / seek the playing track (nothing while a dc_load has not started its player yet: dc_load's `play` and
 * `seconds` are the ones it starts with). */
DC_API void dc_pause(dc_player* p, int paused);
DC_API void dc_seek(dc_player* p, double seconds);
/* Gapless: the track to continue with when this one ends (same output format only; another sample rate
 * when it plays on the same output); NULL / "" clears. */
DC_API void dc_set_next(dc_player* p, const wchar_t* path);
DC_API void dc_set_next_utf8(dc_player* p, const char* path);
/* dc_set_next with the next track's own headroom (dB, settings "headroom_db": ReplayGain per track), applied where
 * its first sample is decoded; NaN keeps the current one. */
DC_API void dc_set_next_gain(dc_player* p, const wchar_t* path, double headroom_db);
DC_API void dc_set_next_gain_utf8(dc_player* p, const char* path, double headroom_db);
/* Stops playback (DSD: the DAC is returned to PCM first) and waits for it. A dc_load in progress is cancelled first
 * (it returns DC_ERR_CANCELLED): a filter design or the prefill stops at once, then dc_stop waits for the load to
 * return. A device that hangs in its driver while opening cannot be interrupted; a host keeps dc_stop off its UI thread
 * for that. Not yet watertight (N26): a cancel that meets the device starting can let a few ms play and leaves that
 * load's "started" event and report behind. */
DC_API void dc_stop(dc_player* p);
/* ABI 4: cancels a dc_load in progress (it returns DC_ERR_CANCELLED) and returns at once, without stopping what
 * plays otherwise: for a host whose stop is queued behind its load (daphne-helper), so the load is not waited for
 * before the stop reaches dc_stop. No effect when nothing is loading, including a dc_load that has not taken the
 * player's operation lock yet. */
DC_API void dc_cancel(dc_player* p);

/* macOS, exclusive mode: keep the device between tracks (MIKU's way; process-wide). On: a Core Audio output closed in
 * exclusive mode (dc_stop, the next dc_load, the end of the last track) stays open: hog mode, rate and DAC format, the
 * IO running silent. The next dc_load on the same device takes it over and sets a new rate on it directly (one DAC
 * relock), and macOS does not move the system output away and back in between. Another device or shared mode puts it
 * back first. dc_release_device() puts it back (rate, DAC format) and releases hog mode, 1 when one was kept; off
 * releases it too. dc_device_kept(): 1 while a device is kept with nothing playing on it. No effect elsewhere. */
DC_API void dc_keep_device(int on);
DC_API int dc_release_device(void);
DC_API int dc_device_kept(void);

/* State snapshot as JSON (never waits for a dc_load in progress: "loading" meanwhile, with "design"):
 *   {"state": "idle|loading|playing|paused|ended|failed", "position": s, "duration": s, "track": n, "design": 0..1,
 *    "error": "...", "report": {"source": {...}, "device": {...}, "device_name": "...", "system_rate": n, "bit_perfect": b,
 *    "reason": "...", "filter": "...", "chain": [...], "latency_ms": x, "underruns": n, "clipped": n, "realtime": x}}
 * "realtime": the processing speed against real time while playing (audio seconds per second of work over the last
 * 2 s; 0 before the first measurement and for DSD files sent directly); below about 1.2 the device plays gaps.
 * "track" counts gapless transitions since dc_load; "asio_rates": PCM rates the open ASIO driver accepts (hosts use
 * them instead of opening the driver a second time); "debug": internal state for logs.
 * Returns the length, or DC_ERR_BUFFER. */
DC_API int dc_state(dc_player* p, char* json, int len);
/* The oldest queued event as JSON, removing it; returns its length, 0 when there is none, or
 * DC_ERR_BUFFER (the event stays queued).
 *   {"type": "started"}            playback started (again after a device restart)
 *   {"type": "track"}              gapless: the next track (dc_set_next) is now heard
 *   {"type": "changed"}            the active filter / processing chain changed (the report in dc_state is new)
 *   {"type": "ended"}              the end of the last track was heard
 *   {"type": "failed", "error": "..."}
 *   {"type": "info", "text": "..."} a settings change was applied, a filter is being designed, ... */
DC_API int dc_poll_event(dc_player* p, char* json, int len);
/* Filter design progress 0..1 (1 = nothing being designed); never blocks, also during dc_load. */
DC_API double dc_design(dc_player* p);
/* Peak level heard since the last call, 0..1 (PCM). */
DC_API void dc_meter(dc_player* p, double* l, double* r);

/* The filter cache folder (UTF-16, NUL-terminated); returns its length, 0 when the cache is disabled
 * (DAPHNE_FILTER_CACHE=off), DC_ERR_BUFFER, or DC_ERR_INTERNAL when the folder could not be worked out. */
DC_API int dc_filter_cache_dir(wchar_t* out, int len);
/* The same in UTF-8 (NUL-terminated; the length in bytes). */
DC_API int dc_filter_cache_dir_utf8(char* out, int len);
/* Keeps the filter cache at most limit_mb (0: deletes everything it may), least recently used first,
 * never the filters `settings` (a settings document, UTF-8) use. Returns the bytes left in the cache,
 * or -1 when the settings do not parse. */
DC_API long long dc_filter_cache_prune(const char* settings, int limit_mb);

/* Output devices as JSON: {"wasapi": [{"id","name","default"}], "asio": [{"name","clsid"}],
 * "coreaudio": [{"uid","name","default","rate","channels","rates","transport","hog_pid"}]} (each list empty on the
 * platforms without that API; output.device takes the WASAPI id, the ASIO name or the Core Audio uid). */
DC_API int dc_devices(char* json, int len);

/* Live input (a browser page's audio tap): the host's shared ring, read by dc_load with the path L"live:".
 * Layout: uint32 write index in frames (wraps) | 12 bytes reserved | float32 stereo frames (cap_frames). The
 * producer writes the frames first, then the index with a release (or seq_cst) atomic store. The memory must stay
 * valid until dc_live_bus(NULL, 0, 0) (or another ring) returns: it returns once no read is left in the previous
 * ring, so the host may free it then, even while a live track plays. Call it from one thread only. A new ring is
 * read from its writer on (the decoder syncs to it, as at the start: N2), whatever its write index. rate: the producer's
 * sample rate (dc_live_rate when it changes). While a live track plays, dc_state has
 *   "live": {"fill": s buffered in the ring, "read": s read, "resyncs": n, "starves": n, "adjust": x,
 *            "target": s of jitter buffer aimed for now (0.15, up to 0.30 after starves)}
 * (seconds from the producer to the DAC: fill + read - position + latency_ms / 1000). */
DC_API void dc_live_bus(void* base, int cap_frames, int rate);
DC_API void dc_live_rate(int rate);
/* Reserved, not implemented yet (returns 0): the producer's frames on a Unix socket (path UTF-8) instead of a
 * shared ring (macOS MIKU's live-relay.js). dc_live_bus(NULL, 0, 0) would end it. */
DC_API int dc_live_socket(const char* path, int rate);

#ifdef __cplusplus
}
#endif
#endif /* DAPHNE_CORE_H */
