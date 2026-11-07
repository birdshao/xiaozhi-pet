"""Server-side HTML for the parent panel. No template engine: every dynamic value goes through e().

`e()` escapes anything that is not already `Safe`; only this module builds `Safe` strings
(from literals and from escaped parts), so a stored value can never become markup.
"""
from __future__ import annotations

import html
from datetime import datetime
from typing import Any, Iterable
from zoneinfo import ZoneInfo

ONLINE_SECONDS = 15 * 60


class Safe(str):
    """Markup that is already escaped."""
    __slots__ = ()


def e(value: Any) -> Safe:
    if isinstance(value, Safe):
        return value
    return Safe(html.escape("" if value is None else str(value), quote=True))


def join(parts: Iterable[Any]) -> Safe:
    return Safe("".join(e(p) for p in parts))


STATUS_LABELS = {
    "queued": ("等待设备获取", "wait"),
    "delivered": ("已下发，等待设备处理", "wait"),
    "awaiting_device_confirm": ("等待设备确认", "warn"),
    "applied": ("已生效", "ok"),
    "rejected": ("被拒绝", "bad"),
    "expired": ("已过期", "muted"),
}
KIND_LABELS = {
    "SET_TEMPERAMENT": "修改性格",
    "MEMORY_CLEAR": "清空记忆",
    "SET_PARENT_SETTINGS": "家长设置",
    "SELECT_VOICE_PACK": "声音包",
    "SYNC_CONTENT": "内容同步",
}
TEMPERAMENT_LABELS = {"curious": "好奇", "gentle": "温柔", "lively": "活泼"}
MODE_LABELS = {"IDLE": "空闲", "INTERACT": "互动中", "STORY": "讲故事", "LISTENING": "聆听中",
               "SPEAKING": "说话中", "QUIET": "安静"}
MOOD_LABELS = {"NEUTRAL": "平静", "HAPPY": "开心", "CURIOUS": "好奇", "THINKING": "思考", "SLEEPY": "困了"}
MILESTONE_LABELS = {"first_book": "读完第一本书", "five_books": "读完 5 本书", "ten_books": "读完 10 本书",
                    "first_game": "完成第一个游戏", "ten_games": "完成 10 个游戏",
                    "first_preference": "第一次选择喜好", "seven_days": "活跃满 7 天"}
CLEAR_LABELS = {"none": "无", "pending": "等待设备确认", "applied": "已清空", "rejected": "设备已拒绝",
                "expired": "已过期（设备未确认）"}
DEVICE_STATUS = {"active": "使用中", "revoked": "已撤销"}


def clock(epoch: int | None, tz: str) -> str:
    if not epoch:
        return "—"
    return datetime.fromtimestamp(epoch, ZoneInfo(tz)).strftime("%Y-%m-%d %H:%M:%S")


def ago(epoch: int | None, now: int) -> str:
    if not epoch:
        return "从未"
    delta = max(0, now - epoch)
    if delta < 60:
        return "刚刚"
    if delta < 3600:
        return f"{delta // 60} 分钟前"
    if delta < 86400:
        return f"{delta // 3600} 小时前"
    return f"{delta // 86400} 天前"


def badge(label: str, kind: str) -> Safe:
    return Safe(f'<span class="badge {e(kind)}">{e(label)}</span>')


def status_badge(status: str) -> Safe:
    label, kind = STATUS_LABELS.get(status, (status, "muted"))
    return badge(label, kind)


NAV = (("devices", "/parent/devices", "设备"),
       ("pairing", "/parent/pairing", "添加设备"),
       ("audit", "/parent/audit", "审计日志"))


