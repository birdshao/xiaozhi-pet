# 设备端：服务器配对、状态快照与命令（V5_V6 §11.2）

对应 `V5_V6_SERVER_PLAN.md` §5.1–5.4、§11.2 前四项，服务端契约见 [SERVER_S1](SERVER_S1.md)、[SERVER_S2](SERVER_S2.md)。

## 任务与数据归属

| 任务 | 拥有 | 绝不做 |
|---|---|---|
| Pet worker（`kid_pet_atomic`） | `PetAtomicLedger`；执行命令（`PostCommand` → `ApplyServerCommand`，`command_id` 即账本事件 id） | 网络 |
| 网络任务（`kid_srv`，`FreertosServerSyncWorker`） | HTTP、NVS 命名空间 `kid_srv`（`dev_id`、`token`、`confirmed`、`rot_at`、`rejected`、可选 `origin`）、回执 / 在途命令簿、上传节奏、配对流程 | 碰账本；只读 `SyncViewNow()` 一次性拷贝（generation、宠物、记忆、家长设置出自同一次提交） |
| 输入任务（`pet_input`） | 家长菜单、`ConfirmDialog`、页面切换 | 直接提交；确认经 `ConfirmRequest` → Pet 任务，拒绝经事件队列 → 网络任务写 NVS |
| 共享 | `ParentRequestQueue`（`requests_mutex_`） | — |

纯逻辑全部可在主机测试：`json_lite`、`server_sync`（快照编码、回复解析、`RouteCommand`、`CommandTracker`、`PairingFlow`、`UploadSchedule`）、`parent_requests`、`pet_command_apply`。测试：`tests/cpp/test_server_sync.cc`（`scripts/p2_test.sh`）与 `tests/test_v5_server_device_sync_contract.py`（真实服务端接受同一批快照夹具，回复夹具由服务端生成）。

## 服务器地址

只用 **HTTPS + 公共 CA 证书**（证书包校验主机名，不跳过、不重定向，**没有**明文 http 开关，局域网也不行）。服务器因此需要一个域名和公共 CA 证书（D2：`birdshao.cn`）。

地址**单独配置**，不再从 OTA URL 推导（OTA 地址目前是 http 局域网地址）：NVS `kid_srv/origin`，没有时取 Kconfig `KID_PET_SERVER_ORIGIN`（默认空 = 未配置，配对页显示叉号，什么也不发）。配对成功时把 origin 存入 `kid_srv/bound`：token 只发往这个 origin，开机发现地址变了 → "需要重新连接"。

## 家长入口

- 菜单第 9 行（云朵）：右侧显示连接状态（✓ 已连接 / … 进行中 / 6 位码 / ✗ 失败 / 圈叉 需重新连接 / 虚线 未连接）。按下进入配对页；未连接或失败时自动开始配对；已连接时第一次按下只把图标变成"重新连接？"（圈叉，10 s 有效），第二次按下才重新配对（新的设备 id `tt-<MAC>-<随机>`）。新 token 先存 `pend_id/pend_tok`，**confirm 返回 200 之后**才原子替换 `dev_id/token`；配对中途失败、断电都不会丢掉现有凭据，开机时若发现未确认的 pend 会继续 confirm（只有 confirm 返回 401 才删除）。屏上显示配对码时页面不因空闲退出。
- 待确认请求（D5）：进入家长入口时先显示请求页（清空记忆 = 划掉的心；改性格 = 新性格的脸），默认停在 ✗；**拨轮往上 = ✓，往下 = ✗（按方向选，不是翻转）**；按下确认或拒绝，但高亮最近一次变化后要过 3 s（墨水屏刷新）才接受按下，防止决定了没看到的选项；长按"以后再说"。确认以 `MemorySource::Parent` 提交；拒绝先写 NVS `kid_srv/rejected` 再回执。首页状态栏左侧出现信封图标表示有请求在等。

## 上传与命令

- 联网后立即上传一次；账本提交、回执或待确认列表变化后去抖 30 s；新 generation 至少隔 10 s；失败 5 s 起指数退避到 5 min，429 遵守 `Retry-After`；30 min 心跳；没有上传时每 15 min `GET commands`；断电/睡眠前（`PowerOffForSleep`）最多等 6 s 再上传一次。
- 命令列表很长：节点池 768、回复缓冲 12 KB，一次最多取前 16 条；回复超限或不可读时按"已上传、无命令"处理，不重传快照。
- 回执一直随快照发送，直到 `acked_commands` 含它。413/422 不重发同一内容（等下一次变化）。
- `MEMORY_CLEAR`、`SET_TEMPERAMENT` 永远要家长确认；已在重放窗口里的命令直接回 `applied`；过期（时钟已知）回 `expired`；`SET_PARENT_SETTINGS`、`SELECT_VOICE_PACK`、`SYNC_CONTENT` 本版回 `rejected`（待 codec v2 / 多声音包 / TF 下载器）。
- 同类命令的顺序（S3 / D5）：`SET_TEMPERAMENT` payload 里的单调整数 `seq`（字段名常量 `kCommandSeqKey`，缺省按 0）。设备只接受 `seq` 不低于已应用值的命令；待确认队列里新请求（`seq` 更高，或相同 `seq` 后到）替换同类旧请求，被替换/过时的回 `rejected` 并写入 NVS 拒绝表；一条已应用的命令之后，所有更旧的待确认请求被丢弃。已应用的 `seq` 持久化在 NVS `kid_srv/seq_temp`。
- 已应用回执持久化在 NVS `kid_srv/applied`，直到服务端 `acked_commands` 确认（重放窗口只有 128 条，否则重启 + 大量交互后清空记忆可能被再确认一次）。
- 家长确认后若 Pet 任务的结果丢了（事件队列满），60 s 后把已确认的命令重新投给 Pet 任务：账本按 id 去重，回执为 `applied`，不会误报 `expired`。
- 离线时配对页也会超时：首次连不上服务器 60 s 失败，屏上的码到期即失败；只有屏上有码时家长页才不因空闲退出。
- 时钟：XiaoZhi 把系统时间设成本地时间，所以 UTC 取自服务器回复的 `Date` 头加单调时钟。
- token：配对后 30 天轮换：新 token 立即在内存里使用，写 NVS 失败会持续重试，再 `token/confirm`。**轮换响应丢失**时下次用旧 token 轮换会得到 409（`ROTATE_WITH_CURRENT_TOKEN`）：设备立刻显示"需要重新连接"（`kid_srv/repair=1`，持久化），设备继续工作到旧 token 失效（24 h），家长须在这段时间内重新配对；根治需要服务端配合（`token_next`），见 PR 待办。连续 3 次 401 → 擦除 token、`repair=2`（持久化），显示"需要重新连接"，不自动重配。日志从不打印 token、claim_secret 或配对码。

## 待办（真机端到端，等公网 HTTPS）

1. 公网 HTTPS 就绪后把板上 OTA 地址（或 `kid_srv/origin`）改为公网地址。
2. 在 CT 108 的 pet-learning 容器里用 `PairingService` 生成并兑换配对码（写审计日志），看设备配对页、串口 `PET_PAIR_*`。
3. 确认服务端收到快照（`PET_SYNC_STATE result=accepted`，`device_state` 有行）。
4. 下发一条命令（如 `SET_TEMPERAMENT` 为当前性格），家长在设备上确认，看 `PET_COMMAND ... outcome=applied` 与回执被确认；测完撤销测试 token。
