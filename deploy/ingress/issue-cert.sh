#!/bin/sh
# Issue (or force-renew) the Let's Encrypt certificate with DNS-01 on DNSPod (no port 80 needed) and
# install it for the ingress. Needs /etc/pet-ingress/dnspod.env (DP_Id, DP_Key). Usage:
#   issue-cert.sh            real certificate
#   issue-cert.sh staging    Let's Encrypt staging CA (untrusted; a dry run of the DNS-01 flow)
set -eu
HOST="${PET_INGRESS_HOST:-pet.birdshao.cn}"
ACME=/opt/acme.sh/acme.sh
CERTS=/etc/pet-ingress/certs
SERVER=letsencrypt
[ "${1:-}" = staging ] && SERVER=letsencrypt_test

set -a; . /etc/pet-ingress/dnspod.env; set +a
if [ -z "${DP_Id:-}" ] || [ -z "${DP_Key:-}" ]; then
    echo "DP_Id/DP_Key missing in /etc/pet-ingress/dnspod.env" >&2
    exit 1
fi

# RSA-2048: the firmware's mbedTLS certificate bundle verifies it everywhere. Exit 2 = not due yet.
rc=0
"$ACME" --home /opt/acme.sh --issue --dns dns_dp -d "$HOST" --keylength 2048 --server "$SERVER" || rc=$?
[ "$rc" = 0 ] || [ "$rc" = 2 ] || exit "$rc"
"$ACME" --home /opt/acme.sh --install-cert -d "$HOST" \
    --fullchain-file "$CERTS/fullchain.pem" --key-file "$CERTS/privkey.pem" \
    --reloadcmd "/opt/pet-ingress/reload-ingress.sh"
chmod 600 "$CERTS/privkey.pem"
