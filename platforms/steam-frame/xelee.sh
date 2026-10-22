#!/usr/bin/env bash
# Starts Xelee on the Steam Frame. Add this file to Steam as a non-Steam game
# (docs/steam-frame.md); it runs from its own folder, so melee-env.txt (knobs,
# as on the Quest), the logs and the settings stay beside it.
HERE="$(dirname "$(readlink -f "$0")")"
cd "${HERE}" || exit 1
mkdir -p logs
export MELEE_LOG_FILE="${MELEE_LOG_FILE:-${HERE}/logs/xelee-$(date +%Y%m%d-%H%M%S).log}"
# Keep the last ten logs.
ls -1t logs/xelee-*.log 2>/dev/null | tail -n +11 | xargs -r rm -f
# Steam Audio from lib/. The OpenXR loader from lib/openxr/ only when the
# environment Steam starts us in has none of its own: a library path already
# set comes first.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+${LD_LIBRARY_PATH}:}${HERE}/lib:${HERE}/lib/openxr"
exec "${HERE}/melee" "$@"
