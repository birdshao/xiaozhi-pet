#!/bin/sh
# Forced SSH command on the Lucky host (authorized_keys: restrict,command="/usr/local/bin/pet-cert-export" ...).
# The only thing the CT 108 pull key can do: print the exported certificate or its key.
CERT=bitwarden-prod-le
DIR=/goodluck/cert_export
case "${SSH_ORIGINAL_COMMAND:-}" in
    crt) exec cat "$DIR/$CERT.crt" ;;
    key) exec cat "$DIR/$CERT.key" ;;
    *) echo "unsupported request" >&2; exit 1 ;;
esac
