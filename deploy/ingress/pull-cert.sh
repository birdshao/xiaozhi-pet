#!/bin/sh
# Pull the TLS certificate that Lucky (ACME, wildcard *.birdshao.cn) exports and install it for the
# ingress. Lucky renews it; this runs daily from pet-cert-pull.timer and reloads nginx only on change.
# Config (not secret) in /etc/pet-ingress/lucky.env:
#   LUCKY_SSH=user@192.168.1.224      LUCKY_SSH_PORT=22
# The Lucky side runs only lucky-export.sh (forced command): it prints the crt for the request "crt" and the
# key for "key", nothing else.
# Auth: the dedicated key /etc/pet-ingress/lucky_pull_ed25519 (no other use). Run by hand to test.
set -eu
. /etc/pet-ingress/lucky.env
HOST="${PET_INGRESS_HOST:-pet.birdshao.cn}"
CERTS=/etc/pet-ingress/certs
SSH="ssh -i /etc/pet-ingress/lucky_pull_ed25519 -p ${LUCKY_SSH_PORT:-22} -o BatchMode=yes -o ConnectTimeout=10 \
 -o UserKnownHostsFile=/etc/pet-ingress/lucky_known_hosts -o StrictHostKeyChecking=yes $LUCKY_SSH"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
umask 077
$SSH crt > "$tmp/fullchain.pem"
$SSH key > "$tmp/privkey.pem"

# Refuse anything that would break the ingress or be useless to devices.
openssl x509 -in "$tmp/fullchain.pem" -noout -checkhost "$HOST" | grep -q "does match" \
    || { echo "certificate does not cover $HOST" >&2; exit 1; }
openssl x509 -in "$tmp/fullchain.pem" -noout -checkend 604800 >/dev/null \
    || { echo "certificate expires within 7 days or is expired" >&2; exit 1; }
[ "$(grep -c 'BEGIN CERTIFICATE' "$tmp/fullchain.pem")" -ge 2 ] \
    || { echo "export has no intermediate certificate (need the full chain)" >&2; exit 1; }
a="$(openssl x509 -in "$tmp/fullchain.pem" -noout -pubkey | openssl sha256)"
b="$(openssl pkey -in "$tmp/privkey.pem" -pubout | openssl sha256)"
[ "$a" = "$b" ] || { echo "private key does not match the certificate" >&2; exit 1; }

if cmp -s "$tmp/fullchain.pem" "$CERTS/fullchain.pem" && cmp -s "$tmp/privkey.pem" "$CERTS/privkey.pem"; then
    echo "certificate unchanged"
    exit 0
fi
install -m 600 "$tmp/privkey.pem" "$CERTS/privkey.pem"
install -m 644 "$tmp/fullchain.pem" "$CERTS/fullchain.pem"
/opt/pet-ingress/reload-ingress.sh
echo "certificate updated: $(openssl x509 -in "$CERTS/fullchain.pem" -noout -enddate)"
