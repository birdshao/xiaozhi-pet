# 家长 Web 面板（`/parent/`）

PetLearning 自带的轻量家长管理端，风格参照小智官方智控台（左侧导航 + 右侧列表/卡片，手机浏览器可用），但**不部署官方 Java 智控台**。代码在 `services/pet_learning/panel/`，由 `api.py` 挂载一个 router。设计背景见 [V5_V6_SERVER_PLAN.md](V5_V6_SERVER_PLAN.md) §7.1。

- 服务端渲染 HTML，无前端构建链、无模板引擎、无新增依赖；只有一个很小的 `panel.js`（销毁性操作前弹确认框，不开 JS 也能用）。
- 业务逻辑一律复用现有服务层（`PairingService`、`StateSync`、`ProfileService`、`ParentAuth`），面板只负责浏览器这一层：表单登录、CSRF、HTML。
- 与 JSON API（`/v1/parent/*`）共用同一套会话：任一入口登录得到的 cookie 在另一个入口同样有效。

## 页面清单

只覆盖**固件实际支持**的功能。固件会继续拒绝 `SET_PARENT_SETTINGS`、`SELECT_VOICE_PACK`、`SYNC_CONTENT`，所以面板不提供会下发这三条命令的功能。

| 路径 | 方法 | 内容 |
|---|---|---|
| `/parent/login` | GET / POST | 用户名 + 密码 + 6 位 TOTP（或 32 位恢复码）。没有注册页，账号用 `pet_learning_admin create-parent` 创建 |
| `/parent/logout` | POST | 退出并作废会话 |
| `/parent/devices` | GET | 设备列表：在线/离线、最近活跃、电量、宠物状态、固件、等待设备确认的请求数 |
| `/parent/devices/{id}` | GET | 概览：最近快照时间、电量、宠物状态、**命令回执表**（已生效 / 等待设备确认 / 被拒绝 / 已过期 / 排队中 / 已下发）、撤销设备 |
| `/parent/devices/{id}/revoke` | POST | 撤销设备（不可恢复） |
| `/parent/pairing` | GET | 添加设备说明 + "开始配对" |
| `/parent/pairings` | POST | 创建配对会话，跳转到 `/parent/pairings/{pairing_id}` |
| `/parent/pairings/{pairing_id}` | GET | 配对状态；设备显示 6 位码后在这里输入 |
| `/parent/pairings/{pairing_id}/redeem` | POST | 提交配对码（5 次错误作废，沿用 S1 的限流） |
| `/parent/devices/{id}/profile` | GET | 宠物档案：名字、想要的性格、设备上已确认的性格、最近一次 `SET_TEMPERAMENT` 的回执。页面顶部固定提示"改性格必须在设备上由家长确认才生效" |
| `/parent/devices/{id}/profile/init` | POST | 首次初始化档案（名字 + 性格） |
| `/parent/devices/{id}/profile` | POST | 改名字 / 性格（带 `base_rev` 防并发覆盖）；性格被拒绝或过期后可"重新请求设备确认"。只改名字不会重发性格命令 |
| `/parent/devices/{id}/memory` | GET | 记忆与每日摘要；清空状态（无 / 等待设备确认 / 已清空 / 设备已拒绝 / 已过期），**墓碑**是否启用，"恢复同步" |
| `/parent/devices/{id}/memory/clear` | POST | 清空记忆：服务器副本立即删除，并下发 `MEMORY_CLEAR`，需在设备上确认 |
| `/parent/devices/{id}/memory/resume` | POST | 设备拒绝或超时后恢复同步（解除墓碑） |
| `/parent/devices/{id}/notes` | GET | 家长备注（昵称、生日月日、文字备注），加密存储，只在面板读写 |
| `/parent/devices/{id}/notes` | POST | 保存备注（自用面板，不再要求二次验证码；JSON API 仍要求 step-up） |
| `/parent/devices/{id}/notes/delete` | POST | 删除备注 |
| `/parent/audit` | GET | 审计日志，只读，每页 50 条 |
| `/parent/static/panel.css`、`panel.js` | GET | 静态资源（白名单两个文件） |

**不做**：内容包分配与上传、声音包选择、家长设置下发（作息/音量/体感）、开放问答开关、注册页。对应的 JSON API 仍在，只是面板不暴露。

## 安全设计

**登录与会话**
- 登录走 `ParentAuth.login`：密码（scrypt）+ TOTP，TOTP 步长不可重放；恢复码一次性。
- 限流沿用现有规则：同一 IP 15 分钟内最多 5 次尝试（含成功），同一账号连续 5 次失败锁定 15 分钟；被限流时返回 429 页面，即使凭据正确也拒绝。
- 会话 cookie `__Host-pet_parent`：`HttpOnly; Secure; SameSite=Strict; Path=/`，12 小时绝对过期 + 1 小时空闲过期，服务端只存哈希。与 JSON API 相同，不新增会话体系。
- 用户名/密码/验证码格式不对时直接返回统一的"用户名、密码或验证码不正确"，不回显任何输入。

