#!/bin/sh
# N3/N10: let the public reach the ingress port and nothing else. ufw opens 8443; Docker-published
# ports bypass ufw's INPUT chain, so the DOCKER-USER rule that drops non-LAN traffic needs the same
# exception. Idempotent. Run as root on the CT.
set -eu
PORT="${PET_INGRESS_PORT:-8443}"
RULES=/etc/ufw/after.rules
ufw allow "${PORT}/tcp" comment 'pet ingress (public HTTPS)' >/dev/null
if ! grep -q "dport ${PORT} -j RETURN" "$RULES"; then
    cp "$RULES" "${RULES}.bak-$(date +%Y%m%d%H%M%S)"
    sed -i "/^-A DOCKER-USER -i eth0 -j DROP/i -A DOCKER-USER -i eth0 -p tcp --dport ${PORT} -j RETURN" "$RULES"
fi
ufw reload >/dev/null
ufw status | grep -E "^(22|8780|${PORT})/tcp"
iptables -S DOCKER-USER
