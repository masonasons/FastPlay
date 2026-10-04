#!/usr/bin/env bash
# Build FastPlay's engine for the iPhone: everything but the windows, as one static
# library per SDK that the Xcode project links.
#
#   ios/build-core.sh                  (the simulator and the device)
#   ios/build-core.sh iphonesimulator  (one of them)
#   ios/build-core.sh iphoneos
#
# Needs what a Mac build needs (download-deps.sh), and FFmpeg for iOS in
# ffmpeg-ios/: ci/ffmpeg/build.sh ios ffmpeg-ios. The libraries come out in
# ios/build/lib/<sdk>/libFastPlayCore.a, FastPlay's own code and the libraries it
# is built with in one; FFmpeg's are linked from ffmpeg-ios/lib/<sdk>.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
config="${CONFIGURATION:-Release}"
min="${IPHONEOS_DEPLOYMENT_TARGET:-17.0}"
sdks=("${@:-iphonesimulator iphoneos}")
# shellcheck disable=SC2206
sdks=(${sdks[*]})

for sdk in "${sdks[@]}"; do
    case "$sdk" in
        iphoneos) archs="arm64" ;;
        iphonesimulator) archs="arm64;x86_64" ;;
        *) echo "usage: $0 [iphonesimulator] [iphoneos]" >&2; exit 2 ;;
    esac
    build="$root/ios/build/core-$sdk"
    cmake -S "$root" -B "$build" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES="$archs" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$min" \
        -DCMAKE_BUILD_TYPE="$config" \
        -DFETCHCONTENT_QUIET=ON
    cmake --build "$build" --parallel "$(sysctl -n hw.ncpu)"

    # One library of them all, for the app to link
    out="$root/ios/build/lib/$sdk"
    mkdir -p "$out"
    libs=()
    while IFS= read -r lib; do libs+=("$lib"); done < <(find "$build" -name '*.a' -not -path '*/CMakeFiles/*')
    libtool -static -no_warning_for_no_symbols -o "$out/libFastPlayCore.a" "${libs[@]}"
    echo "FastPlayCore for $sdk: $out/libFastPlayCore.a"
done
