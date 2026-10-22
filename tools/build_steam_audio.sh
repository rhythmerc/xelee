#!/usr/bin/env bash
# Steam Audio's libphonon.so for Linux arm64 (the Steam Frame), which its
# release SDK doesn't ship, built from that release's source: into
# MELEE_DEPS_DIR beside the release SDK's headers, where cmake/SteamAudio.cmake
# looks. Run on arm64, in the Steam Frame's SDK (tools/package_steam_frame.sh
# runs it when the library is missing). Only the core library, with the
# open-source dependencies its get_dependencies.py builds (PFFFT, MySOFA,
# FlatBuffers, zlib): no Embree, IPP or GPU paths, which the game's direct
# path doesn't use.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n 's/^set(MELEE_STEAM_AUDIO_VERSION "\(.*\)")$/\1/p' "${ROOT}/cmake/SteamAudio.cmake")"
COMMIT=0da18255cca520771f363ee01f100572b39a308e # v4.8.1; a new version needs its tag's commit here
DEPS="${MELEE_DEPS_DIR:-${ROOT}/build/deps}"
LIB="${DEPS}/steamaudio-${VERSION}/steamaudio/lib/linux-arm64"
SRC="${ROOT}/build/steam-audio-src"

if [[ "$(uname -m)" != aarch64 ]]; then
    echo "error: build Steam Audio for arm64 on arm64 (tools/package_steam_frame.sh does)" >&2
    exit 1
fi
if [[ -f "${LIB}/libphonon.so" ]]; then
    echo "Steam Audio ${VERSION} for linux-arm64 is already in ${LIB}"
    exit 0
fi
if [[ ! -d "${SRC}/.git" ]]; then
    echo "=== Fetching Steam Audio ${VERSION} (${COMMIT}) ==="
    git init -q "${SRC}"
    git -C "${SRC}" fetch -q --depth 1 https://github.com/ValveSoftware/steam-audio.git "${COMMIT}"
    git -C "${SRC}" checkout -q FETCH_HEAD
fi
cd "${SRC}/core"
# Its linux-arm64 builds cross-compile from a fixed sysroot; this is native.
cat > build/toolchain_linux_arm64.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $(command -v gcc-14 || command -v gcc))
set(CMAKE_CXX_COMPILER $(command -v g++-14 || command -v g++))
EOF
echo "=== Its dependencies ==="
python3 build/get_dependencies.py --platform linux --architecture arm64
echo "=== libphonon.so ==="
cmake -S . -B build/melee-linux-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${SRC}/core/build/toolchain_linux_arm64.cmake" \
    -DSTEAMAUDIO_BUILD_TESTS=OFF -DSTEAMAUDIO_BUILD_BENCHMARKS=OFF -DSTEAMAUDIO_BUILD_SAMPLES=OFF \
    -DSTEAMAUDIO_BUILD_ITESTS=OFF -DSTEAMAUDIO_BUILD_DOCS=OFF
ninja -C build/melee-linux-arm64 phonon
mkdir -p "${LIB}"
cp "$(find build/melee-linux-arm64 -name libphonon.so -type f | head -n1)" "${LIB}/libphonon.so"
echo "Steam Audio ${VERSION} for linux-arm64: ${LIB}/libphonon.so"
