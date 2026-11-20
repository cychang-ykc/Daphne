cmake_minimum_required(VERSION 3.25)
project(daphne VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# the static libraries also go into the shared daphne_core (needed on Linux / macOS)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# shared library exports: only the dc_* functions (DC_API)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# no contraction of a * b + c into an FMA by the compiler (Clang does it by default, on ARM64 for every
# such expression): the double-double arithmetic and the FMA emulation (dsp fma.h) need each operation
# rounded as written; MSVC's default /fp:precise does not contract
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

option(DAPHNE_BUILD_TESTS "Build unit tests" ON)
# Tube Color (doc/N28): the stage and its settings. Off with color.enabled false (the default), the chain is the same
# as without it. DAPHNE_COLOR_TEST_CURVE: the M1 stage with its polynomial test curve, for test builds only and never
# for a release: without it (every release build) Tube Color is unavailable until M2 brings the real curves.
option(DAPHNE_COLOR "Build Tube Color (N28)" ON)
option(DAPHNE_COLOR_TEST_CURVE "Tube Color with the M1 test curve: test builds only, never a release (N28)" OFF)
if(DAPHNE_COLOR_TEST_CURVE AND NOT DAPHNE_COLOR)
    message(FATAL_ERROR "DAPHNE_COLOR_TEST_CURVE needs DAPHNE_COLOR")
endif()
option(DAPHNE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (CI)" OFF)
# Sanitizers for every target (Clang / GCC; CI): e.g. "address,undefined" or "thread". A finding stops the
# program (no recovery), so a test run fails on it.
set(DAPHNE_SANITIZE "" CACHE STRING "Sanitizers for every target, e.g. address,undefined or thread")
if(DAPHNE_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=${DAPHNE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${DAPHNE_SANITIZE})
endif()
# libFuzzer targets for the file parsers (Clang; fuzz/): every library is built with the coverage the fuzzer
# follows, the targets link libFuzzer's main.
option(DAPHNE_BUILD_FUZZ "Build the libFuzzer targets (Clang)" OFF)
if(DAPHNE_BUILD_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# Warnings for the project's own targets: /W4 (MSVC, above) or -Wall -Wextra. Third-party code stays
# quiet (catch2_main has its own options; the vendored headers are system includes).
function(daphne_warnings target)
    if(MSVC)
        if(DAPHNE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if(DAPHNE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# header-only / vendored dependencies
add_library(daphne_vendor INTERFACE)
target_include_directories(daphne_vendor SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_subdirectory(platform)
add_subdirectory(dsp)
if(DAPHNE_COLOR)
    add_subdirectory(color)
endif()
add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(capi)
add_subdirectory(helper)

if(DAPHNE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(DAPHNE_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
