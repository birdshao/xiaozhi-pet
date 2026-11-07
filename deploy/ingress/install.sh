#!/bin/sh
# One-shot, idempotent host setup for the public ingress. Run as root on the CT from this directory
# (/opt/pet/app/deploy/ingress). DDNS and the certificate are managed by Lucky; this host only pulls the
# certificate Lucky exports (pull-cert.sh). See README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
HOST="${PET_INGRESS_HOST:-pet.birdshao.cn}"

install -d -m 700 /etc/pet-ingress /etc/pet-ingress/certs
install -d -m 750 /var/log/pet-ingress /opt/pet-ingress
install -m 755 "$HERE/pull-cert.sh" "$HERE/reload-ingress.sh" /opt/pet-ingress/
install -m 644 "$HERE/pet-cert-pull.service" "$HERE/pet-cert-pull.timer" /etc/systemd/system/
install -m 644 "$HERE/pet-ingress.logrotate" /etc/logrotate.d/pet-ingress

# Dedicated key for pulling the certificate from Lucky (public half is printed for you to authorize there).
if [ ! -f /etc/pet-ingress/lucky_pull_ed25519 ]; then
    ssh-keygen -q -t ed25519 -N '' -C "pet-ingress cert pull" -f /etc/pet-ingress/lucky_pull_ed25519
fi
echo "public key to authorize on the Lucky host:"; cat /etc/pet-ingress/lucky_pull_ed25519.pub

# Placeholder certificate so the ingress can start before the real one exists. It is NOT trusted by
# devices: they fail closed until pull-cert.sh has replaced it.
if [ ! -s /etc/pet-ingress/certs/fullchain.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=$HOST" -addext "subjectAltName=DNS:$HOST" \
        -keyout /etc/pet-ingress/certs/privkey.pem -out /etc/pet-ingress/certs/fullchain.pem 2>/dev/null
    chmod 600 /etc/pet-ingress/certs/privkey.pem
    echo "placeholder self-signed certificate installed"
fi

"$HERE/firewall.sh"

systemctl daemon-reload
if [ -s /etc/pet-ingress/lucky.env ] && [ -s /etc/pet-ingress/lucky_known_hosts ]; then
    systemctl enable --now pet-cert-pull.timer
else
    echo "no lucky.env / lucky_known_hosts yet: cert pull timer not enabled (see README.md)"
fi
echo "done"
