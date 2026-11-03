#!/usr/bin/env bash
# Build a static libusb for a MinGW target, for SDL's hidapi.
#
# SDL reaches the Wii U / Switch GameCube adapter (057e:0337) only through
# libusb: its Windows HID backend skips that device on purpose
# (SDL_hidapi.c SDL_libusb_required[]). An SDL built without libusb therefore
# never shows the adapter to src/pc/gcadapter.c or anything else. Linking a
# static libusb into SDL3.dll adds no DLL to ship. The adapter still needs the
# WinUSB driver (Zadig), as for Dolphin.
#
#   tools/windows/build_libusb.sh <host-triple> <c-compiler> <work-dir>
#
# Installs into <work-dir>/install (include/libusb-1.0/libusb.h,
# lib/libusb-1.0.a) and skips the build when that is already there.
set -euo pipefail

HOST="$1"
CC_BIN="$2"
WORK_DIR="$3"

LIBUSB_VERSION="1.0.30"
LIBUSB_SHA256="fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2"

PREFIX="${WORK_DIR}/install"
STAMP="${PREFIX}/.libusb-${LIBUSB_VERSION}-${HOST}"
if [[ -f "${STAMP}" && -f "${PREFIX}/lib/libusb-1.0.a" ]]; then
    echo "  libusb ${LIBUSB_VERSION} already built in ${PREFIX}"
    exit 0
fi

mkdir -p "${WORK_DIR}"
TARBALL="${WORK_DIR}/libusb-${LIBUSB_VERSION}.tar.bz2"
if [[ ! -f "${TARBALL}" ]] || ! echo "${LIBUSB_SHA256}  ${TARBALL}" | sha256sum -c --status; then
    curl -sSLf -o "${TARBALL}.part" "${LIBUSB_URL}"
    mv "${TARBALL}.part" "${TARBALL}"
fi
if ! echo "${LIBUSB_SHA256}  ${TARBALL}" | sha256sum -c --status; then
    echo "error: ${TARBALL} does not match the pinned sha256" >&2
    exit 1
fi

SRC_DIR="${WORK_DIR}/libusb-${LIBUSB_VERSION}"
rm -rf "${SRC_DIR}" "${PREFIX}"
tar -xjf "${TARBALL}" -C "${WORK_DIR}"
(
    cd "${SRC_DIR}"
    ./configure --host="${HOST}" CC="${CC_BIN}" --prefix="${PREFIX}" \
        --enable-static --disable-shared --disable-examples-build --disable-tests-build \
        >"${WORK_DIR}/configure.log" 2>&1 || {
        tail -n 40 "${WORK_DIR}/configure.log" >&2
        exit 1
    }
    make -j"$(nproc)" install >"${WORK_DIR}/make.log" 2>&1 || {
        tail -n 40 "${WORK_DIR}/make.log" >&2
        exit 1
    }
)
touch "${STAMP}"
echo "  libusb ${LIBUSB_VERSION} -> ${PREFIX}"