def layout(title: str, body: Safe, *, active: str = "", csrf: str | None = None,
           notice: tuple[str, str] | None = None) -> Safe:
    nav = "".join(
        f'<a href="{e(href)}"{" class=on" if key == active else ""}>{e(label)}</a>' for key, href, label in NAV)
    logout = ""
    if csrf is not None:
        logout = (f'<form method="post" action="/parent/logout" class="logout">'
                  f'<input type="hidden" name="csrf" value="{e(csrf)}">'
                  f'<button type="submit" class="link">退出登录</button></form>')
    flash = ""
    if notice:
        flash = f'<div class="flash {e(notice[1])}" role="status">{e(notice[0])}</div>'
    return Safe(f"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex,nofollow">
<title>{e(title)} · 宠物家长面板</title>
<link rel="stylesheet" href="/parent/static/panel.css">
<script src="/parent/static/panel.js" defer></script></head>
<body><div class="shell">
<aside class="side"><div class="brand">小宠家长面板</div><nav>{nav}</nav>{logout}</aside>
<main class="main"><h1>{e(title)}</h1>{flash}{body}</main></div></body></html>""")


def login_page(csrf_field: str, error: str | None = None) -> Safe:
    flash = f'<div class="flash bad" role="alert">{e(error)}</div>' if error else ""
    return Safe(f"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex,nofollow">
<title>登录 · 宠物家长面板</title>
<link rel="stylesheet" href="/parent/static/panel.css"></head>
<body class="center"><main class="card login"><h1>家长登录</h1>{flash}
<form method="post" action="/parent/login" autocomplete="off">
<input type="hidden" name="csrf" value="{e(csrf_field)}">
<label>用户名<input name="username" required maxlength="64" autocomplete="username" autofocus></label>
<label>密码<input name="password" type="password" required maxlength="256" autocomplete="current-password"></label>
<label>动态验证码（6 位）或恢复码<input name="otp" required maxlength="32" inputmode="numeric" autocomplete="one-time-code"></label>
<button type="submit" class="primary">登录</button></form>
<p class="hint">家长账号由管理员在服务器上创建，这里不提供注册。</p></main></body></html>""")


def message_page(title: str, text: str, *, back: str = "/parent/devices") -> Safe:
    return layout(title, Safe(f'<div class="card"><p>{e(text)}</p><p><a href="{e(back)}">返回</a></p></div>'))


def hidden(csrf: str, **fields: Any) -> Safe:
    return Safe(f'<input type="hidden" name="csrf" value="{e(csrf)}">' + "".join(
        f'<input type="hidden" name="{e(k)}" value="{e(v)}">' for k, v in fields.items()))


# -- devices -----------------------------------------------------------------

def device_tabs(device_id: str, active: str) -> Safe:
    tabs = (("overview", "", "概览"), ("profile", "/profile", "宠物档案"), ("memory", "/memory", "记忆"),
            ("notes", "/notes", "家长备注"))
    return Safe('<div class="tabs">' + "".join(
        f'<a href="/parent/devices/{e(device_id)}{e(path)}"{" class=on" if key == active else ""}>{e(label)}</a>'
        for key, path, label in tabs) + "</div>")


def pet_summary(state: dict | None) -> Safe:
    if not state:
        return Safe('<span class="muted">设备还没有上传过状态</span>')
    pet = state["pet"]
    return join([MODE_LABELS.get(pet["mode"], pet["mode"]), " · ", MOOD_LABELS.get(pet["mood"], pet["mood"])])


def battery_text(state: dict | None) -> str:
    if not state:
        return "—"
    return f"{state['battery_pct']}%" + ("（充电中）" if state["charging"] else "")


def is_online(device: dict, now: int) -> bool:
    return device["status"] == "active" and bool(device["last_seen_at"]) and now - device["last_seen_at"] <= ONLINE_SECONDS


