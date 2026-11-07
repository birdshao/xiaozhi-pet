#!/usr/bin/env python3
"""Keep a DNSPod A (and optionally AAAA) record equal to this network's public address.

Credentials come from the environment (DP_Id, DP_Key; /etc/pet-ingress/dnspod.env via systemd) and are
never printed. DDNS_DOMAIN / DDNS_SUBDOMAIN pick the record; DDNS_IPV6=1 also maintains AAAA (off by
default: the ingress is IPv4-only, and an unreachable AAAA would make clients stall).
"""
import ipaddress
import json
import os
import re
import sys
import urllib.parse
import urllib.request

API = "https://dnsapi.cn/"
V4_SOURCES = ["https://ip.3322.net", "https://myip.ipip.net/s", "https://4.ipw.cn", "https://api.ipify.org"]
V6_SOURCES = ["https://6.ipw.cn", "https://api64.ipify.org"]
_CANDIDATE = re.compile(r"[0-9A-Fa-f:.]{3,45}")


def parse_ip(text, version):
    """First public address of the wanted IP version found in `text`, else None."""
    for token in _CANDIDATE.findall(text):
        try:
            addr = ipaddress.ip_address(token.strip(":"))
        except ValueError:
            continue
        if addr.version == version and addr.is_global:
            return str(addr)
    return None


def fetch_public_ip(version, sources=None, opener=urllib.request.urlopen):
    """Address two sources agree on; the single answer if only one source is reachable; else None."""
    answers = []
    for url in sources or (V4_SOURCES if version == 4 else V6_SOURCES):
        try:
            with opener(url, timeout=8) as resp:
                ip = parse_ip(resp.read(512).decode("utf-8", "replace"), version)
        except Exception:
            continue
        if ip:
            if ip in answers:
                return ip
            answers.append(ip)
    return answers[0] if len(answers) == 1 else None


def plan(records, want):
    """('noop'|'create'|'modify', record_id) given the existing records of one type."""
    if not records:
        return "create", None
    first = records[0]
    return ("noop" if first["value"] == want else "modify"), first["id"]


def api_call(action, token, params, opener=urllib.request.urlopen):
    data = urllib.parse.urlencode({"login_token": token, "format": "json", "lang": "en", **params}).encode()
    req = urllib.request.Request(API + action, data=data, headers={"User-Agent": "pet-ddns/1 (pet-server)"})
    with opener(req, timeout=15) as resp:
        body = json.load(resp)
    status = body.get("status", {})
    code = str(status.get("code"))
    if code == "10":  # no records of that type yet
        return {"records": []}
    if code != "1":
        raise RuntimeError(f"{action}: DNSPod code {code}: {status.get('message')}")
    return body


def sync(token, domain, sub, rtype, want, opener=urllib.request.urlopen):
    body = api_call("Record.List", token, {"domain": domain, "sub_domain": sub, "record_type": rtype}, opener)
    records = [r for r in body.get("records", []) if r.get("name") == sub and r.get("type") == rtype]
    action, rid = plan(records, want)
    if action == "noop":
        return f"{rtype} {sub}.{domain} already {want}"
    common = {"domain": domain, "sub_domain": sub, "record_type": rtype, "record_line": "默认",
              "value": want, "ttl": "120"}
    if action == "create":
        api_call("Record.Create", token, common, opener)
    else:
        api_call("Record.Modify", token, {**common, "record_id": rid}, opener)
    return f"{rtype} {sub}.{domain} {action} -> {want}"


def main(env=os.environ):
    ident, key = env.get("DP_Id", ""), env.get("DP_Key", "")
    if not ident or not key:
        print("DP_Id/DP_Key not set", file=sys.stderr)
        return 1
    token = f"{ident},{key}"
    domain = env.get("DDNS_DOMAIN", "birdshao.cn")
    sub = env.get("DDNS_SUBDOMAIN", "pet")
    rc = 0
    wanted = [(4, "A")] + ([(6, "AAAA")] if env.get("DDNS_IPV6") == "1" else [])
    for version, rtype in wanted:
        ip = fetch_public_ip(version)
        if not ip:
            print(f"no public IPv{version} address found; {rtype} left unchanged", file=sys.stderr)
            rc = 1
            continue
        try:
            print(sync(token, domain, sub, rtype, ip))
        except Exception as exc:  # the message never contains the token
            print(f"{rtype} update failed: {exc}", file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
