#!/usr/bin/env bash
# Build FastPlay's FFmpeg: audio only (the demuxers, decoders and parsers of the
# formats FastPlay plays, the network protocols for streams, and the FLAC encoder
# for recording), as static libraries, with no outside libraries but Codec 2 (for
# its decoder; see build_codec2) and Android's TLS (see below). LGPL. Our few
# changes to FFmpeg are in ci/ffmpeg/patches.
#
#   ci/ffmpeg/build.sh windows <out-dir>   (in MSYS2, with the MSVC tools on PATH)
#   ci/ffmpeg/build.sh macos <out-dir>     (a universal arm64 + x86_64 build)
#   ci/ffmpeg/build.sh ios <out-dir>       (iPhone arm64, and the simulator for both Macs)
#   ci/ffmpeg/build.sh android <out-dir>   (phones' arm64-v8a and the emulator's x86_64;
#                                           ANDROID_NDK_HOME names the NDK)
#   ci/ffmpeg/build.sh linux <out-dir>     (the host's architecture, HTTPS through OpenSSL)
#
# <out-dir> receives include/, lib/ and BUILDINFO.txt. The version to build is in
# ci/ffmpeg/VERSION (an FFmpeg tag), Codec 2's in ci/ffmpeg/CODEC2_VERSION; lib/ has
# Codec 2's library beside FFmpeg's. The iOS build's libraries are in lib/iphoneos
# and lib/iphonesimulator, as Xcode names the two; Android's in lib/<ABI>, as
# Gradle names them.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
platform="$1"
out="$(mkdir -p "$2" && cd "$2" && pwd)"
version="$(tr -d ' \r\n' < "$here/VERSION")"
codec2_version="$(tr -d ' \r\n' < "$here/CODEC2_VERSION")"
work="${FFMPEG_WORK:-$here/work}"

demuxers=(
    aac ac3 ac4 aiff amr ape asf au caf dsf dts dtshd eac3 flac hls iff loas
    matroska mlp mov mp3 mpc mpc8 mpegts ogg oma rm shorten tak truehd tta voc
    w64 wav wv xwma 'pcm_*' codec2
)
decoders=(
    aac aac_latm ac3 eac3 alac als amrnb amrwb ape atrac1 atrac3 atrac3p atrac9
    cook dca dolby_e 'dsd_*' dst flac g723_1 g729 gsm gsm_ms mace3 mace6 mlp
    truehd mp1 mp1float mp2 mp2float mp3 mp3float mp3adu mp3adufloat mp3on4
    mp3on4float mpc7 mpc8 nellymoser opus qdm2 qdmc ra_144 ra_288 ralf shorten
    sipr speex tak truespeech tta twinvq vorbis wavpack wmalossless wmapro wmav1
    wmav2 wmavoice xma1 xma2 'pcm_*' 'adpcm_*' libcodec2
)
parsers=(aac aac_latm ac3 amr cook dca dolby_e flac gsm mlp mpegaudio opus sipr tak vorbis)
protocols=(file http https httpproxy tcp tls hls crypto data)

common=(
    --disable-everything --disable-autodetect
    --disable-programs --disable-doc --disable-debug
    --disable-avdevice --disable-avfilter --disable-swscale
    --enable-static --disable-shared --enable-pic
    --enable-avformat --enable-avcodec --enable-swresample
    --enable-network
    --enable-encoder=flac --enable-muxer=flac
    --enable-libcodec2
)
for d in "${demuxers[@]}"; do common+=(--enable-demuxer="$d"); done
for d in "${decoders[@]}"; do common+=(--enable-decoder="$d"); done
for p in "${parsers[@]}"; do common+=(--enable-parser="$p"); done
for p in "${protocols[@]}"; do common+=(--enable-protocol="$p"); done

