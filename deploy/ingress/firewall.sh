#!/bin/sh
# N3/N10: let the public reach the ingress port and nothing else. ufw opens 8443; Docker-published
# ports bypass ufw's INPUT chain, so the DOCKER-USER rule that drops non-LAN traffic needs the same
# exception, placed before that DROP. Idempotent; fails loudly if the exception is not in effect.
# Run as root on the CT.
set -eu
PORT="${PET_INGRESS_PORT:-8443}"
RULES=/etc/ufw/after.rules
ufw allow "${PORT}/tcp" comment 'pet ingress (public HTTPS)' >/dev/null
if ! grep -q "dport ${PORT} -j RETURN" "$RULES"; then
    grep -q '^-A DOCKER-USER -i eth0 -j DROP' "$RULES" \
        || { echo "no 'DOCKER-USER -i eth0 -j DROP' line in $RULES: not touching it, check the rules by hand" >&2; exit 1; }
    cp "$RULES" "${RULES}.bak-$(date +%Y%m%d%H%M%S)"
    sed -i "/^-A DOCKER-USER -i eth0 -j DROP/i -A DOCKER-USER -i eth0 -p tcp --dport ${PORT} -j RETURN" "$RULES"
    grep -q "dport ${PORT} -j RETURN" "$RULES" || { echo "failed to insert the DOCKER-USER exception" >&2; exit 1; }
fi
ufw reload >/dev/null
ufw status | grep -E "^(22|8780|${PORT})/tcp"
iptables -S DOCKER-USER

# The live chain must have the exception, and before any DROP of eth0 traffic.
chain="$(iptables -S DOCKER-USER)"
ret="$(echo "$chain" | grep -n -e "--dport ${PORT} -j RETURN" | head -1 | cut -d: -f1 || true)"
drop="$(echo "$chain" | grep -n -e '-i eth0 -j DROP' | head -1 | cut -d: -f1 || true)"
if [ -z "$ret" ]; then echo "FAIL: DOCKER-USER has no RETURN for port ${PORT}" >&2; exit 1; fi
if [ -n "$drop" ] && [ "$ret" -gt "$drop" ]; then echo "FAIL: the port ${PORT} exception comes after the DROP" >&2; exit 1; fi
echo "ok: DOCKER-USER passes TCP ${PORT}"
