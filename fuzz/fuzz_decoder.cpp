// open_decoder on arbitrary bytes: the first byte picks the file type (the decoder is chosen by the extension),
// the rest is the file. Open, read through (bounded), seek to the middle and read again.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "daphne/engine/decoder.h"
#include "fuzz_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    static const char* const kExt[] = {".wav", ".aiff", ".flac", ".mp3"};
    const auto path = fuzz::input_file(kExt[data[0] % 4], data + 1, size - 1);
    if (path.empty()) return 0;
    std::string e;
    auto d = daphne::open_decoder(path, &e);
    if (!d) return 0;
    // open_decoder rejects a file with no rate or no channels: the player divides by the rate
    if (d->info().rate <= 0 || d->info().channels < 1) std::abort();
    // the buffer holds what the decoder says it gives (a damaged header's channel count is the decoder's to check)
    const int ch = d->info().channels;
    if (ch > 1024) return 0;
    std::vector<double> buf(static_cast<size_t>(4096) * static_cast<size_t>(ch));
    for (int i = 0; i < 256; ++i)
        if (d->read(buf.data(), 4096) <= 0) break;
    if (d->info().frames > 0) {
        d->seek(d->info().frames / 2);
        d->read(buf.data(), 4096);
    }
    return 0;
}
