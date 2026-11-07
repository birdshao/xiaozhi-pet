# 服务端 S3：初始化、策略与家长备注（V5-06）

对应 `V5_V6_SERVER_PLAN.md` §5.4、§7.1/§7.2/§7.5、D5/D9/D13。代码：`profile.py`（家长路由 + 档案服务）、`policy.py`（策略与保守合并）、`notes.py`（加密备注）、`migrations/006_profile_policy.sql`。`api.py` 只加一行挂载；`sync.py` 只加一个快照钩子；`commands.py` 加 `supersede()`。设备端配套（§11.2：确认页、codec v2、声音包选择）不在本阶段。

## 接口（全部家长会话；写操作另需 CSRF 与 Origin 校验；响应 `Cache-Control: no-store`）

| 路由 | 说明 |
|---|---|
| `POST /v1/parent/devices/{id}/init` | §7.2 初始化：`{pet_name, temperament, voice_pack_id?}` → `profiles(rev=1)`，下发 `SET_TEMPERAMENT`（设备上确认）和（选了声音包时）`SELECT_VOICE_PACK`。已初始化 → 409 |
| `GET/PUT .../profile` | 名字、性格、声音包；PUT 带 `base_rev`，过期 409；只改名字不下发命令 |
| `GET/PUT .../policy` | 作息/音量/体感 + 开放问答；PUT 带 `base_rev` 与部分字段 `settings` |
| `GET/PUT/DELETE .../notes` | 家长备注；PUT/DELETE 必须带 `reauth: {password, otp}` |

- 名字：1–6 个汉字或 1–12 个字母（§6.4）。性格：`curious / gentle / lively`。
- 声音包只能选**设备 inventory 里**且**服务端目录已审核**（revision 与 header CRC 都对上）的包。
- 响应里的 `commands` 是 S2 通道的状态：`queued → delivered → awaiting_device_confirm → applied / rejected / expired`。性格命令 7 天过期，其余 24 h。新的同类命令会让未完成的旧命令以 `SUPERSEDED` 结束，设备不会先后收到互相矛盾的两条。
- 审计 actor 为 `parent:<账号 id>`，只记操作类型（`profile.init`、`profile.update`、`policy.update`、`policy.merge`、`notes.update`、`notes.delete`、`parent.reauth`），不记名字和备注。

## 性格与确认（D5）

不新建确认机制：确认由 S2 的 `CONFIRM_KINDS` 按命令种类固定（`SET_TEMPERAMENT`、`MEMORY_CLEAR` 恒需设备确认，面板无法关闭），状态走 S2 的 pending/applied/rejected/expired。初始化时设备还没有性格，所以 `SET_TEMPERAMENT` 总是发，即使和固件默认值一样。

**请求值与已确认值分开存**：`profiles.temperament` 是家长**请求**的性格；`profiles.temperament_confirmed` 只在收到 `applied` 回执后才写入（之前为 `null`）。接口同时返回两者以及 `device_temperament`（设备快照里的实际值）。**S6 的角色设定只能读 `temperament_confirmed`（或设备快照里的实际性格），不能读请求值。** 设备拒绝或命令过期后，请求值保留，家长再次提交**同一个值**会重新签发命令；已经确认的值再提交则不发命令。

**序号约定（`seq`）**：`SET_TEMPERAMENT` 的 payload 是 `{"temperament": "...", "seq": n}`。`seq` 按设备单调递增（每次签发加一，初始化为 1）。设备**必须只接受 `seq` 不低于已应用序号的同类命令**，并持久化已应用的最大 `seq`；低于它的命令回执 `rejected`。原因：设备可能已持有一条正在等待家长确认的旧命令 X，家长随后改成 Y，服务端把 X 标为 `SUPERSEDED`，但设备上仍可能先确认 Y 再确认 X。服务端兜底：`SUPERSEDED` 的命令若收到 `applied` 回执（设备是权威），仍记为已确认值，并在它不等于家长想要的值、且没有同值的待办命令时，用更高的 `seq` 重新签发想要的值。

## 保守合并（§5.4、D13）

服务端为每台设备记三份：`settings_json`（家长想要的）、`base_json`（上次确认设备持有的）、设备最新快照。S2 每接受一份快照就调用 `policy.on_snapshot`：

| 情况 | 处理 |
|---|---|
| 设备值 = 想要的 | 已生效，`base` 前移 |
| 设备值 = `base` | 命令还在路上，什么也不做 |
| 设备值等于服务端**签发过的某个旧版本**（命令已生效，快照来得晚，面板此时又改了新版本） | 不是本地修改：`base` 前移，**不合并**，新版本的命令继续（否则会被误合并成更低的值） |
| 设备在自己的菜单里改过，且面板有未完成的改动 | **保守合并**后下发新命令，旧命令 `SUPERSEDED`，面板提示 `MERGED_CONSERVATIVELY` |
| 设备改过，面板没有未完成的改动 | 采用设备的值（设备也是合法写入者），`rev` 加一使过期表单 409，提示 `DEVICE_CHANGED` |

