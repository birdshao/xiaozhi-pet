# 服务端 S7：应用部署与运维

对应 `V5_V6_SERVER_PLAN.md` §3.1–3.3、§8、§11.1 S7。本文是运维手册和验收清单；这台 PVE 机器上的实际部署记录见 [DEPLOY_PET_SERVER.md](DEPLOY_PET_SERVER.md)。公网入口（TLS 反向代理、acme.sh 证书、DDNS、防火墙）已在 `deploy/ingress/` 实现并部署到 CT 108，见 [../deploy/ingress/README.md](../deploy/ingress/README.md)；**只有光猫端口转发、DNSPod 令牌和外网连通验证需要用户操作**。

## 1. 组成

`deploy/compose.yaml`（在 `deploy/` 目录执行）：

| 服务 | 说明 |
|---|---|
| `pet-learning` | `services/pet_learning`，容器内 `:8765`，只读根文件系统、非 root、数据卷 `/data`（SQLite + `packages/`） |
| `xiaozhi-server` | `xinnan-tech/xiaozhi-esp32-server` 按 `version-lock.json` 的 commit 构建，叠加儿童守卫 overlay。固定地址 172.28.0.20；守卫经字面 IP `http://172.28.0.10:8765` 调 pet-learning 的 `/v1/voice/{id}/turn`，用 `KID_PET_VOICE_TOKEN` 验响应签名；pet-learning 的 `KID_PET_VOICE_ALLOWED_PEERS` 只放 172.28.0.20 |
| `gateway` | nginx，**唯一发布的端口**（`PET_GATEWAY_BIND:PET_GATEWAY_PORT`，默认 `127.0.0.1:8780`），按路径分流，重复一份 N6 路径白名单与 N7 体积上限 |

路由：`/v1/devices/ /v1/pairing/ /parent/ /v1/parent/ /v1/content/packages/` → PetLearning；`/xiaozhi/v1/`（WSS）和 `/xiaozhi/ota/` → 小智；其余 404（含 `/v1/voice/`、`/play`、`/docs`、`/openapi.json`、小智视觉/管理接口）。

转发头：`.env` 的 `KID_PET_TRUSTED_PROXY` 是**你的入口**的 IP（单个 IP，不能是回环或网段；入口是 compose 自带的 `ingress` 服务时填它的固定地址 `172.28.0.3`；入口在宿主机上、经回环发布的端口访问 gateway 时才填 `172.28.0.1`，因为那种情况下是从 Docker 网桥网关到达 nginx 的），只有来自它的 `X-Forwarded-For` 会被 gateway 采信，并改写成单个客户端地址交给应用；应用侧的受信代理固定为 gateway（172.28.0.2）。

## 2. 首次部署

```sh
# 在仓库根目录：
cp deploy/.env.example deploy/.env && chmod 600 deploy/.env     # 填密钥；提供商设置见 providers.env.example
python3 -m tools.pet_learning_admin check-config --env-file deploy/.env   # 必须输出 CHECK_OK（权限过宽会先给 WARN）
# 宿主没装 Python 依赖时，用镜像检查（先 docker compose build pet-learning）：
#   docker run --rm --user 0 --network none -v "$PWD/deploy:/cfg:ro" pet-learning:s7 python -m tools.pet_learning_admin check-config --env-file /cfg/.env
cd deploy
docker compose --env-file .env up -d --build
docker compose ps                              # 三个服务都应为 healthy
```

**设备在局域网、公网入口还没做时**：在 `.env` 里设 `XZ_WEBSOCKET_URL=ws://<VM 内网 IP>:8780/xiaozhi/v1/`，重建小智容器（`docker compose --env-file .env up -d --build xiaozhi-server`）。公网入口就绪后把它清空，并把 `KID_PET_PUBLIC_ORIGIN` 改成真实 https 域名。`check-config` 在 origin 还是占位域名时打印 WARN；若设备将被告知的 websocket 地址仍是占位域名（既没设覆盖，origin 又是占位），直接 FAIL。验证：

```sh
curl -s -X POST http://<VM 内网 IP>:8780/xiaozhi/ota/ -H 'Device-Id: aa:bb:cc:dd:ee:ff' -H 'Client-Id: test' -H 'Content-Type: application/json' -d '{}'   # websocket.url 应为上面的地址
```

构建只依赖 GitHub、阿里云 PyPI 镜像和 Docker 镜像加速器（国内网络访问不了 Docker Hub 时在 `/etc/docker/daemon.json` 配 `registry-mirrors`）。