fetch() {
    mkdir -p "$work"
    if [ ! -d "$work/src" ]; then
        git clone --depth 1 --branch "$version" https://github.com/FFmpeg/FFmpeg.git "$work/src"
    fi
    # FastPlay's changes to FFmpeg (ci/ffmpeg/patches), each applied once: the
    # ones already applied are listed in .fastplay-patched, so a patch added
    # later reaches a source tree that has the earlier ones.
    applied="$work/src/.fastplay-patched"
    # (A list left empty is from before it named them, when there was one patch.)
    [ -f "$applied" ] && [ ! -s "$applied" ] && echo "schannel-tls12-windows7.patch" > "$applied"
    touch "$applied"
    {
        for p in "$here"/patches/*.patch; do
            [ -e "$p" ] || continue
            grep -qxF "$(basename "$p")" "$applied" && continue
            # The files it changes with LF line endings, as the patch has them,
            # whichever git checked them out
            for t in $(sed -n 's|^+++ b/\([^[:space:]]*\).*|\1|p' "$p"); do
                tr -d '\r' < "$work/src/$t" > "$work/src/$t.lf" && mv "$work/src/$t.lf" "$work/src/$t"
            done
            git -C "$work/src" apply "$p"
            basename "$p" >> "$applied"
        done
    }
    if [ ! -d "$work/codec2" ]; then
        git clone --depth 1 --branch "$codec2_version" https://github.com/drowe67/codec2.git "$work/codec2"
    fi
}

# A path as the native tools take it: on Windows, C:/... rather than MSYS2's /c/...
native() {
    if [ "$platform" = windows ]; then cygpath -m "$1"; else echo "$1"; fi
}

# Codec 2's codebooks are C that its generate_codebook makes from tables. Made here
# once, with the host's compiler, so that a cross build needs no host tool.
codec2_codebooks() {
    local src="$work/codec2/src" cb="$work/codec2-codebooks" gen
    rm -rf "$cb"
    mkdir -p "$cb"
    if [ "$platform" = windows ]; then
        (cd "$cb" && cl -nologo -O2 -Fegenerate_codebook.exe "$(native "$src/generate_codebook.c")" > /dev/null)
        gen="$cb/generate_codebook.exe"
    else
        cc -O2 -o "$cb/generate_codebook" "$src/generate_codebook.c" -lm
        gen="$cb/generate_codebook"
    fi
    (
        cd "$src/codebook"
        "$gen" lsp_cb lsp{1..10}.txt > "$cb/codebook.c"
        "$gen" lsp_cbd dlsp{1..10}.txt > "$cb/codebookd.c"
        "$gen" lsp_cbjmv lspjmv{1..3}.txt > "$cb/codebookjmv.c"
        "$gen" ge_cb gecb.txt > "$cb/codebookge.c"
        "$gen" newamp1vq_cb train_120_1.txt train_120_2.txt > "$cb/codebooknewamp1.c"
        "$gen" newamp1_energy_cb newamp1_energy_q.txt > "$cb/codebooknewamp1_energy.c"
        "$gen" newamp2vq_cb codes_450.txt > "$cb/codebooknewamp2.c"
        "$gen" newamp2_energy_cb newamp2_energy_q.txt > "$cb/codebooknewamp2_energy.c"
    )
}

# Codec 2's speech codec (ci/ffmpeg/codec2), for FFmpeg's libcodec2 decoder, as a
# static library in $2: the prefix FFmpeg is then built into, so that build_one
# finds it and its library goes out with FFmpeg's. The rest is CMake's settings
# for the platform.
build_codec2() {
    local name="$1" prefix="$2"
    shift 2
    rm -rf "$work/codec2-build-$name"
    cmake -S "$(native "$here/codec2")" -B "$(native "$work/codec2-build-$name")" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(native "$prefix")" \
        -DCODEC2_SRC="$(native "$work/codec2")" -DCODEC2_TAG="$codec2_version" \
        -DCODEC2_CODEBOOKS="$(native "$work/codec2-codebooks")" "$@"
    cmake --build "$(native "$work/codec2-build-$name")" --parallel "${JOBS:-4}"
    cmake --install "$(native "$work/codec2-build-$name")"
}

# Configure and build one architecture into $2, from a copy of the source, with
# the Codec 2 that build_codec2 put there.
build_one() {
    local name="$1" prefix="$2"
    shift 2
    rm -rf "$work/build-$name"
    mkdir -p "$work/build-$name"
    (
        cd "$work/build-$name"
        # Codec 2's library: MSVC's linker finds it through LIB, the others through -L
        codec2_lib=(--extra-ldflags="-L$prefix/lib")
        if [ "$platform" = windows ]; then
            export LIB="$(cygpath -w "$prefix/lib");${LIB:-}"
            codec2_lib=()
        fi
        "$work/src/configure" --prefix="$prefix" "${common[@]}" \
            --extra-cflags="-I$(native "$prefix/include")" ${codec2_lib[@]+"${codec2_lib[@]}"} \
            "$@" \
            || { tail -n 60 ffbuild/config.log; exit 1; }
        make -j"${JOBS:-4}"
        make install
    )
}

fetch
codec2_codebooks
rm -rf "$out/include" "$out/lib"

case "$platform" in
    windows)
        # Codec 2 with clang-cl, as MSVC has no variable-length arrays: LLVM's on
        # PATH, or the one Visual Studio comes with
        clang_cl="$(command -v clang-cl || true)"
        [ -n "$clang_cl" ] || clang_cl="$(cygpath -u "${VCINSTALLDIR:-}Tools/Llvm/x64/bin/clang-cl.exe")"
        [ -x "$clang_cl" ] || { echo "clang-cl not found (LLVM, or Visual Studio's C++ Clang tools)" >&2; exit 1; }
        build_codec2 x64 "$work/install-x64" -G "NMake Makefiles" -DCMAKE_C_COMPILER="$(native "$clang_cl")"
        # Static runtime (/MT), as FastPlay; Windows 7 (0x0601) is FFmpeg's own floor.
        build_one x64 "$work/install-x64" \
            --toolchain=msvc --arch=x86_64 --target-os=win64 \
            --extra-cflags="-MT -D_WIN32_WINNT=0x0601" \
            --enable-schannel
        cp -r "$work/install-x64/include" "$out/include"
        mkdir -p "$out/lib"
        # Static libraries come out as X.lib (or, from older FFmpeg, libX.a)
        for f in "$work/install-x64/lib"/*.lib "$work/install-x64/lib"/lib*.a; do
            [ -e "$f" ] || continue
            base="$(basename "$f")"
            base="${base%.lib}"
            base="${base%.a}"
            cp "$f" "$out/lib/${base#lib}.lib"
        done
        ls "$out/lib"/*.lib > /dev/null
        ;;
    macos)
        min="-mmacosx-version-min=11.0"
        for arch in arm64 x86_64; do
            extra=()
            # Intel slice without the x86 assembly (it needs nasm; audio decoding is cheap)
            [ "$arch" = x86_64 ] && extra+=(--disable-x86asm)
            build_codec2 "$arch" "$work/install-$arch" \
                -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
            build_one "$arch" "$work/install-$arch" \
                --enable-cross-compile --target-os=darwin --arch="$arch" \
                --cc="clang -arch $arch" \
                --extra-cflags="$min" --extra-ldflags="$min -arch $arch" \
                --enable-securetransport --enable-zlib \
                ${extra[@]+"${extra[@]}"}
        done
        cp -r "$work/install-arm64/include" "$out/include"
        mkdir -p "$out/lib"
        for f in "$work/install-arm64/lib"/*.a; do
            base="$(basename "$f")"
            lipo -create "$f" "$work/install-x86_64/lib/$base" -output "$out/lib/$base"
        done
        ;;
    ios)
        # The iPhone, and the simulator on Apple silicon and Intel Macs. FTP as
        # well, for playing from a server (the desktop builds have no use for it).
        ios_min=16.0  # as low as anything built on it goes (FastPlay Engine: 16)
        ios_slice() {
            local name="$1" sdk="$2" arch="$3" minflag="$4"
            shift 4
            build_codec2 "$name" "$work/install-$name" \
                -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" \
                -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$ios_min"
            build_one "$name" "$work/install-$name" \
                --enable-cross-compile --target-os=darwin --arch="$arch" \
                --cc="xcrun -sdk $sdk clang -arch $arch" \
                --sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)" \
                --extra-cflags="$minflag" --extra-ldflags="$minflag -arch $arch" \
                --enable-securetransport --enable-zlib --enable-protocol=ftp \
                "$@"
        }
        ios_slice ios-arm64 iphoneos arm64 "-miphoneos-version-min=$ios_min"
        ios_slice sim-arm64 iphonesimulator arm64 "-mios-simulator-version-min=$ios_min"
        ios_slice sim-x86_64 iphonesimulator x86_64 "-mios-simulator-version-min=$ios_min" --disable-x86asm
        cp -r "$work/install-ios-arm64/include" "$out/include"
        mkdir -p "$out/lib/iphoneos" "$out/lib/iphonesimulator"
        for f in "$work/install-ios-arm64/lib"/*.a; do
            base="$(basename "$f")"
            cp "$f" "$out/lib/iphoneos/$base"
            lipo -create "$work/install-sim-arm64/lib/$base" "$work/install-sim-x86_64/lib/$base" \
                -output "$out/lib/iphonesimulator/$base"
        done
        ;;
    android)
        # Android has no system TLS that C code can use, so HTTPS is mbed TLS,
        # built here with FFmpeg (Apache 2.0, which makes this FFmpeg LGPL v3: hence
        # --enable-version3). FFmpeg's mbed TLS code does DTLS too, which needs its
        # UDP protocol. FTP as well, as on the iPhone.
        ndk="${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
        api=26  # the app's minSdk
        host="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"
        bin="$ndk/toolchains/llvm/prebuilt/$host/bin"
        mbedtls_version="$(tr -d ' \r\n' < "$here/MBEDTLS_VERSION")"
        if [ ! -d "$work/mbedtls" ]; then
            git clone --depth 1 --branch "$mbedtls_version" --recurse-submodules --shallow-submodules \
                https://github.com/Mbed-TLS/mbedtls.git "$work/mbedtls"
        fi
        android_slice() {
            local abi="$1" arch="$2" triple="$3"
            shift 3
            local tls="$work/mbedtls-$abi"
            rm -rf "$work/mbedtls-build-$abi"
            cmake -S "$work/mbedtls" -B "$work/mbedtls-build-$abi" \
                -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
                -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$api" \
                -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$tls" \
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
                -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
                -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
            cmake --build "$work/mbedtls-build-$abi" --parallel "${JOBS:-4}"
            cmake --install "$work/mbedtls-build-$abi"
            build_codec2 "android-$abi" "$work/install-android-$abi" \
                -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
                -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$api"
            # (Codec 2 uses libm, which Android has apart from its C library)
            build_one "android-$abi" "$work/install-android-$abi" \
                --enable-cross-compile --target-os=android --arch="$arch" \
                --cc="$bin/$triple$api-clang" --cxx="$bin/$triple$api-clang++" \
                --ar="$bin/llvm-ar" --nm="$bin/llvm-nm" --ranlib="$bin/llvm-ranlib" \
                --strip="$bin/llvm-strip" \
                --extra-cflags="-I$tls/include" --extra-ldflags="-L$tls/lib" --extra-libs=-lm \
                --enable-version3 --enable-mbedtls --enable-zlib --enable-protocol=ftp \
                --enable-protocol=udp \
                "$@"
            mkdir -p "$out/lib/$abi"
            cp "$work/install-android-$abi/lib"/*.a "$out/lib/$abi/"
            # FFmpeg's TLS needs mbed TLS's own libraries at the app's link
            cp "$tls/lib"/libmbed*.a "$out/lib/$abi/"
        }
        android_slice arm64-v8a aarch64 aarch64-linux-android
        android_slice x86_64 x86_64 x86_64-linux-android --disable-x86asm
        cp -r "$work/install-android-arm64-v8a/include" "$out/include"
        ;;
    linux)
        # The host's architecture. HTTPS through the system's OpenSSL (Apache 2.0,
        # which makes this FFmpeg LGPL v3: hence --enable-version3), linked
        # dynamically as every Linux system has it. No x86 assembly (it needs
        # nasm; audio decoding is cheap). Codec 2 uses libm, which FFmpeg's test
        # of it must link. FFmpeg's OpenSSL code does DTLS too, which needs its UDP
        # protocol.
        build_codec2 linux "$work/install-linux" -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        build_one linux "$work/install-linux" \
            --enable-version3 --enable-openssl --enable-zlib --disable-x86asm \
            --enable-protocol=udp --extra-libs=-lm
        cp -r "$work/install-linux/include" "$out/include"
        mkdir -p "$out/lib"
        cp "$work/install-linux/lib"/*.a "$out/lib/"
        ;;
    *)
        echo "usage: $0 windows|macos|ios|android|linux <out-dir>" >&2
        exit 2
        ;;
esac

{
    echo "FFmpeg $version, audio only, built for FastPlay by ci/ffmpeg/build.sh ($platform)"
    echo "LGPL 2.1 or later; source: https://github.com/FFmpeg/FFmpeg/tree/$version"
    echo "With Codec 2 $codec2_version's speech codec (lib/*codec2*), LGPL 2.1; source: https://github.com/drowe67/codec2/tree/$codec2_version"
} > "$out/BUILDINFO.txt"
cp "$work/src/COPYING.LGPLv2.1" "$out/COPYING.LGPLv2.1"
echo "FFmpeg $version built into $out"
