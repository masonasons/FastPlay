#!/bin/bash
# Download FastPlay's dependencies for macOS and Linux: SQLite, the tempo libraries
# and FDK AAC (into deps/), and FastPlay's FFmpeg (into ffmpeg/: CI's on the Mac,
# built here on Linux). download-deps.bat is the Windows equivalent.
set -e

cd "$(dirname "$0")"

echo "============================================"
if [ "$(uname -s)" = Darwin ]; then os=macos; else os=linux; fi
echo "FastPlay Dependency Downloader ($os)"
echo "============================================"
echo

for tool in curl unzip git; do
    if ! command -v "$tool" > /dev/null; then
        echo "Error: $tool is not installed."
        exit 1
    fi
done

mkdir -p deps
rm -rf temp_dl
mkdir -p temp_dl

echo "Downloading SQLite..."
curl -sfL https://sqlite.org/2026/sqlite-amalgamation-3510200.zip -o temp_dl/sqlite.zip
unzip -qo temp_dl/sqlite.zip -d temp_dl/sqlite
cp temp_dl/sqlite/sqlite-amalgamation-3510200/sqlite3.c src/

echo
echo "Downloading tempo libraries..."
clone() {
    rm -rf "$2"
    git clone --quiet --depth 1 "$1" "$2"
}
clone https://github.com/google/speedy.git deps/speedy
clone https://github.com/Signalsmith-Audio/signalsmith-stretch.git deps/signalsmith-stretch
clone https://github.com/Signalsmith-Audio/linear.git deps/signalsmith-stretch/signalsmith-linear
clone https://github.com/waywardgeek/sonic.git deps/sonic
clone https://github.com/mborgerding/kissfft.git deps/kissfft

echo
echo "Downloading FDK AAC (for xHE-AAC)..."
clone https://github.com/mstorsjo/fdk-aac.git deps/fdk-aac

# FFmpeg: FastPlay's audio-only build (ci/ffmpeg). CI builds it in the job; a local
# build takes CI's latest with the GitHub CLI, or builds it: ci/ffmpeg/build.sh macos ffmpeg
if [ "$os" = linux ]; then
    # No Linux FFmpeg in CI: built here (a few minutes), unless it is already
    if [ ! -f ffmpeg/lib/libavformat.a ]; then
        echo
        echo "Building FFmpeg..."
        bash ci/ffmpeg/build.sh linux ffmpeg
    fi
elif [ "${CI:-}" != "true" ]; then
    echo
    echo "Downloading FFmpeg from CI..."
    if command -v gh > /dev/null; then
        found=""
        for run in $(gh run list --repo masonasons/FastPlay --workflow build.yml --status success --limit 20 \
                         --json databaseId --jq '.[].databaseId'); do
            rm -rf temp_dl/ffmpeg
            if gh run download "$run" --repo masonasons/FastPlay --name ffmpeg-macos --dir temp_dl/ffmpeg \
                   > /dev/null 2>&1 && [ -f temp_dl/ffmpeg/lib/libavformat.a ]; then
                rm -rf ffmpeg
                mv temp_dl/ffmpeg ffmpeg
                found=$run
                break
            fi
        done
        if [ -n "$found" ]; then
            echo "FFmpeg from CI run $found."
        else
            echo "No FFmpeg build found in CI's recent runs; build it with ci/ffmpeg/build.sh macos ffmpeg"
        fi
    else
        echo "The GitHub CLI (gh) is not installed; build FFmpeg with ci/ffmpeg/build.sh macos ffmpeg"
    fi
fi

rm -rf temp_dl

echo
echo "Done. Build with:"
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
if [ "$os" = macos ]; then jobs="$(sysctl -n hw.ncpu)"; else jobs="$(nproc)"; fi
echo "  cmake --build build --parallel $jobs"