合并规则：音量上限、安静时音量取小；体感只要一方关就关；安静时段取覆盖两段的最小单一窗口（并集；两段不相连时覆盖中间空档；覆盖全天则放弃）；作息/午睡这类没有"安全方向"的字段，哪一方改了就取哪一方，两边都改取面板的。合并结果若违反设置规则，依次回退到设备的、旧的作息；仍无效则**不猜**：面板 PUT 返回 409 `SETTINGS_MERGE_REVIEW`，快照路径保留设备的值、丢弃面板的待发改动（`MERGE_FAILED_DEVICE_KEPT`）。禁止 last-write-wins。

**不带 `rev` 的设备**（codec v1，`parent_settings_rev` 恒为 0）：变更检测只比内容，`rev` 只用于面板的过期校验；命令里 `base_rev` 取设备上报值（0），`new_rev = max(服务端 rev, 设备 rev) + 1`。设备 codec v2 之后，设备端可据 `base_rev` 拒绝过期命令。已经下发、设备已收下的旧命令无法撤回，在 v1 设备上这是已知窗口（服务端会在下一份快照里发现并合并）。

## 开放问答开关

`policies.open_chat` 默认关。打开必须带密码 + 新的 TOTP（`reauth`），而且代码级开关 `policy.OPEN_CHAT_SELECTABLE` 目前为 `False`（S6 的 TTS 出口审计和真实审核提供方完成前不能打开，§6.5）——所以现在任何请求都得到 409 `OPEN_CHAT_UNAVAILABLE`，面板应置灰。关闭不需要二次验证。旧表 `devices.open_chat` 仍保留，S6 以 `policies.open_chat` 为准。

## 二次验证（reauth）

`ParentAuth.reauth()`：密码 + **新的**TOTP（不接受恢复码；刚用于登录或上一次二次验证的码算重放，需等下一个 30 s）。失败计入账号锁定（5 次锁 15 分钟，与登录共用）。改备注、打开开放问答使用它。

## 家长备注（D9）

昵称（≤20 字）、生日（`MM-DD`，不存年份）、备注（≤500 字），不允许控制字符、双向/零宽等格式字符、行/段分隔符和孤立代理项（换行允许），不合法一律 422。`PUT /notes` 是**整体替换**（未带的字段会被清空，至少带一项）。先校验输入，再做二次验证：格式错误的请求不会消耗 TOTP，也不计入失败次数。已撤销设备的备注仍可删除。三者与设备 id 一起序列化后用 Fernet 整体加密成一个 token 存 `parent_notes`，库文件和备份里都读不出明文。

- 密钥 `KID_PET_NOTES_KEY`（Fernet，须与 `KID_PET_TOTP_KEY`、`KID_PET_BACKUP_KEY` 都不同，单独保管；可写成逗号分隔的多把，**第一把加密，所有都能解密**，用于轮换，见下）。**生产模式缺失即拒绝启动**；开发模式缺失时备注接口返回 503 `NOTES_KEY_MISSING`，绝不明文存储。密钥错误或行被挪到别的设备 → 500 `NOTES_UNREADABLE`，不返回乱码。`deploy/.env.example`、`compose.yaml`、`check-config` 已接线。
- 隔离：只有 `profile.py` 的 `/notes` 三个处理函数引用 `notes.py`；语音、摘要、角色设定、设备、同步、命令、日志、审计详情都不碰它。测试 `test_v5_server_parent_notes.py` 既扫描源码的引用关系，也断言各接口响应、命令/档案/策略/记忆表、日志里没有备注内容。
- 生命周期：清空记忆不删备注；删除设备（外键级联）一并删除；保留期清理任务不触碰（家长删除前一直保留）。
- **部署影响**：`compose.yaml` 现在要求 `KID_PET_NOTES_KEY`，CT 108 升级前需先生成并写进 `.env`（`python -c "from cryptography.fernet import Fernet; print(Fernet.generate_key().decode())"`），否则 compose 起不来。丢了密钥，已有备注就读不出来。

### 备注密钥轮换

1. 生成新密钥，在 `.env` 里写成 `KID_PET_NOTES_KEY=<新>,<旧>`，重启服务（`check-config` 支持多把，要求互不相同且不同于 TOTP/备份密钥）。
2. 新备注用第一把加密；读到旧密钥加密的行时自动用第一把重新加密。
3. 想尽快迁完全部行：`python -m tools.pet_learning_admin --db ... rotate-notes-key`（输出重新加密的行数，不输出内容）。
4. 确认迁完（再跑一次输出 0）后，从 `.env` 去掉旧密钥。**旧密钥被去掉后，还在旧密钥下的行永久不可读**；备份里的旧备注同样需要旧密钥。

## 已知限制

- 不带 `rev` 的旧设备（codec v1）存在无法检测的丢失更新：设备本地改动与面板改动恰好发生在两份快照之间且最终内容相同的情况无法区分；`_three_way` 对"没有安全方向"的作息字段在两边同时改动时取面板的值，与 §5.4 字面（只规定了音量/安静时段/体感三项）略有出入，属于方案未覆盖部分的取舍。
- 单租户假设：只有一个家长账号（D11），家长路由只校验会话，不校验账号与设备的归属；多家庭/多账号需要先加归属表。

## 方案里含糊之处的取舍（取更保护隐私的一边）

1. 方案只说备注的 `note` 和 `birthday` 加密；这里**昵称也加密**，并把三项合成一个密文，字段是否存在也不外露。
2. 方案把"改备注"列为敏感操作但没说"读备注"；读备注只需会话，不需二次验证。
3. 生日只接受 `MM-DD`（可含 2-29），拒绝带年份。