def device_list(rows: list[dict], now: int, tz: str) -> Safe:
    if not rows:
        return Safe('<div class="card"><p>还没有设备。</p><p><a class="btn primary" href="/parent/pairing">添加设备</a></p></div>')
    cards = []
    for r in rows:
        online = is_online(r, now)
        state = r["state"]
        awaiting = r["awaiting"]
        cards.append(f"""<a class="card device" href="/parent/devices/{e(r['device_id'])}">
<div class="row"><strong>{e(r['pet_name'] or r['device_id'])}</strong>
{badge("在线", "ok") if online else badge("离线" if r['status'] == 'active' else DEVICE_STATUS.get(r['status'], r['status']), "muted")}</div>
<dl><dt>设备 ID</dt><dd>{e(r['device_id'])}</dd>
<dt>最近活跃</dt><dd>{e(ago(r['last_seen_at'], now))}</dd>
<dt>电量</dt><dd>{e(battery_text(state))}</dd>
<dt>宠物状态</dt><dd>{pet_summary(state)}</dd>
<dt>固件</dt><dd>{e(r['fw_version'] or (state or {}).get('fw_version') or '—')}</dd></dl>
{badge(f"{awaiting} 项等待设备确认", "warn") if awaiting else ""}</a>""")
    return Safe('<div class="grid">' + "".join(cards) + "</div>")


def command_table(items: list[dict], tz: str) -> Safe:
    if not items:
        return Safe('<p class="muted">暂无命令记录。</p>')
    body = "".join(
        f"<tr><td>{e(KIND_LABELS.get(c['kind'], c['kind']))}</td><td>{status_badge(c['status'])}</td>"
        f"<td>{e(clock(c['created_at'], tz))}</td><td>{e(clock(c['completed_at'], tz))}</td></tr>" for c in items)
    return Safe('<div class="scroll"><table><thead><tr><th>命令</th><th>回执状态</th><th>创建时间</th>'
                f'<th>完成时间</th></tr></thead><tbody>{body}</tbody></table></div>')


def device_overview(d: dict, now: int, tz: str, csrf: str) -> Safe:
    device, state = d["device"], d["state"]
    online = is_online(device, now)
    revoke = ""
    if device["status"] == "active":
        revoke = (f'<form method="post" action="/parent/devices/{e(device["device_id"])}/revoke" '
                  f'data-confirm="撤销后设备将无法再连接服务器，且不能恢复，确定撤销吗？">{hidden(csrf)}'
                  f'<button class="danger" type="submit">撤销此设备</button></form>')
    snap = "—"
    pet = Safe('<span class="muted">尚无快照</span>')
    if state:
        snap = f"{clock(state['received_at'], tz)}（{ago(state['received_at'], now)}）"
        pet = pet_summary(state)
    return Safe(f"""{device_tabs(device['device_id'], 'overview')}
<div class="card"><h2>状态</h2><dl>
<dt>设备 ID</dt><dd>{e(device['device_id'])}</dd>
<dt>设备状态</dt><dd>{badge("在线", "ok") if online else badge(DEVICE_STATUS.get(device['status'], device['status']) if device['status'] != 'active' else '离线', 'muted')}</dd>
<dt>最近活跃</dt><dd>{e(clock(device['last_seen_at'], tz))}（{e(ago(device['last_seen_at'], now))}）</dd>
<dt>配对时间</dt><dd>{e(clock(device['enrolled_at'], tz))}</dd>
<dt>最近快照</dt><dd>{e(snap)}</dd>
<dt>电量</dt><dd>{e(battery_text(state))}</dd>
<dt>宠物状态</dt><dd>{pet}</dd>
<dt>固件版本</dt><dd>{e(device['fw_version'] or (state or {}).get('fw_version') or '—')}</dd></dl></div>
<div class="card"><h2>命令回执</h2>
<p class="hint">「修改性格」「清空记忆」需要在设备上由家长确认后才会生效。</p>{command_table(d['commands'], tz)}</div>
<div class="card">{revoke or '<p class="muted">此设备已撤销。</p>'}</div>""")


