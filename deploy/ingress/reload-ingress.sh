#!/bin/sh
# Validate the ingress config, then reload it (called by acme.sh after a renewal, and by hand).
set -eu
docker exec pet-ingress nginx -t
docker kill -s HUP pet-ingress >/dev/null
echo "pet-ingress reloaded"
