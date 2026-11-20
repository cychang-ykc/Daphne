# libFuzzer targets (DAPHNE_BUILD_FUZZ, Clang): the file parsers on arbitrary bytes. Each input is written to a
# temporary file (the parsers read files), opened and read; a crash, a sanitizer finding or a hang is a failure.
#   fuzz_dsd       DsdFile: DSF and DFF headers, chunks and blocks
#   fuzz_decoder   open_decoder: WAV / AIFF (dr_wav), FLAC (dr_flac), MP3 (minimp3); the first byte picks which
#   fuzz_settings  the settings parser (JSON): in range and the same when written back (doc/N28)
# Seeds: corpus/<target>/ (fuzz/make_seeds.py). CI runs each for 60 s.
foreach(t dsd decoder settings)
    add_executable(fuzz_${t} fuzz_${t}.cpp fuzz_util.h)
    target_link_libraries(fuzz_${t} PRIVATE daphne_engine)
    target_compile_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${t} PRIVATE -fsanitize=fuzzer)
    daphne_warnings(fuzz_${t})
endforeach()