def pairing_start(csrf: str) -> Safe:
    return Safe(f"""<div class="card"><h2>添加设备</h2>
<ol><li>点击下方「开始配对」。</li>
<li>在 10 分钟内，让设备进入配对流程，设备屏幕会显示 6 位配对码。</li>
<li>在本页输入这 6 位码，设备随后自动完成配对。</li></ol>
<form method="post" action="/parent/pairings">{hidden(csrf)}<button class="primary" type="submit">开始配对</button></form></div>""")


PAIRING_STATES = {"open": "等待设备发起配对", "code_issued": "设备已显示配对码，请输入",
                  "approved": "已批准，等待设备完成", "claimed": "设备已领取，等待确认", "void": "已作废",
                  "expired": "已过期", "done": "已完成", "confirmed": "已完成"}


def pairing_wait(pairing_id: str, state: str | None, csrf: str) -> Safe:
    label = PAIRING_STATES.get(state or "", state or "未知")
    form = ""
    if state in {"open", "code_issued"}:
        form = (f'<form method="post" action="/parent/pairings/{e(pairing_id)}/redeem" autocomplete="off">'
                f'{hidden(csrf)}<label>设备上显示的 6 位配对码'
                f'<input name="code" required maxlength="6" minlength="6" pattern="[0-9]{{6}}" inputmode="numeric" '
                f'autocomplete="one-time-code" autofocus></label>'
                f'<button class="primary" type="submit">提交配对码</button></form>')
        if state == "open":
            form = '<p class="hint">设备尚未发起配对；发起后刷新本页。</p>' + form
    return Safe(f"""<div class="card"><h2>配对进行中</h2><p>当前状态：{badge(label, 'wait' if state in {'open', 'code_issued', 'approved', 'claimed'} else 'muted')}</p>
{form}<p><a href="/parent/pairings/{e(pairing_id)}">刷新状态</a> · <a href="/parent/devices">返回设备列表</a></p></div>""")


# -- profile -----------------------------------------------------------------

def command_line(cmd: dict | None, tz: str) -> Safe:
    if not cmd:
        return Safe("")
    return Safe(f'<p>最近一次「{e(KIND_LABELS.get(cmd["kind"], cmd["kind"]))}」：{status_badge(cmd["status"])}'
                f'（{e(clock(cmd["created_at"], tz))}）</p>')


def profile_page(device: dict, view: dict, tz: str, csrf: str) -> Safe:
    device_id = device["device_id"]
    writable = device["status"] == "active"
    options = lambda current: "".join(  # noqa: E731
        f'<option value="{e(t)}"{" selected" if t == current else ""}>{e(TEMPERAMENT_LABELS.get(t, t))}</option>'
        for t in view["temperaments"])
    notice = Safe('<div class="flash warn">更改性格会向设备发出请求，必须由家长在设备上确认后才会生效；'
                  '确认之前设备仍使用原来的性格。</div>')
    cmd = view["commands"]["temperament"]
    if not writable:
        form = '<p class="muted">设备已撤销，不能再修改档案。</p>'
    elif not view["initialized"]:
        form = (f'<form method="post" action="/parent/devices/{e(device_id)}/profile/init">{hidden(csrf)}'
                f'<label>宠物名字（1–6 个汉字，或 1–12 个英文字母）<input name="pet_name" required maxlength="12"></label>'
                f'<label>性格<select name="temperament">{options("gentle")}</select></label>'
                f'<button class="primary" type="submit">初始化档案</button></form>')
    else:
        resend = ""
        if (view["temperament"] != view["temperament_confirmed"] and cmd
                and cmd["status"] in {"rejected", "expired"}):
            resend = (f'<button type="submit" name="resend" value="1" formnovalidate>重新请求设备确认</button>')
        form = (f'<form method="post" action="/parent/devices/{e(device_id)}/profile">'
                f'{hidden(csrf, base_rev=view["rev"])}'
                f'<label>宠物名字<input name="pet_name" value="{e(view["pet_name"])}" required maxlength="12"></label>'
                f'<label>性格<select name="temperament">{options(view["temperament"])}</select></label>'
                f'<button class="primary" type="submit">保存</button>{resend}</form>')
    shown = lambda t: TEMPERAMENT_LABELS.get(t, t) if t else "—"  # noqa: E731
    return Safe(f"""{device_tabs(device_id, 'profile')}{notice}
<div class="card"><h2>宠物档案</h2><dl>
<dt>名字</dt><dd>{e(view['pet_name'] or '—')}</dd>
<dt>想要的性格</dt><dd>{e(shown(view['temperament']))}</dd>
<dt>设备上已确认的性格</dt><dd>{e(shown(view['temperament_confirmed']))}</dd>
<dt>设备最近上报</dt><dd>{e(shown(view['device_temperament']))}</dd></dl>
{command_line(cmd, tz)}{form}</div>""")


