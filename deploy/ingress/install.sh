#!/bin/sh
# One-shot, idempotent host setup for the public ingress. Run as root on the CT from this directory
# (/opt/pet/app/deploy/ingress). Does NOT read or write the DNSPod token; see README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
HOST="${PET_INGRESS_HOST:-pet.birdshao.cn}"
ACME_VERSION="${ACME_VERSION:-3.1.0}"

install -d -m 700 /etc/pet-ingress /etc/pet-ingress/certs
install -d -m 750 /var/log/pet-ingress /opt/pet-ingress
install -m 755 "$HERE/issue-cert.sh" "$HERE/acme-renew.sh" "$HERE/reload-ingress.sh" "$HERE/pet-ddns.py" /opt/pet-ingress/
install -m 644 "$HERE"/pet-ddns.service "$HERE"/pet-ddns.timer "$HERE"/pet-acme-renew.service \
    "$HERE"/pet-acme-renew.timer /etc/systemd/system/
install -m 644 "$HERE/pet-ingress.logrotate" /etc/logrotate.d/pet-ingress

if [ ! -x /opt/acme.sh/acme.sh ]; then
    tmp="$(mktemp -d)"
    curl -fsSL "https://github.com/acmesh-official/acme.sh/archive/refs/tags/${ACME_VERSION}.tar.gz" | tar -xz -C "$tmp"
    (cd "$tmp"/acme.sh-* && ./acme.sh --install --home /opt/acme.sh --nocron --noprofile)
    rm -rf "$tmp"
fi

# Placeholder certificate so the ingress can start before the real one exists. It is NOT trusted by
# devices: they fail closed until issue-cert.sh has replaced it.
if [ ! -s /etc/pet-ingress/certs/fullchain.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=$HOST" -addext "subjectAltName=DNS:$HOST" \
        -keyout /etc/pet-ingress/certs/privkey.pem -out /etc/pet-ingress/certs/fullchain.pem 2>/dev/null
    chmod 600 /etc/pet-ingress/certs/privkey.pem
    echo "placeholder self-signed certificate installed"
fi

"$HERE/firewall.sh"

systemctl daemon-reload
systemctl enable --now pet-acme-renew.timer
if [ -s /etc/pet-ingress/dnspod.env ]; then
    systemctl enable --now pet-ddns.timer
else
    echo "no /etc/pet-ingress/dnspod.env yet: DDNS timer not enabled (see README.md)"
fi
echo "done"
