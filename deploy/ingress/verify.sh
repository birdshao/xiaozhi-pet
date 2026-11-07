#!/bin/sh
# Acceptance checks for the ingress (N2, N4-N9). Usage: verify.sh [IP [-k]]
#   IP  where to connect instead of DNS (default: this host's LAN address 192.168.1.110)
#   -k  accept the placeholder self-signed certificate (before the real one is issued)
# Exits non-zero if any check fails. Needs curl and openssl; run from the LAN or on the CT.
HOST="${PET_INGRESS_HOST:-pet.birdshao.cn}"
PORT="${PET_INGRESS_PORT:-8443}"
IP="${1:-192.168.1.110}"
KFLAG="${2:-}"
U="https://$HOST:$PORT"
fail=0
c() { curl -s -m 8 --noproxy '*' --resolve "$HOST:$PORT:$IP" $KFLAG "$@"; }
code() { c -o /dev/null -w '%{http_code}' "$@"; }
check() { # name expected actual
    if [ "$2" = "$3" ]; then echo "ok   $1 ($3)"; else echo "FAIL $1: expected $2, got $3"; fail=1; fi
}

echo "== allowed (reaches the app; 401/403/404 from the app itself still count as routed, 404 from nginx does not)"
check /healthz 200 "$(code $U/healthz)"
check /xiaozhi/ota/ 200 "$(code $U/xiaozhi/ota/)"
check /v1/content/packages/ 404 "$(code $U/v1/content/packages/)"   # app answers 404 for an unknown package
login=$(code -X POST -H 'Content-Type: application/json' -d '{}' $U/v1/parent/login)
case "$login" in 400|401|422) echo "ok   POST /v1/parent/login ($login, rejected by the app)" ;; *) echo "FAIL POST /v1/parent/login: $login"; fail=1 ;; esac
echo "== N5 websocket upgrade"
ws=$(c -i -m 4 $U/xiaozhi/v1/ -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
    -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' | head -1 | tr -d '\r' | cut -d' ' -f2)
check "wss upgrade" 101 "$ws"
echo "== N6 not routed from outside"
for p in /v1/voice/x /v1/voice/turn /play /docs /openapi.json /redoc /mcp/vision/explain /admin /xiaozhi/admin /v1 / \
         '/parent/..%2fv1/voice/x' '//v1/voice/x' '/v1/devices/../voice/x'; do
    check "$p" 404 "$(code --path-as-is "$U$p")"
done
echo "== N7 body limit"
check "70 KB body" 413 "$(head -c 70000 /dev/zero | code -X POST --data-binary @- $U/v1/pairing/x)"
echo "== N9 HSTS"
hsts=$(c -I $U/healthz | tr -d '\r' | grep -i '^strict-transport-security' | head -1)
case "$hsts" in *max-age=*) echo "ok   $hsts" ;; *) echo "FAIL no HSTS header"; fail=1 ;; esac
echo "== N2 TLS"
if openssl s_client -tls1_1 -connect "$IP:$PORT" -servername "$HOST" </dev/null >/dev/null 2>&1; then
    echo "FAIL TLS 1.1 accepted"; fail=1; else echo "ok   TLS 1.1 refused"; fi
if curl -s -m 5 --noproxy '*' -k -o /dev/null "https://$IP:$PORT/healthz"; then
    echo "FAIL certificate served for a bare-IP request"; fail=1; else echo "ok   bare-IP handshake rejected"; fi
echo "== N2 certificate in service (a failed pull/reload shows up here)"
if echo | openssl s_client -connect "$IP:$PORT" -servername "$HOST" 2>/dev/null | openssl x509 -noout -checkend 1209600 >/dev/null; then
    echo "ok   served certificate valid for at least 14 more days"; else echo "FAIL served certificate expires within 14 days"; fail=1; fi
echo "== N8 rate limit (last: it trips the limiter for this client for a minute)"
seen=""
for i in $(seq 60); do seen="$seen $(code $U/v1/pairing/x)"; done
case "$seen" in *429*) echo "ok   429 seen on /v1/pairing/" ;; *) echo "FAIL no 429 after 60 requests"; fail=1 ;; esac
exit $fail
