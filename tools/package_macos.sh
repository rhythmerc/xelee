#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/macos}"
DIST_DIR="${ROOT_DIR}/dist"
APP="${BUILD_DIR}/Xelee.app"
ARCH="$(uname -m)"

# Apple's clang builds the C++ (aurora, Dawn glue, launcher); the decomp's C
# goes through Homebrew GCC via tools/gcc_launcher.py. See CMakeLists.txt.
echo "=== Building Xelee (macOS ${ARCH}) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DAURORA_SDL3_PROVIDER="${AURORA_SDL3_PROVIDER:-vendor}" \
    -DAURORA_DAWN_PROVIDER="${AURORA_DAWN_PROVIDER:-package}" \
    -DAURORA_NOD_PROVIDER="${AURORA_NOD_PROVIDER:-package}"
ninja -C "${BUILD_DIR}" melee

echo "=== Staging ${APP} ==="
rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources" "${APP}/Contents/Frameworks"
# The version the app itself reports (cmake/AppVersion.cmake: the release tag,
# else the newest tag and the commit), so Info.plist can't drift from the
# string XeleeNet matches lobbies on. Don't set MELEE_VERSION here: cmake
# reads it from the environment, and a default would become the app's version.
APP_VERSION="$(sed -n 's/^#define MELEE_APP_VERSION "\(.*\)"$/\1/p' "${BUILD_DIR}/generated/app_version.h")"
if [ -z "${APP_VERSION}" ]; then
    echo "error: no MELEE_APP_VERSION in ${BUILD_DIR}/generated/app_version.h" >&2
    exit 1
fi
sed "s/@MELEE_VERSION@/${APP_VERSION#v}/g" "${ROOT_DIR}/platforms/macos/Info.plist" \
    > "${APP}/Contents/Info.plist"
cp "${ROOT_DIR}/platforms/macos/xelee.icns" "${APP}/Contents/Resources/"
cp "${BUILD_DIR}/melee" "${APP}/Contents/MacOS/Xelee"
# RelWithDebInfo carries the DWARF in the binary on Mach-O too; ship it stripped.
strip -S "${APP}/Contents/MacOS/Xelee"
# SDL_GetBasePath() is Contents/Resources/ inside a bundle; the launcher and
# aurora look for resources/ and the pipeline cache there.
cp -r "${ROOT_DIR}/resources" "${APP}/Contents/Resources/resources"
gzip -dc "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" \
    > "${APP}/Contents/Resources/initial_pipeline_cache.db"

echo "=== Bundling Homebrew dylibs ==="
# Copy every non-system dylib the binary (transitively) loads into
# Contents/Frameworks and point the load commands at @rpath.
FRAMEWORKS="${APP}/Contents/Frameworks"
# Print the file an @rpath/<name> dependency of <source> resolves to, using
# <source>'s LC_RPATH entries. Homebrew dylibs load their siblings this way
# (libbrotlidec -> @rpath/libbrotlicommon with @loader_path/../lib).
resolve_rpath_dep() {
    local source="$1" name="$2"
    local dir; dir="$(dirname "${source}")"
    otool -l "${source}" | awk '$1 == "cmd" { rpath = ($2 == "LC_RPATH") }
                               rpath && $1 == "path" { print $2 }' |
    while read -r rpath; do
        rpath="${rpath/#@loader_path/${dir}}"
        rpath="${rpath/#@executable_path/${dir}}"
        if [[ -f "${rpath}/${name}" ]]; then
            echo "${rpath}/${name}"
            break
        fi
    done
}
# bundle_deps <target> [<source>]: <source> is where <target> was copied from,
# so its @loader_path rpaths still point at the Homebrew prefix.
bundle_deps() {
    local target="$1" source="${2:-$1}"
    otool -L "${target}" | awk 'NR > 1 { print $1 }' | while read -r dep; do
        local name; name="$(basename "${dep}")"
        local from="${dep}"
        case "${dep}" in
            /usr/lib/*|/System/*|@executable_path/*|@loader_path/*) continue ;;
            @rpath/*)
                [[ "${name}" == "$(basename "${target}")" ]] && continue
                [[ -f "${FRAMEWORKS}/${name}" ]] && continue
                from="$(resolve_rpath_dep "${source}" "${name}")"
                if [[ -z "${from}" ]]; then
                    echo "error: ${target} needs ${dep}, not found via ${source}'s rpaths" >&2
                    exit 1
                fi
                ;;
        esac
        if [[ ! -f "${FRAMEWORKS}/${name}" ]]; then
            cp -L "${from}" "${FRAMEWORKS}/${name}"
            chmod u+w "${FRAMEWORKS}/${name}"
            install_name_tool -id "@rpath/${name}" "${FRAMEWORKS}/${name}"
            bundle_deps "${FRAMEWORKS}/${name}" "${from}"
        fi
        if [[ "${dep}" != "@rpath/${name}" ]]; then
            install_name_tool -change "${dep}" "@rpath/${name}" "${target}"
        fi
    done
}
bundle_deps "${APP}/Contents/MacOS/Xelee" "${BUILD_DIR}/melee"
install_name_tool -add_rpath "@executable_path/../Frameworks" "${APP}/Contents/MacOS/Xelee"
ls "${FRAMEWORKS}"
# Every @rpath load command must now name a bundled file; a miss here is a
# launch-time "Library not loaded" on any Mac without the same Homebrew.
for f in "${APP}/Contents/MacOS/Xelee" "${FRAMEWORKS}"/*.dylib; do
    otool -L "${f}" | awk 'NR > 1 { print $1 }' | grep '^@rpath/' | while read -r dep; do
        [[ "${dep}" == "@rpath/$(basename "${f}")" ]] && continue
        if [[ ! -f "${FRAMEWORKS}/${dep#@rpath/}" ]]; then
            echo "error: $(basename "${f}") needs ${dep}, which is not in Frameworks" >&2
            exit 1
        fi
    done
done

echo "=== Signing (ad hoc) ==="
# No Developer ID: an ad-hoc signature keeps the arm64 kernel happy after
# install_name_tool rewrote the load commands. First launch of a downloaded
# copy still needs Open Anyway in Privacy & Security (Gatekeeper); see the
# release notes.
codesign --force --deep --sign - --timestamp=none "${APP}"
codesign --verify --deep --strict "${APP}"

echo "=== Generating zip ==="
mkdir -p "${DIST_DIR}"
rm -f "${DIST_DIR}/Xelee-macOS-${ARCH}.zip"
ditto -c -k --keepParent "${APP}" "${DIST_DIR}/Xelee-macOS-${ARCH}.zip"

echo "=== Packaging Complete ==="
ls -lh "${DIST_DIR}"