创建家长账号和绑定设备（在 pet-learning 容器内、本地终端执行，密码/令牌不进命令行参数）：

```sh
docker compose exec -it pet-learning python -m tools.pet_learning_admin --db /data/pet_learning.sqlite3 create-parent --username parent
```

小智服务端启动时由 `deploy/xiaozhi/entrypoint.py` 生成 `data/.config.yaml`，并用 `xz_checklist` 自检；任何一项不符就拒绝启动（见 §4）。

## 3. 备份、恢复、清理

`KID_PET_BACKUP_KEY`（Fernet 密钥，须与 `KID_PET_TOTP_KEY` 不同，单独离线保管；丢了就无法恢复备份）。另有 `KID_PET_NOTES_KEY`（S3，家长备注加密密钥，须与前两个都不同，生产缺失拒绝启动，见 [SERVER_S3](SERVER_S3.md)）。

```sh
# 备份（一次性容器，只有它拿到 KID_PET_BACKUP_KEY）：SQLite 在线备份 + packages → 分块加密归档 pet-<时间>.enc，保留最新 14 份
docker compose --env-file .env --profile ops run --rm --no-deps backup
# 恢复：目标已存在时必须 --force，旧文件改名为 *.pre-restore-<时间>；先校验完整性和 schema 版本
docker compose stop xiaozhi-server pet-learning
docker compose --env-file .env run --rm --no-deps -e KID_PET_BACKUP_KEY="$KID_PET_BACKUP_KEY" pet-learning   python -m tools.pet_learning_admin --db /data/pet_learning.sqlite3 restore --archive /backups/pet-YYYYMMDD-HHMMSS.enc --force
docker compose --env-file .env up -d
# 保留期清理（一次性容器）
docker compose --env-file .env --profile ops run --rm --no-deps cleanup
```

**定时执行**（宿主上的 systemd，每天 03:30，备份 + 清理）：

```sh
cp deploy/host/pet-maintenance.service deploy/host/pet-maintenance.timer /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now pet-maintenance.timer
systemctl list-timers pet-maintenance.timer        # 查看下次运行
systemctl start pet-maintenance.service && journalctl -u pet-maintenance -n 20   # 手动验证一次
```

单元里的路径是 `/opt/pet/app/deploy`，部署在别处请改 `WorkingDirectory`。

清理按 §8 D6：`audit_log` 365 天、`commands` 已完成 30 天、`moderation_stats` 90 天，外加过期会话和 1 天前的登录尝试，以及 14 天前由 restore 挪开的 `*.pre-restore-*` 副本。`parent_login_ips` 没有时间列，无法按保留期清理（需要 S1 加列，列为后续）。记忆相关表不在清理清单内，测试断言其不被触碰。`commands`、`moderation_stats` 的表和列由 S2/S6 创建，表不存在时跳过（列名以那两个阶段的迁移为准，届时核对）。备份卷 `pet-backups` 在 Docker 数据盘上，请再用 PVE 备份/`docker cp` 把归档拿到另一块存储。

## 4. 小智儿童模式与设备鉴权清单（对 v0.9.8 / `8f2dda09`）

由 `xz_checklist.check_xiaozhi_config` 实现，容器启动时强制执行；`python -m tools.pet_learning_admin check-xiaozhi <json>` 可离线检查。

| # | 配置键 | 要求 | 原因 |
|---|---|---|---|
| X1 | `server.auth.enabled` | `true` | WebSocket 要求签名 token；**但在锁定的 commit，`ota_handler.py` 会给任何调用方签发 token，设备 id 头可任填**，所以这不是“只有配对设备能连”，只能挡住绕过 OTA 直连 WSS 的人 |
| X2 | `server.auth.allowed_devices` | 空 | 白名单设备**跳过** token 校验，默认示例 `11:22:33:44:55:66` 必须清掉。真正的“只处理自己的板子”靠守卫：`conn.device_id` 必须等于 `KID_PET_UPSTREAM_DEVICE_ID`，其余连接只得到固定提示句 |
| X3 | `server.auth_key` | ≥32 位私密随机值 | token 的 HMAC 密钥；留空时上游会自动生成，不可复现，所以显式配置（`XZ_AUTH_KEY`） |
| X4 | `server.mqtt_gateway` / `udp_gateway` | 未设置 | 不走 MQTT/UDP 网关，只有 WSS |
| X5 | `server.websocket` | 默认 `wss://` 且与 `KID_PET_PUBLIC_ORIGIN` 同主机端口；局域网调试可用 `.env` 的 `XZ_WEBSOCKET_URL` 覆盖为 `ws://<VM 内网 IP>:8780/xiaozhi/v1/`（`ws://` 只接受字面私有 IP）。占位域名（example.com/.test/localhost 等）一律拒绝 | OTA 向设备下发的语音地址，设备在局域网时不能是占位域名 |
| X6 | `read_config_from_api`、`manager-api.url` | `false` / 未设置 | 聊天记录、音频只通过 manager-api 上报（`reportHandle.py`），不接管理端就没有这条路 |
| X7 | `delete_audio` | `true` | 不保留音频文件 |
| X8 | `selected_module.Memory` | `nomem` | 关闭小智记忆 |
| X9 | `selected_module.Intent` | `nointent` | 意图交给守卫和 PetLearning，不开小智的音乐/IoT 工具 |
| X10 | `mcp_endpoint` | 未设置（保持上游占位符） | 不接外部 MCP |
| X11 | `KID_PET_CHILD_MODE=1` + 设备 id + `KID_PET_VOICE_TOKEN` | 必填 | 守卫在儿童模式吞掉每一句 ASR，绝不落到普通 LLM 路径 |

