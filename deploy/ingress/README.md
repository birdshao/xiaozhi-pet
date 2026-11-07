# 公网 HTTPS 入口（`https://pet.birdshao.cn:8443`）

CT 108 上的 TLS 入口：nginx 容器（compose 服务 `ingress`，profile `ingress`，固定地址 172.28.0.3）终止 TLS，按白名单转发到内部 gateway（172.28.0.2:8780）。**DDNS 和证书由局域网里的 Lucky 管**（A 记录同步、`*.birdshao.cn` 泛域名证书的签发与续期），CT 108 每天从 Lucky 拉一次导出的证书。验收对照 [../../docs/SERVER_S7.md](../../docs/SERVER_S7.md) §5 N1–N12。不含任何密钥。

```
手机/设备 ──TCP 8443──> 中兴光猫(端口转发) ──> CT 108:8443 [ingress: TLS、白名单、限流、HSTS]
                                                       └─> gateway 172.28.0.2:8780 ─> pet-learning / xiaozhi
Lucky(192.168.1.224): DDNS(pet.birdshao.cn A)  +  ACME 泛域名证书 ──(每日 SSH 拉取)──> CT 108 /etc/pet-ingress/certs
```

| 文件 | 作用 |
|---|---|
| `../nginx/ingress.conf.template` | 入口配置：TLS≥1.2、HSTS、路径白名单（`/v1/voice/*` 等一律 404）、按 IP 限流、日志不含查询串/Authorization/Cookie |
| `install.sh` | CT 上一次性安装（幂等）：目录、专用拉取密钥、systemd 单元、logrotate（14 天）、占位自签证书、防火墙 |
| `firewall.sh` | ufw 放行 8443；`DOCKER-USER` 对 8443 开例外（Docker 发布的端口不走 ufw INPUT） |
| `pull-cert.sh` + `pet-cert-pull.{service,timer}` | 每天 04:10 左右从 Lucky 拉证书；校验主机名、剩余有效期 ≥7 天、含中间证书、私钥匹配，通过且有变化才替换并 reload |
| `lucky-export.sh` | 装在 Lucky 容器里的 SSH 强制命令：只输出指定证书的 crt/key |
| `reload-ingress.sh` | `nginx -t` 后 HUP 入口容器 |
| `verify.sh` | 局域网/外网验收：路由白名单、voice 被拒、WSS、413、HSTS、TLS 版本、限流 |
| `lucky.env.example` | 拉取配置（非密钥） |

## 部署（CT 108，root）

```bash
cd /opt/pet/app/deploy
# .env: KID_PET_TRUSTED_PROXY=172.28.0.3、COMPOSE_PROFILES=ingress、PET_INGRESS_HOST=pet.birdshao.cn（见 .env.example）
sh ingress/install.sh            # 末尾会打印专用公钥
docker compose --env-file .env up -d gateway ingress
sh ingress/verify.sh 192.168.1.110 -k        # 拿到正式证书前用 -k（占位自签证书）
```

## 接 Lucky

1. **DDNS**：Lucky「动态域名」里已有的任务（腾讯云）加一条域名记录：`pet.birdshao.cn`，A，默认线路，只开 IPv4。
2. **证书**：Lucky「SSL/TLS证书」里的 `*.birdshao.cn` ACME 证书已覆盖 `pet.birdshao.cn`。在「路径映射」里确认导出文件路径（形如 `/goodluck/cert_export/<名称>.crt` 和 `.key`）。注意：泛域名证书的私钥会存在 CT 108 上，CT 108 被攻破等于这把私钥泄露；要隔离就在 Lucky 里单独签一张只含 `pet.birdshao.cn` 的证书。
3. **SSH 授权**：在 Lucky 所在主机在 Lucky 所在容器里装 `lucky-export.sh`（强制命令，只会输出那张证书的 crt 和 key），并把 `install.sh` 打印的公钥以 `restrict,command=` 形式加进 `authorized_keys`（`CERT_NAME` 换成 Lucky「路径映射」里的证书名）：

   ```bash
   install -m 755 lucky-export.sh /usr/local/bin/pet-cert-export    # 先把仓库里的这个文件拷到 Lucky 容器
   sed -i 's/^CERT=.*/CERT=CERT_NAME/' /usr/local/bin/pet-cert-export
   mkdir -p ~/.ssh && echo 'restrict,command="/usr/local/bin/pet-cert-export" ssh-ed25519 AAAA… pet-ingress cert pull' >> ~/.ssh/authorized_keys
   ```

4. **在 CT 108 上**写配置并固定主机密钥（先核对指纹与 Lucky 主机一致）：

   ```bash
   cp /opt/pet/app/deploy/ingress/lucky.env.example /etc/pet-ingress/lucky.env && nano /etc/pet-ingress/lucky.env
   ssh-keyscan -t ed25519 -p 22 192.168.1.224 > /etc/pet-ingress/lucky_known_hosts && ssh-keygen -lf /etc/pet-ingress/lucky_known_hosts
   sh /opt/pet/app/deploy/ingress/pull-cert.sh && systemctl enable --now pet-cert-pull.timer
   ```

## IPv6

入口目前只发布 IPv4（`0.0.0.0:8443`）：Docker 的 IPv6 端口发布会丢掉客户端地址，限流和审计都会失真；没人连得进的 AAAA 记录还会让设备卡在超时。所以 Lucky 里只同步 A 记录。

## 局域网

N1 要求局域网和外网用同一主机名。让家里的 DNS（路由器/光猫的 DNS 重写，或 AdGuard 等）把 `pet.birdshao.cn` 解析到 `192.168.1.110`，否则局域网内会走公网 IP 回环（光猫通常不支持）。

## 回滚

`docker compose --env-file .env stop ingress`；`ufw delete allow 8443/tcp`；删除 `/etc/ufw/after.rules` 里的 `--dport 8443 -j RETURN` 一行（备份文件 `after.rules.bak-*`）后 `ufw reload`；`systemctl disable --now pet-cert-pull.timer`；光猫上删掉 8443 转发。
