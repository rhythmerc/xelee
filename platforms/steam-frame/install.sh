#!/usr/bin/env bash
# Installs or updates Xelee on a Steam Frame: the newest release's
# Xelee-SteamFrame-arm64.tar.gz into ~/Applications/xelee, and Xelee into
# Steam's library the first time. Run by the release's .desktop file (an
# asset, Install-Xelee.desktop: open it on the headset), or by hand:
#
#   curl -fsSL https://raw.githubusercontent.com/rhythmerc/xelee/main/platforms/steam-frame/install.sh | bash
#
# Kept across updates: logs/, melee-env.txt and dumps/ (the settings live in
# ~/.local/share/melee-pc). Knobs, for testing:
#   XELEE_REPO=owner/name     where the releases are (default rhythmerc/xelee)
#   XELEE_PACKAGE=path|URL    this package instead of a release's
#   XELEE_DIR=dir             where to install (default ~/Applications/xelee)
#   XELEE_NO_STEAM=1          leave Steam's library alone
set -euo pipefail

REPO="${XELEE_REPO:-rhythmerc/xelee}"
DEST="${XELEE_DIR:-${HOME}/Applications/xelee}"
ASSET="Xelee-SteamFrame-arm64.tar.gz"

say() { printf '%s\n' "$@"; }
fail() {
    say "" "Error: $*"
    finish 1
}
# The terminal the .desktop file opened closes with the script: wait for a
# key there, not when run from a shell.
finish() {
    if [[ -t 0 && -n "${XELEE_FROM_DESKTOP:-}" ]]; then
        say "" "Press Enter to close."
        read -r _ || true
    fi
    exit "${1:-0}"
}

[[ "$(uname -m)" == aarch64 ]] || fail "this installs the Steam Frame build (Linux arm64); this machine is $(uname -m)."
for tool in curl tar rsync python3; do
    command -v "${tool}" >/dev/null || fail "${tool} is missing."
done

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# The package: a given one, or the newest release that has the Steam Frame's
# (the release list, not "latest": Xelee's releases are prereleases).
tag=""
if [[ -n "${XELEE_PACKAGE:-}" ]]; then
    say "Package: ${XELEE_PACKAGE}"
    if [[ -f "${XELEE_PACKAGE}" ]]; then
        cp "${XELEE_PACKAGE}" "${work}/${ASSET}"
    else
        curl -fL --progress-bar -o "${work}/${ASSET}" "${XELEE_PACKAGE}" || fail "couldn't download ${XELEE_PACKAGE}."
    fi
    tag="(given package)"
else
    say "Looking for the newest Xelee release with the Steam Frame package..."
    curl -fsSL -H "Accept: application/vnd.github+json" "https://api.github.com/repos/${REPO}/releases?per_page=30" \
        -o "${work}/releases.json" || fail "couldn't reach GitHub (is the headset online?)."
    read -r tag url < <(python3 - "${work}/releases.json" "${ASSET}" <<'EOF'
import json, sys
releases, asset = json.load(open(sys.argv[1])), sys.argv[2]
for r in releases:
    if r.get("draft"):
        continue
    for a in r.get("assets", []):
        if a.get("name") == asset:
            print(r["tag_name"], a["browser_download_url"])
            sys.exit(0)
print("", "")
EOF
    ) || true
    [[ -n "${tag}" ]] || fail "no release of ${REPO} has ${ASSET} yet."
    if [[ -f "${DEST}/.release" && "$(cat "${DEST}/.release")" == "${tag}" ]]; then
        say "Xelee ${tag} is already installed in ${DEST}: up to date."
        finish 0
    fi
    say "Downloading Xelee ${tag}..."
    curl -fL --progress-bar -o "${work}/${ASSET}" "${url}" || fail "the download failed."
fi

say "Unpacking..."
tar -xzf "${work}/${ASSET}" -C "${work}" || fail "the package is damaged; try again."
[[ -x "${work}/xelee/xelee.sh" && -x "${work}/xelee/melee" ]] || fail "the package lacks xelee.sh or melee."

# A running game keeps its files open: say so rather than replace them under it.
for p in $(pgrep -u "${USER}" -x 'Main thread' || true); do
    if [[ "$(readlink "/proc/${p}/exe" 2>/dev/null)" == "${DEST}/melee" ]]; then
        fail "Xelee is running; close it, then run the installer again."
    fi
done

update=false
[[ -d "${DEST}" ]] && update=true
mkdir -p "${DEST}"
rsync -a --delete --exclude melee-env.txt --exclude logs/ --exclude dumps/ --exclude .release \
    "${work}/xelee/" "${DEST}/"
printf '%s\n' "${tag}" > "${DEST}/.release"
say "$( ${update} && echo Updated || echo Installed ) Xelee ${tag} in ${DEST}."

# Steam's library, once: Steam rewrites its list of non-Steam games when it
# quits, so it is closed while the entry goes in, then started again.
in_library() {
    local f
    for f in "${HOME}"/.local/share/Steam/userdata/*/config/shortcuts.vdf; do
        [[ -f "${f}" ]] && grep -aq "${DEST}/xelee.sh" "${f}" && return 0
    done
    return 1
}
if [[ -n "${XELEE_NO_STEAM:-}" ]]; then
    say "Steam's library left alone (XELEE_NO_STEAM)."
elif in_library; then
    say "Xelee is already in your Steam library."
else
    say "" "Adding Xelee to your Steam library: Steam closes for a moment and starts again."
    unit=false
    systemctl --user is-active --quiet steam.service 2>/dev/null && unit=true
    if pgrep -u "${USER}" -x steam >/dev/null; then
        if ${unit}; then
            systemctl --user stop steam.service
        else
            steam -shutdown >/dev/null 2>&1 || true
        fi
        for _ in $(seq 1 60); do
            pgrep -u "${USER}" -x steam >/dev/null || break
            sleep 1
        done
    fi
    python3 "${DEST}/add-to-steam.py" "${DEST}" >/dev/null || say "Couldn't add Xelee to Steam's library; add ${DEST}/xelee.sh as a non-Steam game."
    if ${unit}; then
        systemctl --user start steam.service
    else
        (nohup steam >/dev/null 2>&1 &)
    fi
    in_library && say "Added: Xelee is in your library."
fi

say "" "Done. Start Xelee from your Steam library; keep the headset at 120 Hz for the smoothest fights."
finish 0