**已知风险（公网入口，D14）**：任何人能从公网拿到 token 并发起 WSS 会话，消耗本机 ASR/TTS 算力（语音内容仍被守卫吞掉，不会进 LLM）。因此入口**必须**对 `/xiaozhi/ota/` 和 `/xiaozhi/v1/` 按来源 IP 限流（本 gateway 已加 `limit_req`：OTA 20 次/分、WSS 30 次/分，突发 10，超限 429；入口那一层也要加，N8）。

另：`enable_greeting: false`（不生成问候语）。

**pin**：S5（#91）已把 `scripts/p3_server_overlay.py` 的 `PINNED_SERVER_COMMIT` 改为 v0.9.8 的 `8f2dda09…`，与 `version-lock.json` 一致；`deploy/apply_overlay.py` 复用其 `overlay()`，Dockerfile 再校验 commit。

## 5. §3.3 公网入口验收清单（N1–N12）

入口实现在 `deploy/ingress/`（nginx 容器 `ingress`，监听 8443；acme.sh + DNSPod DNS-01；DDNS timer）。`deploy/ingress/verify.sh [IP] [-k]` 一次跑完 N2/N5–N9 的大部分检查。

**实测状态**（2026-11-07，CT 108；局域网 `curl --resolve` 到 192.168.1.110:8443，证书仍是占位自签）：

| # | 状态 | 说明 |
|---|---|---|
| N1 | 待办 | 需要用户在局域网 DNS 把 `pet.birdshao.cn` 指到 192.168.1.110；两处指纹一致要等正式证书 |
| N2 | 部分 | TLS 1.1 被拒、仅 TLS1.2/1.3 已验证；公信 CA、剩余有效期要等令牌签发正式证书 |
| N3 | 部分 | 8780/8000/8003/8765 不对公网：只发布 8443，`DOCKER-USER` 仅对 8443 开例外；外网 nmap 要等光猫转发后由用户验证 |
| N4 | 已验证（配置） | 入口、gateway 两层都把 `X-Forwarded-For` 改写成单个 `$remote_addr`；`KID_PET_TRUSTED_PROXY=172.28.0.3`；入口日志里的客户端地址是真实来源地址。登录审计里的 IP 因没有家长账号未实测 |
| N5 | 已验证 | WSS 升级得 101；空闲 ≥120 s 只配了 `proxy_read_timeout 300s`，未用设备长连实测 |
| N6 | 已验证 | `/v1/voice/*`、`/play`、`/docs`、`/openapi.json`、`/redoc`、`/mcp/…`、`/admin`、`..%2f` 与 `//` 变体全部 404 |
| N7 | 部分 | 70 KB POST → 413 已验证；64 MiB 上传与 `Range` 206 未测（需要内容包） |
| N8 | 已验证 | 对 `/v1/pairing/` 连发 60 次出现 429；入口和 gateway 两层都限流 |
| N9 | 已验证 | `Strict-Transport-Security: max-age=31536000` |
| N10 | 已验证 | ufw 只有 LAN 的 22、8780 和任意来源的 8443；`DOCKER-USER` 规则同步 |
| N11 | 已验证（配置） | `log_format` 只有地址、时间、方法、路径（无查询串）、状态、字节；logrotate 14 天 |
| N12 | 待办 | 要等外网连通确认后改 `KID_PET_PUBLIC_ORIGIN` 并 `check-config` |

把 `H` 换成你的主机名（含端口，如 `pet.birdshao.cn:8443`），`VM` 换成 VM 的内网地址（如 `192.168.1.110:8780`）。外网一遍、局域网一遍。

