# 公网 HTTPS 入口（`https://pet.birdshao.cn:8443`）

CT 108 上的 TLS 入口：nginx 容器（compose 服务 `ingress`，profile `ingress`，固定地址 172.28.0.3）终止 TLS，按白名单转发到内部 gateway（172.28.0.2:8780）。证书用 acme.sh + DNSPod DNS-01（不需要 80 端口），DDNS 用 systemd timer。不含任何密钥。验收对照 [../../docs/SERVER_S7.md](../../docs/SERVER_S7.md) §5 N1–N12。

```
手机/设备 ──TCP 8443──> 中兴光猫(端口转发) ──> CT 108:8443 [ingress: TLS、白名单、限流、HSTS]
                                                       └─> gateway 172.28.0.2:8780 ─> pet-learning / xiaozhi
```

| 文件 | 作用 |
|---|---|
| `../nginx/ingress.conf.template` | 入口配置：TLS≥1.2、HSTS、路径白名单（`/v1/voice/*` 等一律 404）、按 IP 限流、日志不含查询串/Authorization/Cookie |
| `install.sh` | CT 上一次性安装（幂等）：目录、acme.sh、systemd 单元、logrotate（14 天）、占位自签证书、防火墙 |
| `firewall.sh` | ufw 放行 8443；`DOCKER-USER` 对 8443 开例外（Docker 发布的端口不走 ufw INPUT） |
| `issue-cert.sh` | 签发/强制续签并安装证书（`staging` 参数走 Let's Encrypt 测试 CA） |
| `acme-renew.sh` + `pet-acme-renew.{service,timer}` | 每天 03:17 左右检查，剩余 <30 天自动续期并 reload 入口 |
| `reload-ingress.sh` | `nginx -t` 后 HUP 入口容器 |
| `pet-ddns.py` + `pet-ddns.{service,timer}` | 每 5 分钟把 `pet.birdshao.cn` 的 A 记录同步到当前公网 IPv4 |
| `verify.sh` | 局域网/外网验收：路由白名单、voice 被拒、WSS、413、HSTS、TLS 版本、限流 |
| `dnspod.env.example` | 令牌文件格式（变量名） |

## 部署步骤（CT 108，root）

```bash
cd /opt/pet/app/deploy
# .env: KID_PET_TRUSTED_PROXY=172.28.0.3、COMPOSE_PROFILES=ingress、PET_INGRESS_HOST=pet.birdshao.cn（见 .env.example）
sh ingress/install.sh
docker compose --env-file .env up -d gateway ingress
sh ingress/verify.sh 192.168.1.110 -k        # 证书签发前用 -k（占位自签证书）
```

## DNSPod 令牌（用户自己放，不要贴到聊天里）

在 DNSPod 控制台（用户中心 → 密钥管理 → DNSPod Token）创建令牌后，在服务器上执行：

```bash
install -m 600 -o root -g root /dev/null /etc/pet-ingress/dnspod.env
nano /etc/pet-ingress/dnspod.env      # 写入两行：DP_Id=<数字 ID> 和 DP_Key=<Token>，保存
```

令牌放好后：

```bash
systemctl enable --now pet-ddns.timer && systemctl start pet-ddns.service && journalctl -u pet-ddns -n 5 --no-pager
sh /opt/pet/app/deploy/ingress/issue-cert.sh staging   # 可选：先用测试 CA 演练 DNS-01
sh /opt/pet/app/deploy/ingress/issue-cert.sh           # 正式证书；自动写入 /etc/pet-ingress/certs 并 reload
```

若账号已迁移到腾讯云 DNSPod（只有腾讯云 API 密钥），把 `issue-cert.sh` 里的 `dns_dp` 换成 `dns_tencent` 并在 env 里用 `Tencent_SecretId`/`Tencent_SecretKey`；`pet-ddns.py` 目前只支持上面的 DNSPod Token 接口。

## IPv6

入口目前只发布 IPv4（`0.0.0.0:8443`）：Docker 的 IPv6 端口发布会丢掉客户端地址，限流和审计都会失真。`pet-ddns.py` 支持 AAAA（`DDNS_IPV6=1`），但没有开启，因为没人能从 IPv6 连进来的 AAAA 会让设备卡在连接超时。

## 局域网

N1 要求局域网和外网用同一主机名。让家里的 DNS（路由器/光猫的 DNS 重写，或 AdGuard 等）把 `pet.birdshao.cn` 解析到 `192.168.1.110`，否则局域网内会走公网 IP 回环（光猫通常不支持）。

## 回滚

`docker compose --env-file .env stop ingress`；`ufw delete allow 8443/tcp`；删除 `/etc/ufw/after.rules` 里的 `--dport 8443 -j RETURN` 一行（备份文件 `after.rules.bak-*`）后 `ufw reload`；光猫上删掉 8443 转发。
