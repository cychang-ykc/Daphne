add_library(catch2_main STATIC ${PROJECT_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp)
target_include_directories(catch2_main SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/catch2)
if(MSVC)
    target_compile_options(catch2_main PRIVATE /W0)
endif()

add_executable(daphne_tests
    test_capi.cpp
    test_chain.cpp
    test_decoders.cpp
    test_dsd_file.cpp
    test_dsd_output.cpp
    test_downmix.cpp
    test_eq.cpp
    test_file_bytes.cpp
    test_filter_design.cpp
    test_fma.cpp
    test_gain.cpp
    test_live.cpp
    test_modulator.cpp
    test_output_format.cpp
    test_player.cpp
    test_realtime.cpp
    test_resampler.cpp
)
target_link_libraries(daphne_tests PRIVATE daphne_engine daphne_core catch2_main)
# the Core Audio output on a real Mac (hidden tests: daphne_tests "[.coreaudio]")
if(APPLE)
    target_sources(daphne_tests PRIVATE test_coreaudio.cpp)
    target_include_directories(daphne_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
endif()
add_test(NAME daphne_tests COMMAND daphne_tests)
# a test that hangs (a file-parsing loop, a deadlock) fails instead of holding CI until the job's own limit
set_tests_properties(daphne_tests PROPERTIES TIMEOUT 1200)
# Catch2 defines wmain when UNICODE is set (it is, project-wide).
if(MSVC)
    target_link_options(daphne_tests PRIVATE /ENTRY:wmainCRTStartup)
endif()
foreach(dll ${DAPHNE_FFMPEG_DLLS})
    add_custom_command(TARGET daphne_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:daphne_tests>)
endforeach()
daphne_warnings(daphne_tests)
