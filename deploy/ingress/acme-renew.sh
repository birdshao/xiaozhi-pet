#!/bin/sh
# Daily from pet-acme-renew.timer: renews when < 30 days are left; acme.sh then runs the reload command
# saved by issue-cert.sh.
set -eu
set -a; . /etc/pet-ingress/dnspod.env; set +a
exec /opt/acme.sh/acme.sh --home /opt/acme.sh --cron
