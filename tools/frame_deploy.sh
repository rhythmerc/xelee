#!/usr/bin/env bash
# Puts the Steam Frame build on a headset in Developer Mode, over SSH, and
# runs it, for testing (docs/steam-frame.md):
#
#   tools/frame_deploy.sh deploy        dist/Xelee-SteamFrame-arm64.tar.gz into
#                                       ~/Applications/xelee (melee-env.txt,
#                                       logs and settings there are kept)
#   tools/frame_deploy.sh run [args]    starts it: through Steam when
#                                       FRAME_APPID names its non-Steam
#                                       shortcut, else directly, in the running
#                                       Steam client's environment
#   tools/frame_deploy.sh stop          ends it
#   tools/frame_deploy.sh env 'K=V ...' writes melee-env.txt (knobs, one a line)
#   tools/frame_deploy.sh log           the newest log, as it grows
#   tools/frame_deploy.sh fetch DIR     the logs, frame dumps (AURORA_XR_DUMP=dumps)
#                                       and coredump list into DIR
#
# FRAME_HOST is the headset's SSH destination (default steamos@frame).
set -euo pipefail

HOST="${FRAME_HOST:-steamos@frame}"
DEST="${FRAME_DIR:-Applications/xelee}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE="${FRAME_PACKAGE:-${ROOT}/dist/Xelee-SteamFrame-arm64.tar.gz}"

remote() { ssh -o BatchMode=yes "${HOST}" "$@"; }
# Ends the game: found by its executable, since it names its main thread
# "Main thread" (no pkill -x melee).
STOP="pids=\$(for p in \$(pgrep -u \"\$USER\" -x 'Main thread'); do \
    [ \"\$(readlink /proc/\$p/exe)\" = \"\$HOME/${DEST}/melee\" ] && echo \$p; done); \
    if [ -n \"\$pids\" ]; then kill \$pids; \
    for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 \$pids 2>/dev/null || break; sleep 0.5; done; fi; true"

case "${1:-}" in
deploy)
    [[ -f "${PACKAGE}" ]] || { echo "no ${PACKAGE}: run tools/package_steam_frame.sh" >&2; exit 1; }
    stage="$(mktemp -d)"
    trap 'rm -rf "${stage}"' EXIT
    tar -xzf "${PACKAGE}" -C "${stage}"
    remote "mkdir -p '${DEST}'"
    rsync -a --delete --exclude melee-env.txt --exclude logs/ --exclude dumps/ --exclude '*.cfg' \
        "${stage}/xelee/" "${HOST}:${DEST}/"
    echo "deployed to ${HOST}:${DEST}"
    ;;
run)
    shift
    if [[ -n "${FRAME_APPID:-}" ]]; then
        remote "steam -ifrunning steam://rungameid/${FRAME_APPID}"
    else
        # Steam's own environment (its display, audio and XR runtime), from
        # the running client, so the game starts as Steam would start it.
        remote "pid=\$(pgrep -n -u \"\$USER\" -x steam || pgrep -n -u \"\$USER\" steamwebhelper); \
            [ -n \"\$pid\" ] || { echo 'Steam is not running on the headset' >&2; exit 1; }; \
            while IFS= read -r -d '' kv; do case \"\$kv\" in \
                DISPLAY=*|WAYLAND_DISPLAY=*|XDG_RUNTIME_DIR=*|DBUS_SESSION_BUS_ADDRESS=*|PULSE_SERVER=*|XR_RUNTIME_JSON=*|STEAM_*|SteamGameId=*) export \"\$kv\";; \
            esac; done < /proc/\$pid/environ; \
            ${STOP}; \
            cd '${DEST}' || exit 1; mkdir -p logs; \
            nohup ./xelee.sh $* > logs/console.txt 2>&1 < /dev/null &"
    fi
    echo "started on ${HOST}"
    ;;
stop)
    remote "${STOP}"
    ;;
env)
    printf '%s\n' ${2:-} | remote "cat > '${DEST}/melee-env.txt'"
    remote "cat '${DEST}/melee-env.txt'"
    ;;
log)
    remote "tail -n +1 -F \$(ls -1t '${DEST}'/logs/xelee-*.log | head -n1)"
    ;;
fetch)
    out="${2:?usage: fetch DIR}"
    mkdir -p "${out}"
    rsync -a "${HOST}:${DEST}/logs/" "${out}/logs/" || true
    rsync -a "${HOST}:${DEST}/dumps/" "${out}/dumps/" 2>/dev/null || true
    remote "coredumpctl list --no-pager 2>/dev/null | tail -n 5" > "${out}/coredumps.txt" || true
    echo "fetched into ${out}"
    ;;
*)
    sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
