#!/usr/bin/env bash
# The Steam Frame's build: Linux arm64 with OpenXR presentation on by default,
# built in Valve's Steam Linux Runtime 3 (sniper) arm64 SDK so it runs on the
# headset's SteamOS, in the runtime or outside it. Produces
# dist/Xelee-SteamFrame-arm64.tar.gz (see docs/steam-frame.md).
#
# On an arm64 host already inside the SDK (CI's ubuntu-24.04-arm job) it
# builds directly. Anywhere else it reruns itself in the SDK's container
# (docker, or podman via CONTAINER=podman); on x86_64 that needs arm64
# emulation registered with binfmt_misc (QEMU), and a first build takes a few
# hours, mostly Dawn. Later builds reuse build/frame-ccache.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# The SDK pinned by digest, so a build can be reproduced.
SDK_IMAGE="${STEAM_FRAME_SDK_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk/arm64@sha256:966ad3ef70751b98253f031d22c5b75798ceb9c49612b0a3489fc5f8f58858fc}"

if [[ "$(uname -m)" != aarch64 ]] || ! grep -qs "^ID=steamrt" /etc/os-release; then
    if [[ -n "${STEAM_FRAME_IN_CONTAINER:-}" ]]; then
        echo "error: not in the arm64 sniper SDK" >&2
        exit 1
    fi
    CONTAINER="${CONTAINER:-docker}"
    echo "=== Rerunning in ${SDK_IMAGE} (${CONTAINER}) ==="
    exec "${CONTAINER}" run --rm --platform linux/arm64 \
        -u "$(id -u):$(id -g)" -e HOME=/src/build/frame-home -e STEAM_FRAME_IN_CONTAINER=1 \
        -e FRAME_JOBS -e FRAME_STEAM_AUDIO -e MELEE_VERSION -e BUILD_DIR \
        -v "${ROOT_DIR}:/src" -w /src "${SDK_IMAGE}" tools/package_steam_frame.sh "$@"
fi

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/steam-frame}"
DIST_DIR="${ROOT_DIR}/dist"
STAGE="${BUILD_DIR}/xelee"
mkdir -p "${HOME:-/tmp}" "${DIST_DIR}"
export CCACHE_DIR="${CCACHE_DIR:-${ROOT_DIR}/build/frame-ccache}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-6G}"

# The same melee-xr Dawn as the Quest build (one pin: tools/build_android.sh).
MELEE_DAWN_REPO="${MELEE_DAWN_REPO:-https://github.com/rhythmerc/dawn.git}"
MELEE_DAWN_REF="${MELEE_DAWN_REF:-$(sed -n 's/.*MELEE_DAWN_REF:-\([0-9a-f]*\)}.*/\1/p' "${SCRIPT_DIR}/build_android.sh")}"
MELEE_DAWN_SOURCE="${MELEE_DAWN_SOURCE:-${ROOT_DIR}/build/dawn-src}"
if [[ ! -d "${MELEE_DAWN_SOURCE}/.git" ]]; then
    echo "=== Fetching Dawn (${MELEE_DAWN_REPO} @ ${MELEE_DAWN_REF}) ==="
    git init -q "${MELEE_DAWN_SOURCE}"
    git -C "${MELEE_DAWN_SOURCE}" fetch -q --depth 1 "${MELEE_DAWN_REPO}" "${MELEE_DAWN_REF}"
    git -C "${MELEE_DAWN_SOURCE}" checkout -q FETCH_HEAD
    (cd "${MELEE_DAWN_SOURCE}" && python3 tools/fetch_dawn_dependencies.py)
fi

# Steam Audio for linux-arm64, which its release SDK lacks: built from source
# once, into build/deps. Without it the package has no positional audio.
if [[ "${FRAME_STEAM_AUDIO:-1}" != 0 ]]; then
    "${SCRIPT_DIR}/build_steam_audio.sh" ||
        echo "warning: Steam Audio didn't build; the package has no positional audio" >&2
fi

# The SDK's newest GCC (Dawn needs C++20); the decomp needs GCC anyway.
CC_BIN="${CC:-$(command -v gcc-14 || command -v gcc)}"
CXX_BIN="${CXX:-$(command -v g++-14 || command -v g++)}"

echo "=== Building Xelee for the Steam Frame (Linux arm64, ${CC_BIN}) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="${CC_BIN}" -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DMELEE_STEAM_FRAME=ON \
    -DAURORA_ENABLE_OPENXR=ON -DAURORA_XR_DEFAULT_ON=ON -DAURORA_OPENXR_PROVIDER=vendor \
    -DAURORA_DAWN_PROVIDER=vendor "-DFETCHCONTENT_SOURCE_DIR_DAWN=${MELEE_DAWN_SOURCE}" \
    -DDAWN_SUPPORTS_CXX_MODULES=OFF -DDAWN_BUILD_PROTOBUF=OFF -DTINT_BUILD_IR_BINARY=OFF \
    -DAURORA_SDL3_PROVIDER=vendor -DAURORA_NOD_PROVIDER="${AURORA_NOD_PROVIDER:-package}"