# -- memory ------------------------------------------------------------------

def memory_page(device: dict, view: dict, tz: str, csrf: str) -> Safe:
    device_id = device["device_id"]
    clear = view["memory_clear"]
    tomb = clear["state"] in {"pending", "rejected", "expired"}
    state_line = f'清空状态：{badge(CLEAR_LABELS.get(clear["state"], clear["state"]), "warn" if tomb else "muted")}'
    if tomb:
        state_line += ("（墓碑已启用：服务器暂不接收设备上传的记忆。）")
    if clear.get("command"):
        state_line += f' 回执：{status_badge(clear["command"]["status"])}'
    actions = ""
    if device["status"] == "active":
        if clear["state"] == "pending":
            actions = '<p class="muted">已向设备发出清空请求，请在设备上确认。</p>'
        else:
            actions = (f'<form method="post" action="/parent/devices/{e(device_id)}/memory/clear" '
                       f'data-confirm="服务器上的记忆和每日摘要会立即删除，并请求设备清空，确定吗？">{hidden(csrf)}'
                       f'<button class="danger" type="submit">清空记忆</button></form>')
        if clear.get("device_memory_not_cleared"):
            actions += (f'<form method="post" action="/parent/devices/{e(device_id)}/memory/resume">{hidden(csrf)}'
                        f'<p class="hint">设备没有清空记忆。恢复同步后，服务器会重新接收设备上的记忆。</p>'
                        f'<button type="submit">恢复同步</button></form>')
    warnings = "".join(f'<div class="flash warn">{e(w)}</div>' for w in view["warnings"])
    mem = view["memory"]
    if mem:
        likes = "".join(f"<li>{e(l['topic'] or '（未知）')}：{e(l['label'] or '（未知）')}</li>" for l in mem["likes"]) \
            or "<li class=muted>暂无</li>"
        books = "".join(f"<li>{e(b['title'] or '书 #' + str(b['content_id']))}</li>" for b in mem["recent_books"]) \
            or "<li class=muted>暂无</li>"
        miles = "".join(f"<li>{e(MILESTONE_LABELS.get(m, m))}</li>" for m in mem["growth"]["milestones"]) \
            or "<li class=muted>暂无</li>"
        g, t = mem["growth"], mem["today"]
        body = (f'<div class="card"><h2>记忆</h2><dl><dt>今日</dt><dd>喂食 {e(t["feeds"])} · 问候 {e(t["greetings"])} · '
                f'读书 {e(t["books"])} · 游戏 {e(t["games"])}</dd>'
                f'<dt>成长</dt><dd>读完 {e(g["books_finished"])} 本书 · 完成 {e(g["games_completed"])} 个游戏 · '
                f'活跃 {e(g["active_days"])} 天</dd></dl>'
                f'<h3>喜好</h3><ul>{likes}</ul><h3>最近读过</h3><ul>{books}</ul><h3>里程碑</h3><ul>{miles}</ul></div>')
    else:
        body = '<div class="card"><h2>记忆</h2><p class="muted">服务器上没有记忆。</p></div>'
    days = ""
    if view["days"]:
        rows = "".join(f"<tr><td>{e(clock(d['day'] * 86400, 'UTC')[:10])}</td><td>{e(d['feeds'])}</td>"
                       f"<td>{e(d['greetings'])}</td><td>{e(d['books'])}</td><td>{e(d['games'])}</td></tr>"
                       for d in view["days"])
        days = ('<div class="card"><h2>每日摘要</h2><div class="scroll"><table><thead><tr><th>日期</th><th>喂食</th>'
                f'<th>问候</th><th>读书</th><th>游戏</th></tr></thead><tbody>{rows}</tbody></table></div></div>')
    return Safe(f"""{device_tabs(device_id, 'memory')}{warnings}
<div class="card"><h2>清空记忆</h2><p class="hint">清空需要在设备上由家长确认后才会真正生效。</p>
<p>{state_line}</p>{actions}</div>{body}{days}""")


