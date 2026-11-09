# pet-server 部署记录（用户 PVE）

更新时间：2026-11-07。运维手册与验收清单见 [SERVER_S7.md](SERVER_S7.md)，设计依据见 `V5_V6_SERVER_PLAN.md` §3。本文不含任何密钥。

## 与计划的偏差：LXC 容器，而不是 VM

§3.1 决定用专用 VM、不用 LXC。实际部署在 **LXC 容器 CT 108**，原因是用户明确要求不新建 VM、改用容器（2026-11-07）。影响和做法：

- 隔离比 VM 弱：容器共享宿主内核。缓解：**非特权容器**（`unprivileged: 1`），只开 `nesting=1,keyctl=1`（Docker 需要），不给 `fuse`、不挂载宿主目录、不开 `lxc.cgroup.devices.allow`/`lxc.cap.drop` 之类的放宽项；应用容器再加 `read_only`、`cap_drop: ALL`、`no-new-privileges`、非 root。
- 现有的 CT 103 `docker_template` 因主机名含下划线、`pct` 无法解析，没有用来克隆（也没有修改它）；CT 108 是用节点自带的 `debian-12-standard_12.12-1_amd64` 模板新建的。
- PVE 数据中心防火墙在该节点是关闭的，打开会影响其他客户机，所以**没有**使用 PVE 防火墙，而是在 CT 内用 ufw（见下）。
- 宿主机内存紧张（31.8 GB 中可用约 2.8 GB，swap 已满），CT 给 3 GB + 1 GB swap；本地 ASR 选了小模型（见下），实测数字见 §3。

## 1. 机器

| 项 | 值 |
|---|---|
| 类型 / ID / 主机名 | LXC / **108** / `pet-server` |
| 系统 | Debian 12，非特权，`nesting=1,keyctl=1`，开机自启 |
| 资源 | 2 核 / 3 GB 内存 + 1 GB swap / 32 GB 磁盘（`ssd-250g`） |
| 网络 | `vmbr0`，静态 `192.168.1.110/24`，网关 192.168.1.1，PVE 网卡 `firewall=1`（节点防火墙关闭，无效果） |
| 访问 | `ssh -i ~/.ssh/pet_server_ed25519 root@192.168.1.110`（仅局域网；密钥在部署者的机器上，不入库） |
| 应用目录 | `/opt/pet/app`（部署包）、`/opt/pet/app/deploy/.env`（600，本机生成的随机密钥，不在仓库） |
| 内部入口 | **`http://192.168.1.110:8780`**：把你的反向代理指向这里 |

## 2. 防火墙（N10）

CT 内 ufw：默认拒绝入站；只允许 `192.168.1.0/24` 访问 22 和 8780。Docker 发布的端口绕过 ufw 的 INPUT 链，所以在 `/etc/ufw/after.rules` 里加了 `DOCKER-USER` 规则：来自 eth0 且不在 192.168.1.0/24 的流量一律丢弃。公网入口（2026-11-07 加）：`ufw allow 8443/tcp`，并在 `DOCKER-USER` 里给 eth0 上目的端口 8443 的 TCP 开例外（`deploy/ingress/firewall.sh`，幂等）；其余端口规则不变，8780/8000/8003/8765 仍不对外。

## 2.1 公网 HTTPS 入口

`https://pet.birdshao.cn:8443`：compose 服务 `ingress`（nginx，固定地址 172.28.0.3，profile `ingress`，`.env` 里 `COMPOSE_PROFILES=ingress`）终止 TLS，转发到 gateway。`KID_PET_TRUSTED_PROXY=172.28.0.3`（入口是容器，不是 172.28.0.1）。DDNS 和证书由局域网里的 Lucky（192.168.1.224）管：Lucky 同步 A 记录、用腾讯云 DNS 验证签发并续期 `*.birdshao.cn` 泛域名证书；CT 108 的 `pet-cert-pull.timer` 每天通过 SSH 拉取 Lucky 导出的证书，校验后 reload 入口。安装、授权方式、回滚见 [deploy/ingress/README.md](../deploy/ingress/README.md)。

- 已落地：入口容器运行（healthy）、ufw/DOCKER-USER、从 Lucky（`nps`，192.168.1.224）拉到的 `*.birdshao.cn` 正式 Let's Encrypt 证书（到期 2027-01-20）与已启用的 `pet-cert-pull.timer`（每日，校验后才替换）、logrotate；局域网 `curl --resolve`（正式证书，不加 `-k`）验证了路由白名单、`/v1/voice/*` 等 404、WSS 101、413、HSTS、TLS1.1 被拒、429 限流（`verify.sh` 全部通过）。
- Lucky 里 `pet.birdshao.cn` 的 DDNS 记录已加（A → 223.166.24.116），拉取公钥已在 `nps` 授权（`restrict,command` 受限）。等用户：光猫转发 WAN TCP 8443 → 192.168.1.110:8443，外网验证；之后改 `KID_PET_PUBLIC_ORIGIN`、清空 `XZ_WEBSOCKET_URL`（这两项**尚未改**，板子的 OTA 仍下发局域网地址）。
- 升级（2026-11-07）：先备份（`pet-20261107-082642.enc`，恢复到临时目录验证 `RESTORE_OK`；`.env`、旧 compose 与旧应用目录副本在 CT 的 `/root/pet-upgrade-20261107/`），再把应用同步到最新 main 并重建 pet-learning、xiaozhi-server：数据库 `user_version` 5 → 6，生成 `KID_PET_NOTES_KEY`，`check-config` 为 `CHECK_OK`，OTA 仍下发局域网地址。

