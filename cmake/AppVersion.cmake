# The version the app reports, the updater compares and XeleeNet lobbies
# match on (pc_app_rev): run at every build (cmake -P), so a rebuild never
# keeps an old one. -DSRC=<repo> -DOUT=<header>.
#
# A release build has MELEE_VERSION (the tag, from CI). Any other build is
# the newest v* tag plus where it is from, as git describe writes it:
# v0.2.2-beta-135-ge53c3c5e. The newest tag, not the nearest: a hotfix tag on
# a release branch is not in main's history, and main is past it. Every
# platform built from one commit says the same, so a PC and a headset built
# apart still find each other's lobbies, and the updater ranks it above that
# tag (version.cpp). platforms/android/app/build.gradle does the same for the
# APK's versionName; keep them together.
if (NOT "$ENV{MELEE_VERSION}" STREQUAL "")
    set(version "$ENV{MELEE_VERSION}")
else ()
    set(version "v0.1.0-beta") # no git, or no tags
    find_package(Git QUIET)
    if (GIT_FOUND)
        # Pre-releases before their release: v0.3.0-beta < v0.3.0.
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" -c versionsort.suffix=- tag -l "v*" --sort=-v:refname
                        OUTPUT_VARIABLE tags RESULT_VARIABLE tags_failed ERROR_QUIET)
        string(REGEX MATCH "^[^\n]+" newest "${tags}")
        if (NOT tags_failed AND newest)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" rev-list --count "${newest}..HEAD"
                            OUTPUT_VARIABLE count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SRC}" rev-parse --short=9 HEAD
                            OUTPUT_VARIABLE hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if (count STREQUAL "0")
                set(version "${newest}")
            elseif (count AND hash)
                set(version "${newest}-${count}-g${hash}")
            endif ()
        endif ()
    endif ()
endif ()
if (NOT version MATCHES "^v")
    set(version "v${version}")
endif ()

set(text "#define MELEE_APP_VERSION \"${version}\"\n")
if (EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif ()
if (NOT old STREQUAL text) # untouched when the same: nothing recompiles
    file(WRITE "${OUT}" "${text}")
    message(STATUS "melee: version ${version}")
endif ()
