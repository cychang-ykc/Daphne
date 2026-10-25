#!/bin/bash
# Packs Daphne for MIKU's Mac installer: <out>/<arm64|x64>/daphne/ holds what MIKU's extension loader reads from
# MIKU.app/Contents/Resources/ext/<module>/ (mac/app/main/extensions.js):
#   extension.js and the rest of miku/*.js     the shell (MIKU's AudioEngine backend, settings page parts)
#   web/                                       the page parts, shared with the Windows extension (miku/web is a
#                                              symlink in git, copied here as files)
#   bin/daphne-helper, bin/libdaphne_core.dylib, bin/libav*.dylib, bin/libswresample.*.dylib
#                                              the engine (the same core as daphne_core.dll on Windows) and FFmpeg;
#                                              only code in bin/ (MIKU's installer signs every file in it)
#   LICENSE, nlohmann-json-LICENSE.MIT, FFmpeg-COPYING.LGPLv2.1, FFmpeg-SOURCE.txt
#                                              Daphne's license, the JSON library's (MIT, compiled into the core and
#                                              the helper), FFmpeg's license and where its source is
# MIKU's mac/build/build.sh runs Extensions/<Name>/miku/pack-mac.sh for every module that has one; the same
# Extensions/Daphne checkout gives MIKU on Windows its extension (src/Daphne.Extension, scripts/build_for_miku.ps1).
#
#   miku/pack-mac.sh <out-dir> [arm64] [x86_64]     (both architectures by default)
#
# Needs macOS 11 or later with the Xcode command line tools, nothing else: CMake is the one on the PATH (or
# CMAKE=/path/to/cmake), else Kitware's portable build downloaded into .tools/ (as scripts/fetch_cmake.ps1 on Windows).
# FFmpeg (the version the Windows build uses, LGPL, shared, decoders only) is built from source the first time and then
# reused: third_party/ffmpeg-src/ (the source), build-mac-<arch>/ (FFmpeg and the engine).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: miku/pack-mac.sh <out-dir> [arm64] [x86_64]}"; shift
ARCHS=("$@"); [ ${#ARCHS[@]} -gt 0 ] || ARCHS=(arm64 x86_64)
MIN=11.0                        # the oldest macOS the module runs on (MIKU's Electron needs newer anyway)
FF_VER=8.1.3                    # as scripts/fetch_ffmpeg.ps1 (Windows)
FF_SHA=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3   # of ffmpeg-8.1.3.tar.xz from ffmpeg.org
CM_VER=3.31.6                   # as scripts/fetch_cmake.ps1 (Windows)
CM_SHA=330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d   # of cmake-3.31.6-macos-universal.tar.gz
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
fail() { echo "daphne: $1" >&2; exit 1; }

CMAKE="${CMAKE:-$(command -v cmake || true)}"
if [ -z "$CMAKE" ]; then
  CM="$ROOT/.tools/cmake-$CM_VER-macos-universal"
  CMAKE="$CM/CMake.app/Contents/bin/cmake"
  if [ ! -x "$CMAKE" ]; then
    echo "daphne: CMake is not installed; downloading CMake $CM_VER into .tools/"
    mkdir -p "$ROOT/.tools"
    CT="$ROOT/.tools/cmake-$CM_VER-macos-universal.tar.gz"
    curl -fL -o "$CT" "https://github.com/Kitware/CMake/releases/download/v$CM_VER/cmake-$CM_VER-macos-universal.tar.gz" ||
      fail "CMake could not be downloaded"
    echo "$CM_SHA  $CT" | shasum -a 256 -c - >/dev/null || { rm -f "$CT"; fail "cmake-$CM_VER-macos-universal.tar.gz does not match its SHA-256"; }
    rm -rf "$CM"; tar xf "$CT" -C "$ROOT/.tools"; rm -f "$CT"
    xattr -dr com.apple.quarantine "$CM" 2>/dev/null || true
    [ -x "$CMAKE" ] || fail "CMake $CM_VER did not unpack to $CM"
  fi
fi

# FFmpeg's source, checked against the release this script was written for
SRC="$ROOT/third_party/ffmpeg-src"
TAR="$SRC/ffmpeg-$FF_VER.tar.xz"
if [ ! -f "$SRC/ffmpeg-$FF_VER/configure" ]; then
  mkdir -p "$SRC"
  [ -f "$TAR" ] || { echo "daphne: downloading FFmpeg $FF_VER"; curl -fL -o "$TAR" "https://ffmpeg.org/releases/ffmpeg-$FF_VER.tar.xz"; }
  echo "$FF_SHA  $TAR" | shasum -a 256 -c - >/dev/null || { rm -f "$TAR"; fail "ffmpeg-$FF_VER.tar.xz does not match its SHA-256"; }
  tar xf "$TAR" -C "$SRC"
fi

for arch in "${ARCHS[@]}"; do
  case "$arch" in
    arm64) tag=arm64 ;;
    x86_64|x64) arch=x86_64; tag=x64 ;;   # MIKU's installer names the Intel build x64
    *) fail "unknown architecture $arch (arm64 or x86_64)" ;;
  esac
  B="$ROOT/build-mac-$arch"
  FF="$B/ffmpeg"
  mkdir -p "$B"

  # 1. FFmpeg: shared libraries found through the helper's rpath, only the demuxers and decoders the engine uses
  #    for the formats without a dedicated decoder (M4A / ALAC / AAC, Ogg, Opus, APE, WavPack, WMA, TTA, …)
  FF_OPTS=(--prefix="$FF" --install-name-dir=@rpath
    --arch="$arch" --target-os=darwin --enable-cross-compile --cc="clang -arch $arch"
    --extra-cflags="-mmacosx-version-min=$MIN" --extra-ldflags="-mmacosx-version-min=$MIN"
    --enable-shared --disable-static --disable-programs --disable-doc --disable-avdevice --disable-swscale
    --disable-avfilter --disable-network --disable-x86asm --disable-autodetect --disable-everything
    --enable-protocol=file
    --enable-demuxer=mov,caf,ogg,matroska,flac,wav,w64,aiff,mp3,aac,ape,wv,asf,tta
    --enable-decoder=aac,aac_latm,alac,vorbis,opus,flac,mp3,mp3float,mp2,ape,wavpack,wmav1,wmav2,wmapro,wmalossless,tta,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,pcm_s16be,pcm_s24be,pcm_s32be,pcm_f32be
    --enable-parser=aac,flac,opus,vorbis,mpegaudio)
  # what the build in $FF was made from: another FF_VER or other options build it again (bin/ and FFmpeg-SOURCE.txt
  # must name the same release)
  STAMP="$FF_VER ${FF_OPTS[*]}"
  if [ ! -f "$FF/lib/libavcodec.dylib" ] || [ "$(cat "$FF/daphne-ffmpeg.stamp" 2>/dev/null)" != "$STAMP" ]; then
    echo "daphne: building FFmpeg $FF_VER for $arch (a few minutes, once)"
    rm -rf "$B/ffmpeg-build" "$FF"; mkdir -p "$B/ffmpeg-build"
    ( cd "$B/ffmpeg-build" && "$SRC/ffmpeg-$FF_VER/configure" "${FF_OPTS[@]}" >"$B/ffmpeg-configure.log" 2>&1 &&
      make -j"$JOBS" >"$B/ffmpeg-make.log" 2>&1 && make install >"$B/ffmpeg-install.log" 2>&1 ) ||
      fail "FFmpeg ($arch) did not build; see $B/ffmpeg-*.log"
    printf '%s\n' "$STAMP" >"$FF/daphne-ffmpeg.stamp"
  fi

  # 2. the engine: libdaphne_core.dylib and the helper that runs it for MIKU
  echo "daphne: building the engine for $arch"
  "$CMAKE" -S "$ROOT" -B "$B/daphne" -DCMAKE_BUILD_TYPE=Release -DDAPHNE_BUILD_TESTS=OFF \
    -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DDAPHNE_FFMPEG_DIR="$FF" >"$B/cmake.log" 2>&1 ||
    fail "CMake ($arch) failed; see $B/cmake.log"
  grep -q "FFmpeg found in" "$B/cmake.log" || fail "the engine ($arch) did not find FFmpeg in $FF"
  "$CMAKE" --build "$B/daphne" --target daphne-helper -j "$JOBS" >"$B/build.log" 2>&1 ||
    { tail -30 "$B/build.log" >&2; fail "the engine ($arch) did not build; see $B/build.log"; }

  # 3. the module
  D="$OUT/$tag/daphne"
  rm -rf "$D"; mkdir -p "$D/bin"
  cp "$ROOT"/miku/*.js "$D/"
  cp -RL "$ROOT/miku/web" "$D/web"
  cp "$ROOT/LICENSE" "$D/LICENSE"
  cp "$ROOT/third_party/nlohmann/LICENSE.MIT" "$D/nlohmann-json-LICENSE.MIT"   # compiled into the core and the helper
  cp "$B/daphne/bin/daphne-helper" "$B/daphne/bin/libdaphne_core.dylib" "$D/bin/"
  # FFmpeg's libraries under the names the core was linked against (@rpath/libavcodec.62.dylib, …)
  for name in $(otool -L "$D/bin/libdaphne_core.dylib" | awk '/@rpath\/lib(av|sw)/ { sub("@rpath/", "", $1); print $1 }'); do
    cp -L "$FF/lib/$name" "$D/bin/$name"
    for dep in $(otool -L "$FF/lib/$name" | awk '/@rpath\/lib(av|sw)/ { sub("@rpath/", "", $1); print $1 }'); do
      [ -f "$D/bin/$dep" ] || cp -L "$FF/lib/$dep" "$D/bin/$dep"
    done
  done
  cp "$SRC/ffmpeg-$FF_VER/COPYING.LGPLv2.1" "$D/FFmpeg-COPYING.LGPLv2.1"
  printf 'FFmpeg %s (LGPL 2.1, shared libraries in bin/), unmodified source: https://ffmpeg.org/releases/ffmpeg-%s.tar.xz\nBuilt by miku/pack-mac.sh (https://github.com/cychang-ykc/Daphne), which has the configure options.\n' \
    "$FF_VER" "$FF_VER" >"$D/FFmpeg-SOURCE.txt"
  # only relative search paths (@loader_path: bin/) stay in what ships; none of this machine's folders
  for f in "$D"/bin/daphne-helper "$D"/bin/*.dylib; do
    # line by line: a path with spaces (the checkout's) is one rpath, not several words
    while IFS= read -r rp; do
      case "$rp" in @*) ;; *) install_name_tool -delete_rpath "$rp" "$f" ;; esac
    done < <(otool -l "$f" | awk '/cmd LC_RPATH/ { getline; getline; sub(/^ *path /, ""); sub(/ \(offset [0-9]+\)$/, ""); print }')
    if otool -L "$f" | tail -n +2 | awk '{ print $1 }' | grep -v -e '^@rpath/' -e '^/usr/lib/' -e '^/System/' >/dev/null; then
      fail "$(basename "$f") links to a library outside bin/ and the system: $(otool -L "$f" | tail -n +2 | awk '{ print $1 }' | grep -v -e '^@rpath/' -e '^/usr/lib/' -e '^/System/' | tr '\n' ' ')"
    fi
    # ad hoc (install_name_tool invalidates it; MIKU's installer signs again)
    codesign -f -s - "$f" 2>"$B/codesign.log" || { cat "$B/codesign.log" >&2; fail "codesign failed on $(basename "$f")"; }
  done
  # the helper starts and says hello (on this machine's architecture)
  if [ "$(uname -m)" = "$arch" ]; then
    # not under pipefail: a helper that dies here comes to the message below with what it wrote, not ends the script
    hello=$("$D/bin/daphne-helper" </dev/null 2>"$B/helper-check.log" | head -1) || true
    case "$hello" in
      *'"hello"'*) echo "daphne: $tag: $hello" ;;
      *) cat "$B/helper-check.log" >&2; fail "the packed helper ($arch) did not start: $hello" ;;
    esac
  fi
  echo "daphne: packed $D"
done
