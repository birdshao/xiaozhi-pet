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
- 审计只记操作类型（`profile.init`、`profile.update`、`policy.update`、`policy.merge`、`notes.update`、`notes.delete`、`parent.reauth`），不记名字和备注。

## 性格与确认（D5）

不新建确认机制：确认由 S2 的 `CONFIRM_KINDS` 按命令种类固定（`SET_TEMPERAMENT`、`MEMORY_CLEAR` 恒需设备确认，面板无法关闭），状态走 S2 的 pending/applied/rejected/expired。初始化时设备还没有性格，所以 `SET_TEMPERAMENT` 总是发，即使和固件默认值一样。设备拒绝后，面板档案仍保留家长的选择，同时显示"被拒绝"和 `device_temperament`（设备上的实际值）。

## 保守合并（§5.4、D13）

服务端为每台设备记三份：`settings_json`（家长想要的）、`base_json`（上次确认设备持有的）、设备最新快照。S2 每接受一份快照就调用 `policy.on_snapshot`：

| 情况 | 处理 |
|---|---|
| 设备值 = 想要的 | 已生效，`base` 前移 |
| 设备值 = `base` | 命令还在路上，什么也不做 |
| 设备在自己的菜单里改过，且面板有未完成的改动 | **保守合并**后下发新命令，旧命令 `SUPERSEDED`，面板提示 `MERGED_CONSERVATIVELY` |
| 设备改过，面板没有未完成的改动 | 采用设备的值（设备也是合法写入者），`rev` 加一使过期表单 409，提示 `DEVICE_CHANGED` |

合并规则：音量上限、安静时音量取小；体感只要一方关就关；安静时段取覆盖两段的最小单一窗口（并集；两段不相连时覆盖中间空档；覆盖全天则放弃）；作息/午睡这类没有"安全方向"的字段，哪一方改了就取哪一方，两边都改取面板的。合并结果若违反设置规则，依次回退到设备的、旧的作息；仍无效则**不猜**：面板 PUT 返回 409 `SETTINGS_MERGE_REVIEW`，快照路径保留设备的值、丢弃面板的待发改动（`MERGE_FAILED_DEVICE_KEPT`）。禁止 last-write-wins。

**不带 `rev` 的设备**（codec v1，`parent_settings_rev` 恒为 0）：变更检测只比内容，`rev` 只用于面板的过期校验；命令里 `base_rev` 取设备上报值（0），`new_rev = max(服务端 rev, 设备 rev) + 1`。设备 codec v2 之后，设备端可据 `base_rev` 拒绝过期命令。已经下发、设备已收下的旧命令无法撤回，在 v1 设备上这是已知窗口（服务端会在下一份快照里发现并合并）。

## 开放问答开关

`policies.open_chat` 默认关。打开必须带密码 + 新的 TOTP（`reauth`），而且代码级开关 `policy.OPEN_CHAT_SELECTABLE` 目前为 `False`（S6 的 TTS 出口审计和真实审核提供方完成前不能打开，§6.5）——所以现在任何请求都得到 409 `OPEN_CHAT_UNAVAILABLE`，面板应置灰。关闭不需要二次验证。旧表 `devices.open_chat` 仍保留，S6 以 `policies.open_chat` 为准。

## 二次验证（reauth）

`ParentAuth.reauth()`：密码 + **新的**TOTP（不接受恢复码；刚用于登录或上一次二次验证的码算重放，需等下一个 30 s）。失败计入账号锁定（5 次锁 15 分钟，与登录共用）。改备注、打开开放问答使用它。

## 家长备注（D9）

昵称（≤20 字）、生日（`MM-DD`，不存年份）、备注（≤500 字），不允许控制字符。三者与设备 id 一起序列化后用 Fernet 整体加密成一个 token 存 `parent_notes`，库文件和备份里都读不出明文。

- 密钥 `KID_PET_NOTES_KEY`（Fernet，须与 `KID_PET_TOTP_KEY`、`KID_PET_BACKUP_KEY` 都不同，单独保管）。**生产模式缺失即拒绝启动**；开发模式缺失时备注接口返回 503 `NOTES_KEY_MISSING`，绝不明文存储。密钥错误或行被挪到别的设备 → 500 `NOTES_UNREADABLE`，不返回乱码。`deploy/.env.example`、`compose.yaml`、`check-config` 已接线。
- 隔离：只有 `profile.py` 的 `/notes` 三个处理函数引用 `notes.py`；语音、摘要、角色设定、设备、同步、命令、日志、审计详情都不碰它。测试 `test_v5_server_parent_notes.py` 既扫描源码的引用关系，也断言各接口响应、命令/档案/策略/记忆表、日志里没有备注内容。
- 生命周期：清空记忆不删备注；删除设备（外键级联）一并删除；保留期清理任务不触碰（家长删除前一直保留）。
- **部署影响**：`compose.yaml` 现在要求 `KID_PET_NOTES_KEY`，CT 108 升级前需先生成并写进 `.env`（`python -c "from cryptography.fernet import Fernet; print(Fernet.generate_key().decode())"`），否则 compose 起不来。丢了密钥，已有备注就读不出来。

## 方案里含糊之处的取舍（取更保护隐私的一边）

1. 方案只说备注的 `note` 和 `birthday` 加密；这里**昵称也加密**，并把三项合成一个密文，字段是否存在也不外露。
2. 方案把"改备注"列为敏感操作但没说"读备注"；读备注只需会话，不需二次验证。
3. 生日只接受 `MM-DD`（可含 2-29），拒绝带年份。
