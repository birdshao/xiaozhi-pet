# 服务端 S1：配对与设备凭证

对应 `V5_V6_SERVER_PLAN.md` §5.1、§9、§11.1。代码：`services/pet_learning/pairing.py`，迁移 `003_pairing.sql`，测试 `tests/test_v5_pairing_auth.py`。

## 流程

1. 家长（登录会话 + CSRF）`POST /v1/parent/pairings` → `{pairing_id, expires_in: 600}`。同账号新建会使旧的未完成配对作废。没有开放配对时 `/v1/pairing/*` 一律 404。
2. 设备 `POST /v1/pairing/start {"device_id"}` → `{code(6 位), claim_secret, expires_in}`；设备屏幕显示 code，`claim_secret` 留在 RAM。已存在的 device_id（含已撤销）、无开放配对、他人已占用，一律同一个 404。每配对最多 start 3 次。
3. 家长 `POST /v1/parent/pairings/{pairing_id}/redeem {"code"}`。常量时间比较；同一配对 5 次错码作废（410），之后正确码也 404；同账号 15 分钟内累计 10 次错码 → 429 锁定到窗口滑出。
4. 设备轮询 `POST /v1/pairing/claim {"device_id","claim_secret"}`：未批准返回 `{"status":"pending"}`；批准后（120 s 内）一次性返回 32 字节随机 token，服务端只存 SHA-256，设备行为 `pending`（此时 token 不能调用任何业务接口）。
5. 设备写入 NVS 后 `POST /v1/pairing/confirm`（Bearer 新 token）→ `active`。300 s 内未确认则该 pending 设备被清除，需重新配对。

## 凭证

- **轮换**：`POST /v1/devices/{id}/token/rotate`（Bearer 当前或仍在重叠期的旧 token）→ 新 token；旧 token 在 24 h 内仍有效。设备写入 NVS 后调用 `POST /v1/devices/{id}/token/confirm`（Bearer 新 token），旧 token 立即失效。设备没收到上次轮换响应时可用旧 token 再轮换，重叠截止时间不顺延。每设备每小时最多 10 次。
- **撤销**：`POST /v1/parent/devices/{id}/revoke`（家长会话 + CSRF）→ `status=revoked`，新旧 token 立即 401（所有经 `PetStore.authenticated` 的设备/语音接口、轮换接口）。
- **列表**：`GET /v1/parent/devices` 返回状态、入网时间、轮换时间、`last_seen_at`/`last_seen_ip`（写入限频：每设备 60 s 一次，IP 变化立即写）、固件版本、重叠截止时间；绝不返回哈希。
- **越权**：token 只与自己的 device_id 绑定；A 的 token 对 B 的任何接口（含 rotate）为 401。
- **审计**（`audit_log`）：`pairing.create/start/redeem/claim/confirm`、`token.rotate/rotate_confirm`、`device.revoke`，含失败、限流；不记 token、code、claim_secret。

## 限流（应用内，在 §3.3 N8 入口限流之外）

| 范围 | 限制 |
|---|---|
| 家长创建配对 | 每账号 10 / 10 min |
| `start` | 每 IP 10 / 10 min，每 device_id 5 / 10 min |
| `claim` | 每 IP 120 / 10 min |
| `redeem` | 每 IP 10 / 10 min；每账号错码 10 / 15 min |
| 坏 token 打 rotate/confirm | 每 IP 20 / 10 min（窗口内该 IP 对这两个接口的正确 token 也拒绝；普通业务接口不受影响） |

客户端 IP 取法同家长登录（仅信任 `KID_PET_TRUSTED_PROXY` 的 XFF）。

## 迁移与既有 CLI token

`003_pairing.sql` 为 `devices` 增加 `status`（默认 `active`）、`enrolled_at`、`token_rotated_at`、`fw_version`、`last_seen_at`、`last_seen_ip`、`prev_token_sha`、`prev_token_expires_at`，新建 `pairing_codes`、`pairing_attempts`。**S0 时代 `enroll_device` 写入的 token 原样继续有效**（`active`，`enrolled_at` 为空表示 CLI 入网），也可以被轮换/撤销；`enroll_device` 保持不变。S0 库 → S1 库有测试。

`PetStore.authenticated` 现在委托给 `pairing.verify_token`（status、重叠期、last-used 都在这里），因此 device/voice 路由无需改动。

## 固件待办（本 PR 不改固件）

- 家长入口"连接家里的服务器"：调 `start`，局刷提示行显示 6 位码；`claim_secret` 只放 RAM，每 ~5 s 轮询 `claim`，码到期（`expires_in`）或收到非 pending 终态就退出。
- 收到 token 后写 NVS，成功后调 `confirm`；写失败不要 confirm，让配对超时。
- 轮换：定期（建议 30 天）调 `rotate`，NVS 写成功后调 `token/confirm`；写失败继续用旧 token（24 h 内可再轮换）。
- 收到 401 且重试无效：显示"需要重新连接"，不要自动重配。
- NVS 加密留待 V7（D12）；在此之前靠撤销、轮换和 last-seen 记录补偿。

## 已知限制

- 没有 `DELETE /v1/parent/devices/{id}`（级联删除全部数据，§8）；依赖 S2+ 的数据表，留给后续阶段。目前已撤销的 device_id 不能重新配对。
- 敏感操作二次验证（§9）不在本阶段。
- 开放配对窗口内，网络上任何人可抢先 `start` 占住（拿到的码家长看不到，因此只能造成拒绝服务，不能入网）；设备本机重试可重新取码。
