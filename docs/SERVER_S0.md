# 服务端 S0：本地操作与接口

实现范围对应 `V5_V6_SERVER_PLAN.md` §11 S0。设备/绘本/测验/故事接口保持原路径、请求、响应和设备鉴权；意图接口归到 voice router，但 `/v1/voice/*` 新协议及语音专用身份留给 S5。

## 安装和配置

```sh
python -m venv .venv
# 激活虚拟环境后
python -m pip install -r services/requirements.txt
```

将 `services/.env.example` 复制到运行目录的 `.env`。配置优先级：进程环境变量 > `.env` > 默认值；文件支持 `KEY=value`、单/双引号包裹的字面值及整行注释，不执行命令、变量插值或行尾注释。密码、密钥不要放命令行参数，也不要提交到 Git。

唯一新增运行依赖是 `cryptography>=44,<47`，用于 Fernet 认证加密 TOTP 密钥。方案表中 `totp_secret_enc` 要求加密存储，因此将原拟 S3 引入的依赖提前；scrypt、RFC 6238 TOTP、恢复码及 `.env` 读取均使用标准库。TOTP 加密密钥在数据库外生成并保存：

```sh
python -c "from cryptography.fernet import Fernet; print(Fernet.generate_key().decode())"
```

把输出私下保存为 `KID_PET_TOTP_KEY`，服务和 CLI 必须使用同一个值。已创建账号时，密钥缺失/不匹配拒绝启动；新开发库可先只使用设备接口。不要直接换掉密钥，否则已有 TOTP 无法解密。

`KID_PET_ENV=production` 启用基础校验：必须配置 HTTPS 的 `KID_PET_PUBLIC_ORIGIN`、单个代理 IP `KID_PET_TRUSTED_PROXY` 和有效 Fernet 密钥；关闭 `/play`、`/docs`、`/redoc`、`/openapi.json`。完整部署检查在 S7 扩展 `Settings.validate()`。代理必须覆盖 `X-Forwarded-For` 为单个客户端 IP；应用仅当直连 peer 等于受信代理时读取该头。启动 uvicorn 时使用 `--no-proxy-headers`，保留真实直连 peer 供应用判断：

```sh
python -m uvicorn services.pet_learning.api:app --host 127.0.0.1 --port 8780 --no-proxy-headers
```

## 创建唯一家长管理员

```sh
python -m tools.pet_learning_admin create-parent --username parent
```

密码由无回显提示或 `KID_PET_PARENT_PASSWORD` 环境变量读取（12–256 字符）。CLI 在本地终端显示新 TOTP secret 和 `otpauth://` URI；在认证器手动输入密钥或使用支持 URI 的认证器导入，输入当前验证码确认绑定后才写入账号。不新增 QR 图像依赖，不提供网页注册/绑定流程。成功后显示 8 个一次性恢复码，请离线保管；数据库只保存恢复码 SHA-256，恢复码仍须配合正确密码使用。不允许第二个管理员或静默覆盖现有账号。绑定使用过的验证码不能用于紧接着登录，等待认证器下一轮更新即可。

## 家长 API

`POST /v1/parent/login`，JSON：

```json
{"username":"parent","password":"由家长输入","otp":"六位验证码或32位恢复码"}
```

成功返回 `{"status":"ok","csrf_token":"..."}`，设置 `__Host-pet_parent`（Secure、HttpOnly、SameSite=Strict）及 `__Host-pet_csrf`（Secure、SameSite=Strict、供客户端读取）。两者均为 host-only、Path=/。家长接口即使开发模式也需 HTTPS 客户端；测试使用 HTTPS TestClient。响应禁止缓存。数据库只存会话和 CSRF 哈希。

`POST /v1/parent/logout` 需带会话 Cookie、CSRF Cookie 和相等的 `X-CSRF-Token` 请求头；令牌还须匹配当前会话存储的哈希。成功立即撤销会话并删除 Cookie。若带 `Origin`，登录和登出均要求与配置 origin 一致（开发未配置时匹配请求 origin）。设备 Bearer token 不可充当家长身份，家长 Cookie 也不可访问设备接口。

会话硬过期 12 小时，每次通过 `ParentAuth.session()` 验证才更新活动时间，连续 1 小时无活动失效；后续家长路由必须调用该验证方法，写操作必须传入 CSRF 双提交值。每 IP 15 分钟最多 5 次有效格式登录请求（包括成功、未知账号和失败）；账号连续 5 次认证失败锁定 15 分钟，跨 IP 生效，成功后清零。IP 限流和账号锁定返回 429，凭证错误返回通用 401。验证码窗口 ±1 个 30 秒周期，使用过的时间步拒绝重放。所有计数、锁定、恢复码消费和会话操作持久化并在 SQLite 写事务内执行。

审计表记录 CLI 创建、登录成功/失败/限流/锁定、新 IP 登录、恢复码使用、登出，字段为操作类型、时间、账号 ID、结果和 IP；不记密码、验证码、密钥、原始会话令牌或儿童内容。未知账号的用户名不写审计。S7 补充审计 365 天保留及清理任务。

## 数据库与测试

`db.py` 顺序应用 SQL 迁移，每个迁移独立事务；失败回滚本次数据、DDL 和 `user_version`，版本高于代码则拒绝启动。`001_baseline.sql` 收录原 P3/P4 的七张表，旧 `user_version=0` 数据库原地升级保留全部行；`002_parent_auth.sql` 新增家长鉴权及审计表。多个初始化器持写锁复查版本，支持重复启动。启用 WAL、foreign_keys 和 5 秒 busy_timeout。

```sh
python -m unittest discover -s tests -p "test_*.py"
```

新增 `test_v5_server_*` 覆盖迁移回滚/并发/旧库、RFC TOTP 向量/窗口/重放、CLI 绑定/恢复码、会话期限/撤销/CSRF、身份隔离、限流/锁定重启、审计隐私和配置。CI Python 3.10/3.12 执行全量 Python 测试，既有测试断言不改。

S1+ 未实现：配对和设备 token 轮换/撤销、记忆/状态同步/命令、家长业务面板及设置、内容目录分发、语音新协议、开放问答、敏感业务二次验证、部署/备份/清理/证书检查。未触及固件、串口和开发板。