# -- notes -------------------------------------------------------------------

def notes_page(device: dict, notes: dict | None, tz: str, csrf: str, values: dict | None = None) -> Safe:
    device_id = device["device_id"]
    if notes is None:
        return Safe(f'{device_tabs(device_id, "notes")}<div class="card"><p class="muted">'
                    "未配置备注加密密钥（KID_PET_NOTES_KEY），备注功能不可用。</p></div>")
    v = values or {"nickname": notes["nickname"] or "", "birthday": notes["birthday"] or "",
                   "note": notes["note"] or ""}
    form = ""
    if device["status"] == "active":
        form = (f'<form method="post" action="/parent/devices/{e(device_id)}/notes">{hidden(csrf)}'
                f'<label>孩子昵称（最多 20 字）<input name="nickname" maxlength="20" value="{e(v["nickname"])}"></label>'
                f'<label>生日（月-日，如 03-15，不含年份）<input name="birthday" maxlength="5" value="{e(v["birthday"])}"></label>'
                f'<label>备注（最多 500 字）<textarea name="note" rows="5" maxlength="500">{e(v["note"])}</textarea></label>'
                f'<button class="primary" type="submit">保存备注</button></form>')
    delete = ""
    if notes["exists"]:
        delete = (f'<form method="post" action="/parent/devices/{e(device_id)}/notes/delete" '
                  f'data-confirm="确定删除这份备注吗？">{hidden(csrf)}'
                  f'<button class="danger" type="submit">删除备注</button></form>')
    return Safe(f"""{device_tabs(device_id, 'notes')}
<div class="card"><h2>家长备注</h2>
<p class="hint">备注加密存储，只在家长面板里读写，不会同步给设备或语音服务。</p>
<p>最近更新：{e(clock(notes['updated_at'], tz))}</p>{form}</div>
<div class="card">{delete or '<p class="muted">尚无备注。</p>'}</div>""")


# -- audit -------------------------------------------------------------------

def audit_page(rows: list[dict], tz: str, page: int, more: bool) -> Safe:
    if not rows:
        return Safe('<div class="card"><p class="muted">暂无记录。</p></div>')
    body = "".join(
        f"<tr><td>{e(clock(r['at'], tz))}</td><td>{e(r['actor'])}</td><td>{e(r['action'])}</td>"
        f"<td>{e(r['target'])}</td><td>{e(r['result'])}</td><td>{e(r['ip'] or '—')}</td></tr>" for r in rows)
    pager = []
    if page > 1:
        pager.append(f'<a href="/parent/audit?page={page - 1}">上一页</a>')
    if more:
        pager.append(f'<a href="/parent/audit?page={page + 1}">下一页</a>')
    return Safe('<div class="card"><p class="hint">只读。仅记录操作类型，不含密码、令牌或备注内容。</p><div class="scroll"><table>'
                '<thead><tr><th>时间</th><th>操作者</th><th>操作</th><th>对象</th><th>结果</th><th>IP</th></tr></thead>'
                f'<tbody>{body}</tbody></table></div><p class="pager">{" · ".join(pager)}</p></div>')
