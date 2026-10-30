#!/bin/sh
# Installs or upgrades the lobby service on a fresh Ubuntu/Debian server
# (README.md, "Running it on a server"). Run from the repository root:
#
#   tools/xeleenet/deploy/provision.sh root@203.0.113.7 xn.example.com [city.mmdb]
#
# It builds the binary here, copies it with the systemd unit, fetches this
# month's DB-IP city database on the server (or copies the one given), opens
# the firewall, and starts the service. The token key is made on the server
# the first time and never leaves it. Running it again upgrades the binary
# and refreshes the database; lobbies survive (-state).
set -eu
host=$1
domain=$2
mmdb=${3:-}
here=$(cd "$(dirname "$0")/.." && pwd)
key=${XELEENET_SSH_KEY:-$HOME/.ssh/xeleenet_ed25519}
ssh="ssh -i $key -o StrictHostKeyChecking=accept-new"
scp="scp -i $key -o StrictHostKeyChecking=accept-new"

go=${GO:-$(command -v go || echo "$HOME/.local/opt/go/bin/go")}
arch=$($ssh "$host" 'dpkg --print-architecture')
(cd "$here" && CGO_ENABLED=0 GOARCH=$arch "$go" build -trimpath -ldflags=-s -o "$here/xeleenet-$arch" .)

$scp "$here/xeleenet-$arch" "$host:/tmp/xeleenet"
$scp "$here/deploy/xeleenet.service" "$host:/etc/systemd/system/xeleenet.service"
[ -n "$mmdb" ] && $scp "$mmdb" "$host:/tmp/city.mmdb"
rm -f "$here/xeleenet-$arch"

$ssh "$host" DOMAIN="$domain" sh -s <<'EOF'
set -eu
install -m 755 /tmp/xeleenet /usr/local/bin/xeleenet && rm /tmp/xeleenet
if [ ! -f /etc/xeleenet.env ]; then
    umask 077
    printf 'XELEENET_TOKEN_KEY=%s\nXELEENET_DOMAIN=%s\n' "$(openssl rand -hex 32)" "$DOMAIN" > /etc/xeleenet.env
fi
mkdir -p /usr/local/share/xeleenet
if [ -f /tmp/city.mmdb ]; then
    install -m 644 /tmp/city.mmdb /usr/local/share/xeleenet/city.mmdb && rm /tmp/city.mmdb
else
    # DB-IP IP to City Lite (CC BY 4.0), published monthly: this month's, or
    # last month's early in the month.
    for month in $(date +%Y-%m) $(date -d '-1 month' +%Y-%m); do
        if curl -sSfLo /tmp/city.mmdb.gz "https://download.db-ip.com/free/dbip-city-lite-$month.mmdb.gz"; then
            gunzip -f /tmp/city.mmdb.gz && install -m 644 /tmp/city.mmdb /usr/local/share/xeleenet/city.mmdb
            break
        fi
    done
    rm -f /tmp/city.mmdb
fi
if command -v ufw >/dev/null; then
    ufw allow OpenSSH >/dev/null
    ufw allow 80/tcp >/dev/null
    ufw allow 443/tcp >/dev/null
    ufw allow 3478/udp >/dev/null
    ufw --force enable >/dev/null
fi
systemctl daemon-reload
systemctl enable xeleenet >/dev/null 2>&1
systemctl restart xeleenet
sleep 2
systemctl --no-pager --lines=5 status xeleenet
EOF