**CSRF（三层）**
1. 登录表单（尚无会话）：双提交。`GET /parent/login` 下发 `__Host-pet_login_csrf`（HttpOnly、Strict），表单里的隐藏字段是该 cookie 值经 HMAC（密钥派生自 `KID_PET_TOTP_KEY`）得到的令牌；两者对不上即 403。
2. 已登录的所有 POST：隐藏字段 `csrf` 必须同时等于 `__Host-pet_csrf` cookie 和会话里存的哈希（复用 `ParentAuth.session` 的校验）。缺失或不符 → 403，什么都不执行。
3. 所有 POST 还要过 `check_origin`：带了 `Origin` 头就必须等于服务自己的来源（`KID_PET_PUBLIC_ORIGIN`）。因此面板的 `Referrer-Policy` 用 `same-origin` 而不是 `no-referrer`——后者会让浏览器对表单 POST 发 `Origin: null`，被拒绝。
- 表单体只接受 `application/x-www-form-urlencoded`，上限 8 KiB，最多 20 个字段。
- 未登录访问任何页面或 POST → 303 到 `/parent/login`（JSON API 仍返回 401，不跳转）。

**响应头（所有面板响应，含错误页、重定向、静态文件）**
- `Cache-Control: no-store`、`Pragma: no-cache`
- CSP 沿用 playground 的做法并更严：`default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'none'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'`（没有内联脚本和样式）
- `X-Content-Type-Options: nosniff`、`X-Frame-Options: DENY`、`Referrer-Policy: same-origin`、`Permissions-Policy` 关闭摄像头/麦克风/定位
- 页面带 `noindex`

**输出转义**
- `render.py` 里所有动态值都经过 `e()`（`html.escape(quote=True)`），只有模块自己拼出的 `Safe` 才不转义；不使用模板引擎，不用 `|safe` 之类的出口。
- 提示条（flash）按固定键名查表，查询串里的 `msg` 不会进入页面文字；错误页文字也全部来自固定表，不回显表单输入。
- 备注校验失败时会把已输入内容回填，回填同样经过转义；

**其他**
- 自用场景：面板的备注保存/删除不做二次验证码（登录已有密码 + TOTP）；JSON API 的 step-up 不变。
- 备注读写、清空记忆等敏感操作写审计（只记操作类型，不含内容），与 JSON API 一致。
- 设备 ID、配对 ID 进入路由前先过现有正则；不存在的设备返回 404 页面。
- 不影响设备路由和语音路由：它们的鉴权（设备 token、语音 token + 内网白名单）没有改动，面板会话对它们无效；测试里有对应断言。

## 测试

`tests/test_v5_parent_panel.py`（随 `python -m unittest discover -s tests -p "test_*.py"` 一起跑）：

- **登录**：成功、密码错误、TOTP 错误/格式错误、未知用户、TOTP 重放、5 次失败后限流（正确凭据也 429）、恢复码、登录 CSRF（缺失 / cookie 与令牌不配）、`Origin` 不符与 `Origin: null`、非表单请求 415、退出。
- **未登录**：每个 GET 页面和每个 POST 都重定向到登录页。
- **CSRF**：每个写操作缺失/错误令牌都是 403 且不改数据；跨站 Origin 被拒；超大表单 413。
- **每个页面和每个操作**：设备列表/概览/在线状态、配对全流程（含错码、作废）、撤销、档案初始化与修改（含 `base_rev` 冲突、只改名不重发性格）、性格回执四种状态与重新请求、命令过期、清空记忆（墓碑、被拒绝后恢复同步、已清空、已过期）、备注保存/读取/删除（密文落库、校验失败回填表单、无密钥时不可用）、审计日志只读与分页、静态文件白名单。
- **不暴露**：面板页面里没有声音包/内容包/开放问答/设置下发入口，相应路径 404，且不会产生 `SELECT_VOICE_PACK` / `SET_PARENT_SETTINGS` / `SYNC_CONTENT` 命令。
- **XSS**：设备固件版本、IP、审计日志各字段、备注昵称/正文中的 HTML 全部被转义；路径、查询串、表单输入不被回显；登录错误页不回显输入。
- **回归**：`/healthz`、设备命令路由、语音路由、JSON 登录接口行为不变。

## 本地运行

```bash
export KID_PET_TOTP_KEY=$(python -c "from cryptography.fernet import Fernet;print(Fernet.generate_key().decode())")
export KID_PET_NOTES_KEY=$(python -c "from cryptography.fernet import Fernet;print(Fernet.generate_key().decode())")
python tools/pet_learning_admin.py create-parent   # 创建家长账号并绑定 TOTP
python -m uvicorn services.pet_learning.api:app --host 127.0.0.1 --port 8765
```

浏览器打开 `http://127.0.0.1:8765/parent/`。会话 cookie 带 `Secure`，浏览器只对 `localhost`/`127.0.0.1` 之外的地址要求 HTTPS；上线时务必走 §3.3 的 HTTPS 入口，并在 `.env` 里设置 `KID_PET_PUBLIC_ORIGIN`、`KID_PET_TRUSTED_PROXY`。
