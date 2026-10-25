# Builds daphne_core.dll for MIKU's Daphne extension (src/Daphne.Extension, MikuDeploy) and copies it with the
# FFmpeg DLLs and the licenses (Daphne, nlohmann/json, FFmpeg and where its source is) to -Out. Fetches FFmpeg and the
# ASIO SDK on the first build, and FFmpeg again when fetch_ffmpeg.ps1 names another package.
# The build tools are taken from this machine first and downloaded into -Tools only when missing:
#   MSVC   this shell's x64 developer environment, an installed Visual Studio with the C++ tools, else fetch_msvc.ps1
#   CMake  PATH (3.25+), the one in that Visual Studio, else fetch_cmake.ps1
#   Ninja  PATH, the one in that Visual Studio, else fetch_ninja.ps1
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Config = "Release",
    # where downloaded build tools go (MIKU passes its .tools folder); this repository's .tools otherwise
    [string]$Tools = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Tools) { $Tools = Join-Path $root ".tools" }
# FFmpeg: fetched when missing, or when fetch_ffmpeg.ps1 names another package than the one there (its SOURCE.txt):
# the DLLs copied below and FFmpeg-SOURCE.txt must name the same build
$ffDir = Join-Path $root "third_party\ffmpeg"
$ffWant = [regex]::Match((Get-Content -Raw (Join-Path $PSScriptRoot "fetch_ffmpeg.ps1")), '\[string\]\$Url = "([^"]+)"').Groups[1].Value
$ffHave = if (Test-Path (Join-Path $ffDir "SOURCE.txt")) { (Get-Content (Join-Path $ffDir "SOURCE.txt") | Select-Object -First 1).Trim() } else { "" }
# (by the package's file name: the same package has been downloaded from BtbN and from this repository's release)
if (-not (Test-Path (Join-Path $ffDir "include\libavcodec\avcodec.h")) -or
    ($ffWant -and [IO.Path]::GetFileName($ffHave) -ne [IO.Path]::GetFileName($ffWant))) { & (Join-Path $PSScriptRoot "fetch_ffmpeg.ps1") }
if (-not (Test-Path (Join-Path $root "third_party\asiosdk\common\iasiodrv.h"))) { & (Join-Path $PSScriptRoot "fetch_asiosdk.ps1") }

# ── MSVC ──
$vs = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and $env:INCLUDE -and $env:LIB -and
    ($env:VSCMD_ARG_TGT_ARCH -eq "x64" -or $env:Platform -eq "x64")) {
    Write-Host "daphne: MSVC from this shell's developer environment"
} else {
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
            Select-Object -First 1
    }
    if ($vs) {
        Write-Host "daphne: MSVC from Visual Studio $vs"
        # vcvars64.bat sets PATH, INCLUDE, LIB, ... in a cmd: take them over into this process
        $vars = cmd /c "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set"
        if ($LASTEXITCODE -ne 0) { throw "daphne: vcvars64.bat failed ($vs)" }
        foreach ($line in $vars) { if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
    } else {
        $msvc = Join-Path $Tools "msvc"
        if (-not (Test-Path (Join-Path $msvc "SOURCE.txt"))) {
            Write-Host "daphne: no Visual Studio with the C++ tools on this machine, downloading a portable MSVC to $msvc"
            & (Join-Path $PSScriptRoot "fetch_msvc.ps1") -Dest $msvc
        }
        Write-Host "daphne: MSVC from $msvc"
        $vc = (Get-ChildItem (Join-Path $msvc "VC\Tools\MSVC") -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1).FullName
        $kits = Join-Path $msvc "Windows Kits\10"
        $sdkv = (Get-ChildItem (Join-Path $kits "Include") -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1).Name
        $inc = Join-Path $kits "Include\$sdkv"
        $lib = Join-Path $kits "Lib\$sdkv"
        $env:PATH = "$vc\bin\Hostx64\x64;$kits\bin\$sdkv\x64;$env:PATH"
        $env:INCLUDE = "$vc\include;$inc\ucrt;$inc\shared;$inc\um;$inc\winrt"
        $env:LIB = "$vc\lib\x64;$lib\ucrt\x64;$lib\um\x64"
    }
}
$cl = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
if (-not $cl) { throw "daphne: MSVC (cl.exe) not found" }