## 3. 应用与提供商

- 三个服务：`pet-learning`、`xiaozhi-server`（commit `8f2dda09…`，v0.9.8，叠加儿童守卫）、`gateway`（nginx）。
- Docker Hub 在这个网络不可达：`/etc/docker/daemon.json` 配了 `docker.m.daocloud.io`、`docker.1ms.run` 两个镜像加速器；PyPI 用阿里云镜像，GitHub 可直连。
- 提供商（协调会话决定，零成本、儿童录音不出本机）：ASR `SherpaParaformerASR`（本地，`sherpa-onnx-paraformer-zh-small-2024-03-09`，约 74 MB 下载 / 80 MB 解压，`deploy/fetch_models.sh` 校验 SHA-256）；TTS `EdgeTTS`（只把回复文本发给微软）；LLM `ChatGLMLLM`，**未配 key**，S6 之前守卫接管每一句话，用不上。
- **S6 开放问答（ADR-015）**：GLM 由 pet-learning 直接调用（`https://open.bigmodel.cn`，地址写死在代码里），不经过小智的 `ChatGLMLLM`；小智的 `chat()` 在儿童模式下被 overlay 关掉了。用户要在 `deploy/providers.env` 里加 `KID_PET_GLM_API_KEY=<key>`，可选再加 `KID_PET_GLM_MODEL=glm-4.7-flash`（这是默认值）。compose 现在让 pet-learning 也读 `providers.env`。没有密钥时，面板上的开放问答开关置灰。详见 [SERVER_S6.md](SERVER_S6.md) §5。pet-learning 容器要能出网访问 `open.bigmodel.cn:443`。
- 实测内存与冒烟结果见下方「验证」。

## 4. 验证

2026-11-07 在 CT 108 上实测（`docker compose up -d --build`，三个服务均 healthy）：

| 项 | 结果 |
|---|---|
| 内存（SherpaParaformerASR 已加载，空闲） | xiaozhi-server 719 MiB，pet-learning 52 MiB，gateway 3 MiB；CT 合计已用约 614 MB / 3072 MB（含缓存），装得下 |
| 磁盘 | xiaozhi-server 镜像 10.8 GB（上游依赖很重），pet-learning 237 MB；CT 磁盘 32 GB，注意 |
| `GET :8780/healthz` | `{"status":"ok"}` |
| `/v1/voice/x`、`/docs`、`/play` | 均 404（gateway 不路由） |
| `/xiaozhi/ota/` | 200 |
| `/xiaozhi/v1/` WebSocket 升级 | 101 Switching Protocols（经 gateway） |
| `POST /v1/parent/login` `{}` | `INVALID_PARENT_REQUEST` |
| 语音一轮（守卫 → pet-learning，真实 token 与 HMAC 签名，临时库里登记测试设备，用后销毁容器） | “团团睡觉”→ local_command，守卫回“用设备按键”；“现在几点了”→“现在是12点30分。”；开放问题→固定兜底句；空文本→“没听清”；设备 id 不符→“请让家长帮忙连接儿童模式。” |

评审后补充（同日）：`check-config` 在 CT 上用镜像运行输出 `CHECK_OK`；`pet-maintenance.timer` 已安装并手动触发一次，备份写出 `pet-<时间>.enc`（600 权限，只有备份任务容器拿到 `KID_PET_BACKUP_KEY`）、清理任务正常，下次运行 03:30 左右；gateway 对 `/xiaozhi/ota/` 限流生效（突发约 10 次后返回 429）。

没有验证的：真实音频经 ASR/TTS（EdgeTTS 需出网）的完整对话、真实设备连接、gateway 对语音路由的 403（只确认了坏 token 得 401）、外网（光猫转发后）能否连通入口。
上线时小坑：上游在导入时会创建 `config/assets/wakeup_words`，只读根文件系统下用 tmpfs 解决（见 compose）。

## 5. 待办与未决项

- pin 不一致已由 S5（#91）统一为 `8f2dda09…`，不再是未决项。
- **定时备份和清理**：`deploy/host/pet-maintenance.{service,timer}` 已入库，CT 108 上需按 SERVER_S7.md §3 安装并手动验证一次（见下方「验证」）。
- 公网接入：反向代理、限流、HSTS、防火墙、证书流程、DDNS 已就位（§2.1）；待用户光猫转发 8443、外网验证，逐条验收见 SERVER_S7.md §5 状态表。IPv6 未开（原因见 deploy/ingress/README.md）。局域网 DNS 要把 `pet.birdshao.cn` 指到 192.168.1.110（N1）。
- `.env` 里的 `KID_PET_PUBLIC_ORIGIN`、`KID_PET_TRUSTED_PROXY`（单个 IP；现已设为入口容器 172.28.0.3）、设备 id 目前是占位值（`KID_PET_VOICE_TOKEN` 是本机生成的随机值，两侧同值，只在 `.env`），配对真实设备和确定公网地址后要改，并重新执行 `check-config`。
- 家长账号尚未创建（需要用户在本地终端设置密码并绑定 TOTP，见 SERVER_S7.md §2）。
- 备份：`pet-backups` 卷只是同机副本，请在 PVE 上对 CT 108 做 `vzdump`，并把加密归档拷到另一块存储；`KID_PET_BACKUP_KEY` 单独离线保管。