ninja -C "${BUILD_DIR}" -j "${FRAME_JOBS:-$(nproc)}" melee

echo "=== Staging ==="
rm -rf "${STAGE}"
mkdir -p "${STAGE}/lib/openxr" "${STAGE}/logs"
cp "${BUILD_DIR}/melee" "${STAGE}/melee"
# The symbols stay beside the package (CI keeps them as an artifact) for
# reading the headset's coredumps.
objcopy --only-keep-debug "${STAGE}/melee" "${BUILD_DIR}/melee.debug"
strip --strip-debug "${STAGE}/melee"
objcopy --add-gnu-debuglink="${BUILD_DIR}/melee.debug" "${STAGE}/melee"
# Khronos' OpenXR loader, for when the runtime doesn't provide one (xelee.sh).
OPENXR_LOADER="$(find "${BUILD_DIR}/_deps" -name 'libopenxr_loader.so.1*' -type f | head -n1)"
if [[ -n "${OPENXR_LOADER}" ]]; then
    cp "${OPENXR_LOADER}" "${STAGE}/lib/openxr/libopenxr_loader.so.1"
fi
# Steam Audio, when the build has positional audio (cmake/SteamAudio.cmake).
if [[ -f "${BUILD_DIR}/libphonon.so" ]]; then
    cp "${BUILD_DIR}/libphonon.so" "${STAGE}/lib/"
fi
cp -r "${ROOT_DIR}/resources" "${STAGE}/resources"
# No initial_pipeline_cache.db: the desktop seed's pipelines are a desktop
# GPU's, and the headset records its own.
cp "${ROOT_DIR}/platforms/steam-frame/xelee.sh" "${STAGE}/xelee.sh"
cp "${ROOT_DIR}/platforms/steam-frame/melee-env.example.txt" "${STAGE}/melee-env.example.txt"
cp "${ROOT_DIR}/platforms/steam-frame/README.txt" "${STAGE}/README.txt"
cp "${ROOT_DIR}/tools/frame_shortcut.py" "${STAGE}/add-to-steam.py"
cp "${ROOT_DIR}/platforms/linux/melee.png" "${STAGE}/xelee.png"
chmod +x "${STAGE}/xelee.sh" "${STAGE}/melee" "${STAGE}/add-to-steam.py"

# The libraries it needs from the SDK go beside it: the headset's SteamOS
# lacks some (OpenSSL 1.1, curl's GnuTLS build) and has newer sonames of
# others, and the arm64 runtime it has installed isn't this one. glibc, zlib
# and the Vulkan loader (the headset's GPU driver) stay the system's.
LD_LIBRARY_PATH="${STAGE}/lib:${STAGE}/lib/openxr" ldd "${STAGE}/melee" | awk '$3 ~ /^\// {print $1, $3}' |
    while read -r name path; do
        case "${name}" in
        ld-linux* | libc.so.* | libdl.so.* | libm.so.* | libpthread.so.* | librt.so.* | libvulkan.so.* | libz.so.*) ;;
        *) [[ -e "${STAGE}/lib/${name}" || -e "${STAGE}/lib/openxr/${name}" ]] || cp -L "${path}" "${STAGE}/lib/${name}" ;;
        esac
    done
echo "Bundled: $(ls "${STAGE}/lib" | grep -v '^openxr$' | tr '\n' ' ')"

echo "=== Checking what it needs ==="
# The Steam Linux Runtime 3 (sniper) has glibc 2.31: nothing may need newer.
shopt -s nullglob
newest="$(objdump -T "${STAGE}/melee" "${STAGE}"/lib/*.so* "${STAGE}"/lib/openxr/*.so* |
    grep -o 'GLIBC_[0-9][0-9.]*' | sort -uV | tail -n1)"
shopt -u nullglob
echo "Newest glibc symbol: ${newest}"
if [[ "$(printf '%s\n' "${newest#GLIBC_}" 2.31 | sort -V | tail -n1)" != 2.31 ]]; then
    echo "error: ${newest} is newer than the Steam Linux Runtime's glibc 2.31" >&2
    exit 1
fi
echo "Libraries it loads:"
readelf -d "${STAGE}/melee" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/  \1/p'

tar -czf "${DIST_DIR}/Xelee-SteamFrame-arm64.tar.gz" -C "${BUILD_DIR}" xelee
echo "=== Packaging complete ==="
ls -lh "${DIST_DIR}/Xelee-SteamFrame-arm64.tar.gz"
