#!/bin/sh
# Validate the ingress config, then reload it (called by pull-cert.sh after a certificate change, and by hand).
set -eu
docker exec pet-ingress nginx -t
docker kill -s HUP pet-ingress >/dev/null
echo "pet-ingress reloaded"