| # | 验收 | 自查命令 |
|---|---|---|
| N1 | 局域网和外网用同一主机名、同一证书；局域网直接解析到入口 | `getent hosts H` 在局域网内应是内网地址；`openssl s_client -connect H -servername H </dev/null 2>/dev/null \| openssl x509 -noout -fingerprint -sha256` 两处指纹一致 |
| N2 | 公信 CA、主机名匹配、未过期、TLS ≥1.2 | `openssl s_client -connect H -servername H -verify_return_error </dev/null 2>&1 \| grep -E "Verify return code\|Protocol"`（应为 `0 (ok)`）；`openssl x509 -checkend 1209600 -noout`（剩余 ≥14 天返回 0）；`openssl s_client -tls1_1 -connect H </dev/null` 应失败 |
| N3 | 应用端口不直接暴露到外网 | 在外网机器上 `curl -m 5 http://公网IP:8780/healthz` 应超时/拒绝；`nmap -p 8780,8000,8003,8765 公网IP` 应全部 closed |
| N4 | 覆盖客户端自带的转发头 | `curl -s https://H/v1/devices/x/ -H 'X-Forwarded-For: 1.2.3.4'`，查看 VM 上审计里的 IP 不是 1.2.3.4（登录失败一次后查 `audit_log.ip`） |
| N5 | WebSocket 升级且空闲 ≥120 s | `curl -si -m 5 https://H/xiaozhi/v1/ -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=='`（应为 `101` 或小智的握手响应，不是 400/404/502）；空闲测试用设备或 `websocat` 保持 130 s |
| N6 | 只转发白名单 | 允许：`curl -so /dev/null -w '%{http_code}\n' https://H/healthz`（若入口放行）、`/xiaozhi/ota/`；必须 404：`for p in /v1/voice/x /play /docs /openapi.json /redoc /mcp/vision/explain /admin; do curl -so /dev/null -w "$p %{http_code}\n" https://H$p; done` |
| N7 | 体积上限与 Range | `head -c 70000 /dev/zero \| curl -s -o /dev/null -w '%{http_code}\n' -X POST --data-binary @- https://H/v1/pairing/x`（应为 413）；`curl -sI -H 'Range: bytes=0-9' https://H/v1/content/packages/<sha>` 应为 206 |
| N8 | 按 IP 限流 | `for i in $(seq 40); do curl -so /dev/null -w '%{http_code} ' https://H/v1/pairing/x; done` 后段出现 429 |
| N9 | HSTS ≥180 天，不缓存 | `curl -sI https://H/ \| grep -i strict-transport-security`（`max-age=15552000` 以上）；`curl -sI https://H/v1/parent/login` 无 `Cache-Control: public` |
| N10 | VM 防火墙 | 在 VM 上 `ufw status`（只有 LAN 的 22 和 8780，或你改过的值）；LAN 外机器 `ssh` 应超时 |
| N11 | 入口日志不含请求体和 `Authorization`/`Cookie`，保留 ≤14 天 | 检查入口配置里的 `log_format` 和 logrotate；`grep -ri authorization 入口日志` 应为空 |
| N12 | `KID_PET_PUBLIC_ORIGIN` 为最终地址并验收 | `python3 -m tools.pet_learning_admin check-config --env-file deploy/.env` 输出 `CHECK_OK`；重启后配对页/面板链接用该地址 |

VM 上的内部自查（不经入口）：

```sh
curl -s http://VM/healthz                                         # {"status":"ok"}
curl -so /dev/null -w '%{http_code}\n' http://VM/v1/voice/x         # 404（gateway 不路由）
curl -so /dev/null -w '%{http_code}\n' http://VM/xiaozhi/ota/       # 200，小智 OTA
curl -s -X POST http://VM/v1/parent/login -H 'content-type: application/json' -d '{}'   # 422 INVALID_PARENT_REQUEST
```

## 6. 提供商（用户决定）

小智的 ASR/LLM/TTS 由 `.env` 的 `XZ_ASR_MODULE`、`XZ_LLM_MODULE`、`XZ_TTS_MODULE` 选择上游 `config.yaml` 里的模块名，密钥与细项放 `deploy/providers.env`（`XZ__<段>__<模块>__<键>=值`，仅小智容器可见）。儿童模式下守卫吞掉所有 ASR 文本，LLM 在 S6 之前不会被普通对话路径调用；ASR 和 TTS 必须可用才能真正说话。**这些密钥由用户自己填入 VM，不经过 Claude。**
