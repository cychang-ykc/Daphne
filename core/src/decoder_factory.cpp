#include <algorithm>

#include "daphne/engine/decoder.h"
#include "daphne/engine/dsd_file.h"
#include "daphne/engine/live_source.h"
#include "daphne/platform/platform.h"

namespace daphne {
namespace {

bool any_of(const std::string& s, std::initializer_list<const char*> list) {
    return std::any_of(list.begin(), list.end(), [&](const char* x) { return s == x; });
}

// dr_flac takes STREAMINFO as it is: a sample rate of 0 opens, and the player's divisions by the rate then
// stop the whole process (SIGFPE), also for a file prepared as the gapless next track
bool usable(const Decoder& d, std::string* error) {
    if (d.info().rate > 0 && d.info().channels > 0) return true;
    if (error) *error = "the file reports no sample rate or no channels";
    return false;
}

}  // namespace

std::string file_extension(const std::filesystem::path& path) {
    const std::string name = platform::path_to_utf8(path.filename());
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string s = name.substr(dot + 1);
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return s;
}

std::unique_ptr<Decoder> open_decoder(const std::filesystem::path& path, std::string* error) {
    if (is_live_path(path)) {   // the host's live input (live_source.h)
        auto l = make_live_decoder();
        if (l->open(path, error)) return l;
        return nullptr;
    }
    const std::string ext = file_extension(path);
    std::unique_ptr<Decoder> d;
    const bool dsd = is_dsd_path(path);
    if (any_of(ext, {"wav", "wave", "bwf", "aiff", "aif", "aifc"})) d = make_wav_decoder();
    else if (any_of(ext, {"flac", "fla"})) d = make_flac_decoder();
    else if (any_of(ext, {"mp3", "mp2", "mp1"})) d = make_mp3_decoder();
    else if (dsd) d = make_dsd_pcm_decoder();
    else d = make_ffmpeg_decoder();   // FFmpeg probes the content
    std::string e;
    if (d->open(path, &e) && usable(*d, &e)) return d;
    // a file the dedicated decoder cannot read (an unusual WAV codec, FLAC data named .wav): FFmpeg once more
    if (!dsd) {
        auto f = make_ffmpeg_decoder();
        if (f->open(path, nullptr) && usable(*f, nullptr)) return f;
    }
    if (error) *error = e;
    return nullptr;
}

}  // namespace daphne
