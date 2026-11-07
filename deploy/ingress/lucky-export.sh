#!/bin/sh
# Forced SSH command on the Lucky host (authorized_keys: restrict,command="/usr/local/bin/pet-cert-export" ...).
# The only thing the CT 108 pull key can do: print the exported certificate (full chain) or its key.
CERT=bitwarden-prod-le
DIR=/docker/goodluck/cert_export     # host side of the Lucky container's /goodluck mount
case "${SSH_ORIGINAL_COMMAND:-}" in
    crt)
        if [ "$(grep -c 'BEGIN CERTIFICATE' "$DIR/$CERT.crt")" -ge 2 ]; then
            exec cat "$DIR/$CERT.crt"
        fi
        exec cat "$DIR/$CERT.crt" "$DIR/${CERT}_issuerCertificate.crt" ;;
    key) exec cat "$DIR/$CERT.key" ;;
    *) echo "unsupported request" >&2; exit 1 ;;
esac
