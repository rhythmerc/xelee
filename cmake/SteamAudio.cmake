# Steam Audio (Valve, Apache-2.0) for positional sound effects
# (src/pc/spatial_audio.c). Only its direct path is used: the HRTF binaural
# effect and the direct effect's distance gain, no simulation.
#
# The SDK zip is fetched from Valve's GitHub release, pinned by version and
# SHA-256, into MELEE_DEPS_DIR (shared by every build directory of this
# checkout), and only the header and this target's libphonon.so are
# unpacked. Linux x86-64 and Android arm64 only for now; anywhere else, or
# when the download fails, the feature is compiled out and the game builds
# as before.
#
# Sets MELEE_SPATIAL_AUDIO_READY and the imported target melee_phonon.

set(MELEE_STEAM_AUDIO_VERSION "4.8.1")
set(MELEE_STEAM_AUDIO_SHA256 "4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449")
set(MELEE_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/deps" CACHE PATH
    "Where downloaded third-party SDKs (Steam Audio) are kept, outside any one build directory")
option(MELEE_FETCH_STEAM_AUDIO "Download the Steam Audio SDK when it is not in MELEE_DEPS_DIR" ON)

set(MELEE_SPATIAL_AUDIO_READY OFF)

if (ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
    set(_sa_lib "android-armv8")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_sa_lib "linux-x64")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    # Not in the release SDK: built from its source by tools/build_steam_audio.sh
    # (tools/package_steam_frame.sh runs it), beside the SDK's headers.
    set(_sa_lib "linux-arm64")
else ()
    message(STATUS "melee: spatial audio off (no Steam Audio library wired up for this platform yet)")
    return()
endif ()

set(_sa_dir "${MELEE_DEPS_DIR}/steamaudio-${MELEE_STEAM_AUDIO_VERSION}")
set(_sa_zip "${MELEE_DEPS_DIR}/downloads/steamaudio_${MELEE_STEAM_AUDIO_VERSION}.zip")
set(_sa_header "${_sa_dir}/steamaudio/include/phonon.h")
set(_sa_so "${_sa_dir}/steamaudio/lib/${_sa_lib}/libphonon.so")

if (NOT EXISTS "${_sa_header}" OR NOT EXISTS "${_sa_so}")
    if (NOT MELEE_FETCH_STEAM_AUDIO AND NOT EXISTS "${_sa_zip}")
        message(STATUS "melee: spatial audio off (no Steam Audio SDK in ${_sa_dir}; MELEE_FETCH_STEAM_AUDIO is OFF)")
        return()
    endif ()
    # A zip already there with the right hash is not downloaded again.
    set(_sa_url "https://github.com/ValveSoftware/steam-audio/releases/download/v${MELEE_STEAM_AUDIO_VERSION}/steamaudio_${MELEE_STEAM_AUDIO_VERSION}.zip")
    message(STATUS "melee: fetching Steam Audio ${MELEE_STEAM_AUDIO_VERSION} (180 MB, once per checkout)")
    file(DOWNLOAD "${_sa_url}" "${_sa_zip}" EXPECTED_HASH SHA256=${MELEE_STEAM_AUDIO_SHA256} STATUS _sa_status)
    list(GET _sa_status 0 _sa_code)
    if (NOT _sa_code EQUAL 0)
        file(REMOVE "${_sa_zip}")
        message(WARNING "melee: downloading ${_sa_url} failed (${_sa_status}); spatial audio off")
        return()
    endif ()
    file(ARCHIVE_EXTRACT INPUT "${_sa_zip}" DESTINATION "${_sa_dir}"
         PATTERNS "steamaudio/include/*" "steamaudio/lib/${_sa_lib}/*" "steamaudio/THIRDPARTY.md")
    if (_sa_lib STREQUAL "linux-arm64" AND EXISTS "${_sa_header}" AND NOT EXISTS "${_sa_so}")
        message(STATUS "melee: spatial audio off (no ${_sa_so}: build it with tools/build_steam_audio.sh)")
        return()
    endif ()
    if (NOT EXISTS "${_sa_header}" OR NOT EXISTS "${_sa_so}")
        message(WARNING "melee: ${_sa_zip} has no ${_sa_lib}/libphonon.so; spatial audio off")
        return()
    endif ()
endif ()

add_library(melee_phonon SHARED IMPORTED)
set_target_properties(melee_phonon PROPERTIES
        IMPORTED_LOCATION "${_sa_so}"
        IMPORTED_SONAME "libphonon.so"
        INTERFACE_INCLUDE_DIRECTORIES "${_sa_dir}/steamaudio/include")
# Beside the game, where the loader finds it: the executable's RPATH is
# $ORIGIN on Linux, and tools/build_android.sh copies it into jniLibs.
configure_file("${_sa_so}" "${CMAKE_BINARY_DIR}/libphonon.so" COPYONLY)
set(MELEE_SPATIAL_AUDIO_READY ON)
message(STATUS "melee: spatial audio built in (Steam Audio ${MELEE_STEAM_AUDIO_VERSION}, ${_sa_lib}); "
        "the launcher's Audio setting turns it on or off")
