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

CT 内 ufw：默认拒绝入站；只允许 `192.168.1.0/24` 访问 22 和 8780。Docker 发布的端口绕过 ufw 的 INPUT 链，所以在 `/etc/ufw/after.rules` 里加了 `DOCKER-USER` 规则：来自 eth0 且不在 192.168.1.0/24 的流量一律丢弃。公网入口（端口转发、反向代理）不在本机配置，由用户实施。

## 3. 应用与提供商

- 三个服务：`pet-learning`、`xiaozhi-server`（commit `8f2dda09…`，v0.9.8，叠加儿童守卫）、`gateway`（nginx）。
- Docker Hub 在这个网络不可达：`/etc/docker/daemon.json` 配了 `docker.m.daocloud.io`、`docker.1ms.run` 两个镜像加速器；PyPI 用阿里云镜像，GitHub 可直连。
- 提供商（协调会话决定，零成本、儿童录音不出本机）：ASR `SherpaParaformerASR`（本地，`sherpa-onnx-paraformer-zh-small-2024-03-09`，约 74 MB 下载 / 80 MB 解压，`deploy/fetch_models.sh` 校验 SHA-256）；TTS `EdgeTTS`（只把回复文本发给微软）；LLM `ChatGLMLLM`，**未配 key**，S6 之前守卫接管每一句话，用不上。
- 实测内存与冒烟结果见下方「验证」。

## 4. 验证

（部署完成后填写。）

## 5. 待办与未决项

- **pin 不一致（未决，本 PR 不改）**：`scripts/p3_server_overlay.py` 的 `PINNED_SERVER_COMMIT = f1965167…` 与 `version-lock.json`（v0.9.8 = `8f2dda09…`）不是祖先关系，该脚本在锁定 checkout 上会拒绝。部署用 `deploy/apply_overlay.py` 复用其 `overlay()`，commit 由 Dockerfile 校验。S5（PR #91）正在改该脚本，请在那边统一为读取 `version-lock.json`。
- 公网接入（DNS、公信证书、端口转发/IPv6、反向代理、限流、HSTS）由用户按 SERVER_S7.md §5 实施并逐条验收。
- `.env` 里的 `KID_PET_PUBLIC_ORIGIN`、`KID_PET_TRUSTED_PROXY`、设备 id/令牌目前是占位值，配对真实设备和确定公网地址后要改，并重新执行 `check-config`。
- 家长账号尚未创建（需要用户在本地终端设置密码并绑定 TOTP，见 SERVER_S7.md §2）。
- 备份：`pet-backups` 卷只是同机副本，请在 PVE 上对 CT 108 做 `vzdump`，并把加密归档拷到另一块存储；`KID_PET_BACKUP_KEY` 单独离线保管。