# ── CMake and Ninja ──
function Test-CMake($exe) {
    if (-not $exe -or -not (Test-Path $exe)) { return $false }
    $v = & $exe --version | Select-Object -First 1
    return ($v -match '(\d+\.\d+\.\d+)' -and [version]$Matches[1] -ge [version]"3.25.0")
}
$vsCMake = if ($vs) { Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake" }
$cmake = @((Get-Command cmake.exe -ErrorAction SilentlyContinue).Source,
           $(if ($vsCMake) { Join-Path $vsCMake "CMake\bin\cmake.exe" }),
           (Join-Path $Tools "cmake\bin\cmake.exe")) | Where-Object { Test-CMake $_ } | Select-Object -First 1
if (-not $cmake) {
    Write-Host "daphne: no CMake 3.25+ on this machine, downloading it to $(Join-Path $Tools 'cmake')"
    & (Join-Path $PSScriptRoot "fetch_cmake.ps1") -Dest (Join-Path $Tools "cmake")
    $cmake = Join-Path $Tools "cmake\bin\cmake.exe"
}
$ninja = @((Get-Command ninja.exe -ErrorAction SilentlyContinue).Source,
           $(if ($vsCMake) { Join-Path $vsCMake "Ninja\ninja.exe" }),
           (Join-Path $Tools "ninja\ninja.exe")) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $ninja) {
    & (Join-Path $PSScriptRoot "fetch_ninja.ps1") -Dest (Join-Path $Tools "ninja")
    $ninja = Join-Path $Tools "ninja\ninja.exe"
}
Write-Host "daphne: compiler $cl"
Write-Host "daphne: $cmake, $ninja"

# ── build ──
# Ninja for every MSVC above (a portable MSVC has no Visual Studio for CMake's Visual Studio generator)
$bd = Join-Path $root "build-miku"
# a build folder made by another generator (the Visual Studio one, before) or with another compiler starts over
$cache = Join-Path $bd "CMakeCache.txt"
if (Test-Path $cache) {
    $c = Get-Content $cache
    if (-not ($c -contains "CMAKE_GENERATOR:INTERNAL=Ninja") -or -not ($c -contains "CMAKE_CXX_COMPILER:FILEPATH=$($cl -replace '\\', '/')")) {
        Remove-Item -Recurse -Force $bd
    }
}
& $cmake -S $root -B $bd -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$cl" "-DCMAKE_BUILD_TYPE=$Config" -DDAPHNE_BUILD_TESTS=OFF | Out-Host
if ($LASTEXITCODE -ne 0) { throw "daphne: CMake configure failed" }
& $cmake --build $bd --target daphne_core | Out-Host
if ($LASTEXITCODE -ne 0) { throw "daphne: build failed" }
New-Item -ItemType Directory -Force $Out | Out-Null
$bin = Join-Path $bd "bin"
Copy-Item (Join-Path $bin "daphne_core.dll") $Out -Force
Get-ChildItem $bin -Filter "*.dll" | Where-Object { $_.Name -match '^(avformat|avcodec|avutil|swresample)-' } | Copy-Item -Destination $Out -Force
# the licenses, as miku/pack-mac.sh puts them in the Mac module: Daphne's (GPL), the JSON library's (MIT, compiled into
# daphne_core.dll), FFmpeg's (LGPL) and where its source is
Copy-Item (Join-Path $root "LICENSE") (Join-Path $Out "LICENSE") -Force
Copy-Item (Join-Path $root "third_party\nlohmann\LICENSE.MIT") (Join-Path $Out "nlohmann-json-LICENSE.MIT") -Force
Copy-Item (Join-Path $ffDir "LICENSE.txt") (Join-Path $Out "FFmpeg-LICENSE.txt") -Force
$ffPkg = (Get-Content (Join-Path $ffDir "SOURCE.txt") | Select-Object -First 1).Trim()
$ffCommit = [regex]::Match($ffPkg, '-g([0-9a-f]{7,40})-').Groups[1].Value
$ffSrc = if ($ffCommit) { "FFmpeg source: commit $ffCommit, https://github.com/FFmpeg/FFmpeg/commit/$ffCommit" } else { "FFmpeg source: https://ffmpeg.org/download.html" }
Set-Content -Path (Join-Path $Out "FFmpeg-SOURCE.txt") -Encoding utf8 -Value @(
    "FFmpeg (LGPL, the DLLs avcodec / avformat / avutil / swresample next to this file), unmodified build: $ffPkg",
    "It is BtbN's FFmpeg-Builds package (https://github.com/BtbN/FFmpeg-Builds, its build scripts), kept as a release of",
    "https://github.com/cychang-ykc/Daphne (scripts/fetch_ffmpeg.ps1).",
    $ffSrc)
Write-Host "daphne_core.dll deployed to $Out"
