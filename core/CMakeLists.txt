# daphne_engine: decoders, processing chain, device output, playback
add_library(daphne_engine STATIC
    src/text.cpp
    src/ring_feeder.cpp
    src/player.cpp
    src/player/change_plan.cpp
    src/player/changes.cpp
    src/player/common.cpp
    src/player/core.cpp
    src/player/crossfader.cpp
    src/player/dsd_stream.cpp
    src/player/gapless.cpp
    src/player/output_session.cpp
    src/player/pcm_stream.cpp
    src/player/report.cpp
    src/player/track_queue.cpp
    src/chain.cpp
    src/decoder_factory.cpp
    src/decoder_flac.cpp
    src/decoder_live.cpp
    src/decoder_wav.cpp
    src/decoder_mp3.cpp
    src/decoder_ffmpeg.cpp
    src/file_bytes.cpp
    src/dsd_file.cpp
    src/dsd_output.cpp
    src/filters.cpp
    src/sample_format.cpp
    src/settings.cpp
    src/stages.cpp
)
target_include_directories(daphne_engine PUBLIC include)
target_link_libraries(daphne_engine PUBLIC daphne_dsp daphne_platform daphne_vendor)
# the device outputs (N24, N25): WASAPI and ASIO on Windows, Core Audio on macOS; none yet on Linux
if(WIN32)
    target_sources(daphne_engine PRIVATE
        src/output_win.cpp
        src/wasapi_output.cpp
        src/asio_output.cpp
        src/devices_wasapi.cpp
        src/devices_asio.cpp)
    target_link_libraries(daphne_engine PRIVATE ole32 advapi32 avrt winmm propsys)
elseif(APPLE)
    target_sources(daphne_engine PRIVATE
        src/output_mac.cpp
        src/coreaudio_output.cpp
        src/devices_coreaudio.cpp)
    target_link_libraries(daphne_engine PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
else()
    target_sources(daphne_engine PRIVATE src/output_none.cpp)
endif()

# ASIO (optional): the Steinberg ASIO SDK (dual licence: proprietary / GPLv3) unpacked to
# third_party/asiosdk (scripts/fetch_asiosdk.ps1); it is not part of the repository.
set(DAPHNE_ASIO_DIR ${PROJECT_SOURCE_DIR}/third_party/asiosdk)
if(WIN32 AND EXISTS ${DAPHNE_ASIO_DIR}/common/iasiodrv.h)
    message(STATUS "daphne: ASIO SDK found in ${DAPHNE_ASIO_DIR}")
    target_compile_definitions(daphne_engine PRIVATE DAPHNE_HAVE_ASIO)
    target_include_directories(daphne_engine SYSTEM PRIVATE ${DAPHNE_ASIO_DIR}/common)
elseif(WIN32)
    message(STATUS "daphne: ASIO SDK not found (ASIO output disabled); run scripts/fetch_asiosdk.ps1")
endif()

# FFmpeg (optional): scripts/fetch_ffmpeg.ps1 puts the shared development package in third_party/ffmpeg (Windows);
# miku/pack-mac.sh builds one per architecture and passes its folder as DAPHNE_FFMPEG_DIR (macOS)
set(DAPHNE_FFMPEG_DIR ${PROJECT_SOURCE_DIR}/third_party/ffmpeg CACHE PATH "FFmpeg shared libraries: include/ and lib/")
if(NOT WIN32 AND EXISTS ${DAPHNE_FFMPEG_DIR}/include/libavcodec/avcodec.h)
    # macOS / Linux: an FFmpeg built into one folder (include/, lib/libav*.dylib or .so)
    message(STATUS "daphne: FFmpeg found in ${DAPHNE_FFMPEG_DIR}")
    target_compile_definitions(daphne_engine PRIVATE DAPHNE_HAVE_FFMPEG)
    target_include_directories(daphne_engine SYSTEM PRIVATE ${DAPHNE_FFMPEG_DIR}/include)
    foreach(lib avformat avcodec avutil swresample)
        target_link_libraries(daphne_engine PRIVATE ${DAPHNE_FFMPEG_DIR}/lib/lib${lib}${CMAKE_SHARED_LIBRARY_SUFFIX})
    endforeach()
elseif(EXISTS ${DAPHNE_FFMPEG_DIR}/include/libavcodec/avcodec.h)
    message(STATUS "daphne: FFmpeg found in ${DAPHNE_FFMPEG_DIR}")
    target_compile_definitions(daphne_engine PRIVATE DAPHNE_HAVE_FFMPEG)
    target_include_directories(daphne_engine SYSTEM PRIVATE ${DAPHNE_FFMPEG_DIR}/include)
    target_link_libraries(daphne_engine PRIVATE
        ${DAPHNE_FFMPEG_DIR}/lib/avformat.lib ${DAPHNE_FFMPEG_DIR}/lib/avcodec.lib
        ${DAPHNE_FFMPEG_DIR}/lib/avutil.lib ${DAPHNE_FFMPEG_DIR}/lib/swresample.lib)
    file(GLOB DAPHNE_FFMPEG_DLLS ${DAPHNE_FFMPEG_DIR}/bin/avformat-*.dll ${DAPHNE_FFMPEG_DIR}/bin/avcodec-*.dll
         ${DAPHNE_FFMPEG_DIR}/bin/avutil-*.dll ${DAPHNE_FFMPEG_DIR}/bin/swresample-*.dll)
    set(DAPHNE_FFMPEG_DLLS ${DAPHNE_FFMPEG_DLLS} PARENT_SCOPE)
elseif(NOT WIN32)
    # macOS / Linux: the system's FFmpeg development packages, when installed
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DAPHNE_FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
    endif()
    if(DAPHNE_FFMPEG_FOUND)
        message(STATUS "daphne: FFmpeg found with pkg-config")
        target_compile_definitions(daphne_engine PRIVATE DAPHNE_HAVE_FFMPEG)
        target_link_libraries(daphne_engine PRIVATE PkgConfig::DAPHNE_FFMPEG)
    else()
        message(STATUS "daphne: FFmpeg not found (formats without a dedicated decoder disabled)")
    endif()
else()
    message(STATUS "daphne: FFmpeg not found (formats without a dedicated decoder disabled); run scripts/fetch_ffmpeg.ps1")
endif()
daphne_warnings(daphne_engine)
