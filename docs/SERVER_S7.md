# 服务端 S7：应用部署与运维

对应 `V5_V6_SERVER_PLAN.md` §3.1–3.3、§8、§11.1 S7。本文是运维手册和验收清单；这台 PVE 机器上的实际部署记录见 [DEPLOY_PET_SERVER.md](DEPLOY_PET_SERVER.md)。**DNS、证书、端口转发、公网反向代理由用户实施**，这里只给需求与自查命令。

## 1. 组成

`deploy/compose.yaml`（在 `deploy/` 目录执行）：

| 服务 | 说明 |
|---|---|
| `pet-learning` | `services/pet_learning`，容器内 `:8765`，只读根文件系统、非 root、数据卷 `/data`（SQLite + `packages/`） |
| `xiaozhi-server` | `xinnan-tech/xiaozhi-esp32-server` 按 `version-lock.json` 的 commit 构建，叠加儿童守卫 overlay。与 pet-learning **共用网络命名空间**，所以守卫调用的 `127.0.0.1:8765` 仍成立（S5 把守卫白名单改成配置项后可拆开） |
| `gateway` | nginx，**唯一发布的端口**（`PET_GATEWAY_BIND:PET_GATEWAY_PORT`，默认 `127.0.0.1:8780`），按路径分流，重复一份 N6 路径白名单与 N7 体积上限 |

路由：`/v1/devices/ /v1/pairing/ /parent/ /v1/parent/ /v1/content/packages/` → PetLearning；`/xiaozhi/v1/`（WSS）和 `/xiaozhi/ota/` → 小智；其余 404（含 `/v1/voice/`、`/play`、`/docs`、`/openapi.json`、小智视觉/管理接口）。

转发头：`.env` 的 `KID_PET_TRUSTED_PROXY` 是**你的入口**的 IP，只有来自它的 `X-Forwarded-For` 会被 gateway 采信，并改写成单个客户端地址交给应用；应用侧的受信代理固定为 gateway（172.28.0.2）。

## 2. 首次部署

```sh
cd deploy
cp .env.example .env && chmod 600 .env        # 填密钥；提供商设置见 providers.env.example
python3 -m tools.pet_learning_admin check-config --env-file .env   # 在仓库根目录运行，必须输出 CHECK_OK
docker compose --env-file .env up -d --build
docker compose ps                              # 三个服务都应为 healthy
```

构建只依赖 GitHub、阿里云 PyPI 镜像和 Docker 镜像加速器（国内网络访问不了 Docker Hub 时在 `/etc/docker/daemon.json` 配 `registry-mirrors`）。

创建家长账号和绑定设备（在 pet-learning 容器内、本地终端执行，密码/令牌不进命令行参数）：

```sh
docker compose exec -it pet-learning python -m tools.pet_learning_admin --db /data/pet_learning.sqlite3 create-parent --username parent
```

小智服务端启动时由 `deploy/xiaozhi/entrypoint.py` 生成 `data/.config.yaml`，并用 `xz_checklist` 自检；任何一项不符就拒绝启动（见 §4）。

## 3. 备份、恢复、清理

`KID_PET_BACKUP_KEY`（Fernet 密钥，须与 `KID_PET_TOTP_KEY` 不同，单独离线保管；丢了就无法恢复备份）。

```sh
# 备份：SQLite 在线备份（sqlite3.backup）+ packages 目录 → 分块加密归档，不覆盖已有文件
docker compose exec pet-learning python -m tools.pet_learning_admin --db /data/pet_learning.sqlite3 backup --out /backups/pet-$(date +%F).enc
# 恢复：目标已存在时必须 --force，旧文件改名为 *.pre-restore-<时间>；先校验完整性和 schema 版本
docker compose stop xiaozhi-server pet-learning
docker compose run --rm --no-deps pet-learning python -m tools.pet_learning_admin --db /data/pet_learning.sqlite3 restore --archive /backups/pet-YYYY-MM-DD.enc --force
docker compose up -d
# 保留期清理（建议每天一次，宿主 cron 调用）
docker compose exec pet-learning python -m tools.pet_learning_admin --db /data/pet_learning.sqlite3 cleanup
```

清理按 §8 D6：`audit_log` 365 天、`commands` 已完成 30 天、`moderation_stats` 90 天，外加过期会话和 1 天前的登录尝试。记忆相关表不在清理清单内，测试断言其不被触碰。`commands`、`moderation_stats` 的表和列由 S2/S6 创建，表不存在时跳过（列名以那两个阶段的迁移为准，届时核对）。备份卷 `pet-backups` 在 Docker 数据盘上，请再用 PVE 备份/`docker cp` 把归档拿到另一块存储。

## 4. 小智儿童模式与设备鉴权清单（对 v0.9.8 / `8f2dda09`）

由 `xz_checklist.check_xiaozhi_config` 实现，容器启动时强制执行；`python -m tools.pet_learning_admin check-xiaozhi <json>` 可离线检查。

| # | 配置键 | 要求 | 原因 |
|---|---|---|---|
| X1 | `server.auth.enabled` | `true` | 设备鉴权必开（§3.2）；`websocket_server.py` 与 `ota_handler.py` 据此校验 token |
| X2 | `server.auth.allowed_devices` | 空 | 白名单设备**跳过** token 校验，默认示例 `11:22:33:44:55:66` 必须清掉 |
| X3 | `server.auth_key` | ≥32 位私密随机值 | token 的 HMAC 密钥；留空时上游会自动生成，不可复现，所以显式配置（`XZ_AUTH_KEY`） |
| X4 | `server.mqtt_gateway` / `udp_gateway` | 未设置 | 不走 MQTT/UDP 网关，只有 WSS |
| X5 | `server.websocket` | `wss://` 且与 `KID_PET_PUBLIC_ORIGIN` 同主机端口 | OTA 向设备下发的语音地址 |
| X6 | `read_config_from_api`、`manager-api.url` | `false` / 未设置 | 聊天记录、音频只通过 manager-api 上报（`reportHandle.py`），不接管理端就没有这条路 |
| X7 | `delete_audio` | `true` | 不保留音频文件 |
| X8 | `selected_module.Memory` | `nomem` | 关闭小智记忆 |
| X9 | `selected_module.Intent` | `nointent` | 意图交给守卫和 PetLearning，不开小智的音乐/IoT 工具 |
| X10 | `mcp_endpoint` | 未设置（保持上游占位符） | 不接外部 MCP |
| X11 | `KID_PET_CHILD_MODE=1` + 设备 id/token | 必填 | 守卫在儿童模式吞掉每一句 ASR，绝不落到普通 LLM 路径 |

另：`enable_greeting: false`（不生成问候语）。

**已知问题（pin 不一致）**：`scripts/p3_server_overlay.py` 的 `PINNED_SERVER_COMMIT` 是 `f1965167…`，而 `version-lock.json` 是 v0.9.8 的 `8f2dda09…`，二者不是祖先关系，直接用该脚本在锁定 checkout 上会拒绝执行。部署用 `deploy/apply_overlay.py` 复用其 `overlay()` 函数（锚点检查仍有效，已在 v0.9.8 上验证锚点唯一），commit 由 Dockerfile 校验。建议另行把脚本的常量改为读 `version-lock.json`（本阶段不碰该文件，S5 在改它）。

## 5. §3.3 公网入口验收清单（N1–N12，用户实施后逐条勾选）

把 `H` 换成你的主机名（含端口，如 `pet.example.com:8443`），`VM` 换成 VM 的内网地址（如 `192.168.1.110:8780`）。外网一遍、局域网一遍。

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
