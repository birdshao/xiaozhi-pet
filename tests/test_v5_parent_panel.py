"""Parent web panel (/parent/*): login, session, CSRF, every page and action, XSS escaping."""
from pathlib import Path
import re
import sys
import unittest
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from s3_support import DEVICE, PASSWORD, TOKEN, S3Base  # noqa: E402
from services.pet_learning.auth import ParentAuth  # noqa: E402
from services.pet_learning.totp import code_at  # noqa: E402

BASE = f"/parent/devices/{DEVICE}"
XSS = "<script>alert(1)</script>"


def field(html, name):
    return re.search(rf'name="{name}" value="([^"]*)"', html).group(1)


class PanelBase(S3Base):
    """Account exists; `signin()` logs in through the HTML form."""

    def setUp(self):
        super().setUp()
        self.app.state.parent_panel.clock = lambda: self.now
        self.auth = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        self.auth.create_account("parent", PASSWORD, self.secret, code_at(self.secret, self.now))
        self.app.state.parent_auth = self.auth
        self.now += 30

    def get(self, url, **kw):
        return self.client.get(url, follow_redirects=False, **kw)

    def form_login(self, *, password=PASSWORD, otp=None, username="parent", csrf=True):
        page = self.get("/parent/login")
        self.assertEqual(page.status_code, 200)
        data = {"username": username, "password": password,
                "otp": otp if otp is not None else code_at(self.secret, self.now),
                "csrf": field(page.text, "csrf") if csrf else ""}
        return self.client.post("/parent/login", data=data, follow_redirects=False)

    def signin(self):
        result = self.form_login()
        self.assertEqual(result.status_code, 303, result.text)
        self.csrf = self.client.cookies.get("__Host-pet_csrf")
        self.assertTrue(self.csrf)

    def post(self, url, data=None, *, csrf=True, headers=None):
        data = dict(data or {})
        if csrf:
            data.setdefault("csrf", self.csrf)
        return self.client.post(url, data=data, follow_redirects=False, headers=headers or {})

    def expire_commands(self):
        self.table("SELECT 1")
        from services.pet_learning.db import connect
        with connect(self.path) as db:
            db.execute("UPDATE commands SET expires_at=? WHERE state='pending'", (self.now - 1,))

    def with_state(self):
        self.put_state(self.snapshot())


class LoginTests(PanelBase):
    def test_pages_redirect_to_login_when_anonymous(self):
        for url in ("/parent", "/parent/", "/parent/devices", BASE, f"{BASE}/profile", f"{BASE}/memory",
                    f"{BASE}/notes", "/parent/pairing", "/parent/audit", "/parent/pairings/" + "a" * 24):
            result = self.get(url)
            self.assertEqual((result.status_code, result.headers["location"].split("?")[0]),
                             (303, "/parent/login") if url not in {"/parent", "/parent/"} else (303, "/parent/devices"),
                             url)
        for url in (f"{BASE}/revoke", "/parent/pairings", f"{BASE}/profile", f"{BASE}/profile/init",
                    f"{BASE}/memory/clear", f"{BASE}/memory/resume", f"{BASE}/notes", f"{BASE}/notes/delete",
                    "/parent/logout"):
            result = self.client.post(url, data={"csrf": "x"}, follow_redirects=False)
            self.assertEqual((result.status_code, result.headers["location"]), (303, "/parent/login"), url)

    def test_login_page_is_hardened(self):
        page = self.get("/parent/login")
        self.assertEqual(page.status_code, 200)
        self.assertEqual(page.headers["cache-control"], "no-store")
        self.assertIn("default-src 'none'", page.headers["content-security-policy"])
        self.assertIn("script-src 'self'", page.headers["content-security-policy"])
        self.assertNotIn("unsafe-inline", page.headers["content-security-policy"])
        self.assertEqual(page.headers["x-frame-options"], "DENY")
        # "no-referrer" would make browsers send "Origin: null" on our own form POSTs.
        self.assertEqual(page.headers["referrer-policy"], "same-origin")
        self.assertNotIn("<script", page.text)  # no inline script; password never echoed
        cookie = page.headers["set-cookie"]
        self.assertIn("HttpOnly", cookie)
        self.assertIn("Secure", cookie)
        self.assertIn("SameSite=strict", cookie)
        self.assertNotIn("register", page.text.lower().replace("不提供注册", ""))

    def test_login_success_sets_session_cookies(self):
        result = self.form_login()
        self.assertEqual((result.status_code, result.headers["location"]), (303, "/parent/devices"))
        cookies = result.headers.get_list("set-cookie")
        session = next(c for c in cookies if c.startswith("__Host-pet_parent="))
        for flag in ("HttpOnly", "Secure", "SameSite=strict", "Path=/"):
            self.assertIn(flag, session)
        csrf = next(c for c in cookies if c.startswith("__Host-pet_csrf="))
        self.assertNotIn("HttpOnly", csrf)
        self.assertEqual(self.get("/parent/devices").status_code, 200)

    def test_wrong_password_and_wrong_totp(self):
        for kwargs in ({"password": "wrong-password-0000"}, {"otp": "000000"}, {"otp": "12345"},
                       {"username": "nobody"}):
            result = self.form_login(**kwargs)
            self.assertEqual(result.status_code, 401, kwargs)
            self.assertNotIn("__Host-pet_parent", result.headers.get("set-cookie", ""))
            self.assertIn("不正确", result.text)
            self.now += 1
        self.assertEqual(self.get("/parent/devices").status_code, 303)

    def test_totp_replay_refused(self):
        otp = code_at(self.secret, self.now)
        self.assertEqual(self.form_login(otp=otp).status_code, 303)
        self.client.cookies.clear()
        self.assertEqual(self.form_login(otp=otp).status_code, 401)

    def test_rate_limit(self):
        for _ in range(5):
            self.assertEqual(self.form_login(otp="000000").status_code, 401)
        locked = self.form_login()  # even the right credentials are refused now
        self.assertEqual(locked.status_code, 429)
        self.assertIn("次数过多", locked.text)
        self.assertEqual(self.get("/parent/devices").status_code, 303)

    def test_login_csrf(self):
        self.assertEqual(self.form_login(csrf=False).status_code, 403)
        page = self.get("/parent/login")
        token = field(page.text, "csrf")
        self.client.cookies.clear()  # the token without its cookie is worthless
        result = self.client.post("/parent/login", follow_redirects=False, data={
            "username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now), "csrf": token})
        self.assertEqual(result.status_code, 403)
        self.assertNotIn("__Host-pet_parent", result.headers.get("set-cookie", ""))

    def test_login_origin_and_media_type(self):
        page = self.get("/parent/login")
        data = {"username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now),
                "csrf": field(page.text, "csrf")}
        evil = self.client.post("/parent/login", data=data, headers={"Origin": "https://evil.example"},
                                follow_redirects=False)
        self.assertEqual(evil.status_code, 403)
        null = self.client.post("/parent/login", data=data, headers={"Origin": "null"}, follow_redirects=False)
        self.assertEqual(null.status_code, 403)
        as_json = self.client.post("/parent/login", json=data, follow_redirects=False)
        self.assertEqual(as_json.status_code, 415)

    def test_recovery_code_login(self):
        auth = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        with self.assertRaises(ValueError):
            auth.create_account("again", PASSWORD, self.secret, code_at(self.secret, self.now))
        from services.pet_learning.db import connect
        with connect(self.path) as db:  # recovery codes are stored hashed: add one we know
            import hashlib
            code = "a" * 32
            db.execute("INSERT INTO parent_recovery_codes VALUES(1,?)", (hashlib.sha256(code.encode()).hexdigest(),))
        self.assertEqual(self.form_login(otp=code).status_code, 303)

    def test_logout(self):
        self.signin()
        self.assertEqual(self.post("/parent/logout", csrf=False).status_code, 403)
        result = self.post("/parent/logout")
        self.assertEqual((result.status_code, result.headers["location"].split("?")[0]), (303, "/parent/login"))
        self.assertEqual(self.get("/parent/devices").status_code, 303)
        self.assertIn("已退出", self.get("/parent/login?msg=logged_out").text)


class CsrfTests(PanelBase):
    def setUp(self):
        super().setUp()
        self.signin()

    def test_every_write_requires_csrf(self):
        urls = [f"{BASE}/revoke", "/parent/pairings", f"{BASE}/profile", f"{BASE}/profile/init",
                f"{BASE}/memory/clear", f"{BASE}/memory/resume", f"{BASE}/notes", f"{BASE}/notes/delete",
                "/parent/pairings/" + "a" * 24 + "/redeem"]
        for url in urls:
            for data in ({}, {"csrf": "wrong"}, {"csrf": self.csrf[:-1] + "x"}):
                result = self.client.post(url, data=data, follow_redirects=False)
                self.assertEqual(result.status_code, 403, (url, data))
        self.assertEqual(self.table("SELECT status FROM devices WHERE device_id=?", DEVICE), [("active",)])

    def test_cross_origin_post_rejected(self):
        result = self.post("/parent/pairings", headers={"Origin": "https://evil.example"})
        self.assertEqual(result.status_code, 403)
        self.assertEqual(self.table("SELECT COUNT(*) FROM pairing_codes"), [(0,)])
        self.assertEqual(self.get("/parent/devices", headers={"Origin": "https://evil.example"}).status_code, 403)

    def test_forms_carry_the_csrf_token(self):
        html = self.get(BASE).text
        self.assertEqual(field(html, "csrf"), self.csrf)

    def test_non_form_body_rejected(self):
        result = self.client.post("/parent/pairings", json={"csrf": self.csrf}, follow_redirects=False)
        self.assertEqual(result.status_code, 415)
        big = self.client.post("/parent/pairings", content=b"csrf=" + b"a" * 20000, follow_redirects=False,
                               headers={"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(big.status_code, 413)


class PageTests(PanelBase):
    def setUp(self):
        super().setUp()
        self.signin()
        self.with_state()

    def assert_page(self, url, *texts):
        result = self.get(url)
        self.assertEqual(result.status_code, 200, url)
        self.assertEqual(result.headers["cache-control"], "no-store")
        self.assertIn("default-src 'none'", result.headers["content-security-policy"])
        self.assertTrue(result.headers["content-type"].startswith("text/html"))
        for text in texts:
            self.assertIn(text, result.text, url)
        return result.text

    def test_device_list_and_overview(self):
        html = self.assert_page("/parent/devices", DEVICE, "64%", "开心", "pet-002", "最近活跃")
        self.assertIn(f"/parent/devices/{DEVICE}", html)
        html = self.assert_page(BASE, "64%", "空闲", "pet-2026.11.04", "命令回执")
        self.assertIn("撤销此设备", html)
        self.assertEqual(self.get("/parent/devices/nope-device").status_code, 404)
        self.assertEqual(self.get("/parent/devices/a%2F..%2Fb").status_code, 404)

    def test_online_state_follows_last_seen(self):
        from services.pet_learning.db import connect
        with connect(self.path) as db:
            db.execute("UPDATE devices SET last_seen_at=? WHERE device_id=?", (self.now - 60, DEVICE))
        self.assertIn("在线", self.get("/parent/devices").text)
        with connect(self.path) as db:
            db.execute("UPDATE devices SET last_seen_at=? WHERE device_id=?", (self.now - 7200, DEVICE))
        self.assertIn("离线", self.get("/parent/devices").text)

    def test_pairing_flow(self):
        self.assert_page("/parent/pairing", "开始配对")
        started = self.post("/parent/pairings")
        self.assertEqual(started.status_code, 303)
        location = started.headers["location"]
        pairing_id = location.rsplit("/", 1)[1]
        self.assertIn("等待设备发起配对", self.assert_page(location))
        device = self.client.post("/v1/pairing/start", json={"device_id": "pet-new-01"})
        self.assertEqual(device.status_code, 200, device.text)
        code, claim = device.json()["code"], device.json()["claim_secret"]
        self.assertIn("请输入", self.assert_page(location))
        wrong = self.post(f"/parent/pairings/{pairing_id}/redeem", {"code": "000000" if code != "000000" else "111111"})
        self.assertEqual(wrong.status_code, 400)
        self.assertIn("配对码不正确", wrong.text)
        self.assertEqual(self.post(f"/parent/pairings/{pairing_id}/redeem", {"code": "12ab"}).status_code, 422)
        ok = self.post(f"/parent/pairings/{pairing_id}/redeem", {"code": code})
        self.assertEqual(ok.status_code, 303)
        self.assertIn("配对码正确", self.assert_page(location + "?msg=pairing_approved"))
        claimed = self.client.post("/v1/pairing/claim", json={"device_id": "pet-new-01", "claim_secret": claim})
        self.assertEqual(claimed.json()["status"], "claimed")
        self.assertEqual(self.get("/parent/pairings/" + "z" * 24).status_code, 404)

    def test_pairing_voids_after_wrong_codes(self):
        pairing_id = self.post("/parent/pairings").headers["location"].rsplit("/", 1)[1]
        code = self.client.post("/v1/pairing/start", json={"device_id": "pet-new-02"}).json()["code"]
        wrong = "000000" if code != "000000" else "111111"
        statuses = [self.post(f"/parent/pairings/{pairing_id}/redeem", {"code": wrong}).status_code for _ in range(5)]
        self.assertEqual(statuses[-1], 410)
        self.assertIn("已作废", self.get(f"/parent/pairings/{pairing_id}").text)

    def test_revoke(self):
        result = self.post(f"{BASE}/revoke")
        self.assertEqual((result.status_code, result.headers["location"].split("?")[0]), (303, BASE))
        self.assertIn("已撤销", self.assert_page(BASE))
        self.assertNotIn("撤销此设备", self.get(BASE).text)
        self.assertEqual(self.post("/parent/devices/missing-device/revoke").status_code, 404)
        denied = self.client.get(f"/v1/devices/{DEVICE}/commands", headers={"Authorization": f"Bearer {TOKEN}"})
        self.assertEqual(denied.status_code, 401)

    def test_profile_init_update_and_confirmation_notice(self):
        html = self.assert_page(f"{BASE}/profile", "初始化档案", "必须由家长在设备上确认")
        bad = self.post(f"{BASE}/profile/init", {"pet_name": "糖糖1", "temperament": "gentle"})
        self.assertEqual(bad.status_code, 422)
        self.assertEqual(self.post(f"{BASE}/profile/init", {"pet_name": "糖糖", "temperament": "evil"}).status_code, 422)
        ok = self.post(f"{BASE}/profile/init", {"pet_name": "糖糖", "temperament": "gentle"})
        self.assertEqual(ok.status_code, 303)
        self.assertEqual(self.post(f"{BASE}/profile/init", {"pet_name": "糖糖", "temperament": "gentle"}).status_code, 409)
        html = self.assert_page(f"{BASE}/profile", "糖糖", "等待设备获取")
        rev = field(html, "base_rev")
        changed = self.post(f"{BASE}/profile", {"base_rev": rev, "pet_name": "糖糖", "temperament": "lively"})
        self.assertEqual(changed.status_code, 303)
        kinds = self.table("SELECT kind,json_extract(payload_json,'$.temperament') FROM commands "
                           "WHERE device_id=? AND kind='SET_TEMPERAMENT' AND state='pending'", DEVICE)
        self.assertEqual(kinds, [("SET_TEMPERAMENT", "lively")])
        html = self.assert_page(f"{BASE}/profile", "活泼")
        # a name change alone never re-sends the temperament
        before = self.table("SELECT COUNT(*) FROM commands WHERE kind='SET_TEMPERAMENT'")
        self.post(f"{BASE}/profile", {"base_rev": field(html, "base_rev"), "pet_name": "豆豆", "temperament": "lively"})
        self.assertEqual(self.table("SELECT COUNT(*) FROM commands WHERE kind='SET_TEMPERAMENT'"), before)
        self.assert_page(f"{BASE}/profile", "豆豆")
        stale = self.post(f"{BASE}/profile", {"base_rev": rev, "pet_name": "糖糖", "temperament": "curious"})
        self.assertEqual(stale.status_code, 409)
        self.assertIn("刷新页面", stale.text)
        self.assertEqual(self.post(f"{BASE}/profile", {"base_rev": "x", "pet_name": "糖糖"}).status_code, 422)
        # no voice pack or settings command can come out of the panel
        self.assertEqual(self.table("SELECT COUNT(*) FROM commands WHERE kind IN "
                                    "('SELECT_VOICE_PACK','SET_PARENT_SETTINGS','SYNC_CONTENT')"), [(0,)])

    def test_temperament_receipt_states_shown(self):
        self.post(f"{BASE}/profile/init", {"pet_name": "糖糖", "temperament": "lively"})
        command = self.app.state.profile_service.get_profile(DEVICE)["commands"]["temperament"]["command_id"]
        self.put_state(self.snapshot(awaiting=[command]))
        self.assert_page(f"{BASE}/profile", "等待设备确认")
        self.assert_page(BASE, "等待设备确认", "修改性格")
        self.put_state(self.snapshot(receipts=[(command, "rejected")]))
        html = self.assert_page(f"{BASE}/profile", "被拒绝", "重新请求设备确认")
        resend = self.post(f"{BASE}/profile", {"base_rev": field(html, "base_rev"), "pet_name": "糖糖",
                                               "temperament": "lively", "resend": "1"})
        self.assertEqual(resend.status_code, 303)
        self.assertEqual(self.table("SELECT COUNT(*) FROM commands WHERE kind='SET_TEMPERAMENT' AND state='pending'"),
                         [(1,)])
        pending = self.app.state.profile_service.get_profile(DEVICE)["commands"]["temperament"]["command_id"]
        self.put_state(self.snapshot(receipts=[(pending, "applied")]))
        self.assert_page(f"{BASE}/profile", "已生效")
        self.assert_page(BASE, "已生效", "被拒绝")

    def test_command_expiry_shown(self):
        self.post(f"{BASE}/profile/init", {"pet_name": "糖糖", "temperament": "lively"})
        self.expire_commands()
        self.assert_page(BASE, "已过期")

    def test_memory_clear_tombstone_and_resume(self):
        self.assert_page(f"{BASE}/memory", "清空记忆", "设备上由家长确认")
        cleared = self.post(f"{BASE}/memory/clear")
        self.assertEqual(cleared.status_code, 303)
        html = self.assert_page(f"{BASE}/memory", "等待设备确认", "墓碑")
        self.assertNotIn("恢复同步", html)
        self.assertEqual(self.table("SELECT COUNT(*) FROM memory_current"), [(0,)])
        command = self.table("SELECT command_id FROM commands WHERE kind='MEMORY_CLEAR'")[0][0]
        self.put_state(self.snapshot(receipts=[(command, "rejected")]))
        html = self.assert_page(f"{BASE}/memory", "设备已拒绝", "恢复同步", "被拒绝")
        self.assertEqual(self.post(f"{BASE}/memory/resume").status_code, 303)
        html = self.assert_page(f"{BASE}/memory")
        self.assertNotIn("恢复同步", html)
        self.assertEqual(self.post(f"{BASE}/memory/resume").status_code, 409)

    def test_memory_clear_applied_and_expired(self):
        self.post(f"{BASE}/memory/clear")
        command = self.table("SELECT command_id FROM commands WHERE kind='MEMORY_CLEAR'")[0][0]
        self.put_state(self.snapshot(receipts=[(command, "applied")]))
        self.assert_page(f"{BASE}/memory", "已清空", "已生效")
        other = self.post(f"{BASE}/memory/clear")  # a second clear is allowed after the first completed
        self.assertEqual(other.status_code, 303)
        self.expire_commands()
        self.assert_page(f"{BASE}/memory", "已过期", "恢复同步")

    def test_memory_content_rendered(self):
        html = self.assert_page(f"{BASE}/memory", "记忆", "今日")
        self.assertIn("喂食", html)

    def test_notes_read_write_delete(self):
        self.assert_page(f"{BASE}/notes", "加密存储", "尚无备注")
        fields = {"nickname": "小满", "birthday": "03-15", "note": "怕黑\n喜欢恐龙"}
        saved = self.post(f"{BASE}/notes", fields)
        self.assertEqual(saved.status_code, 303, saved.text)
        html = self.assert_page(f"{BASE}/notes", "小满", "03-15", "怕黑", "喜欢恐龙")
        raw = self.table("SELECT payload_enc FROM parent_notes WHERE device_id=?", DEVICE)[0][0]
        self.assertNotIn("怕黑", raw)  # encrypted at rest
        self.assertIn("notes.update", [r[0] for r in self.table("SELECT action FROM audit_log")])
        self.assertNotIn("怕黑", str(self.table("SELECT * FROM audit_log")))
        gone = self.post(f"{BASE}/notes/delete")
        self.assertEqual(gone.status_code, 303)
        self.assert_page(f"{BASE}/notes", "尚无备注")
        self.assertEqual(self.table("SELECT COUNT(*) FROM parent_notes"), [(0,)])

    def test_notes_validation_errors_keep_the_form(self):
        bad = self.post(f"{BASE}/notes", {"nickname": "x" * 21, "birthday": "02-30", "note": ""})
        self.assertEqual(bad.status_code, 422)
        self.assertIn("备注内容不合法", bad.text)
        self.assertEqual(self.post(f"{BASE}/notes", {"nickname": "", "birthday": "", "note": ""}).status_code, 422)
        self.assertEqual(self.table("SELECT COUNT(*) FROM parent_notes"), [(0,)])

    def test_notes_without_key(self):
        self.settings = self.app.state.settings
        object.__setattr__(self.settings, "notes_key", "")
        self.assertIn("不可用", self.assert_page(f"{BASE}/notes"))

    def test_audit_log_read_only(self):
        self.post(f"{BASE}/memory/clear")
        html = self.assert_page("/parent/audit", "memory.clear", "parent.login", "只读")
        self.assertNotIn("<form", html.split("<main")[1])
        for method in ("post", "put", "delete"):
            self.assertIn(getattr(self.client, method)("/parent/audit", follow_redirects=False).status_code,
                          {403, 405, 415})
        from services.pet_learning.db import connect
        with connect(self.path) as db:
            db.executemany("INSERT INTO audit_log(at,actor,action,target,result,ip) VALUES(?,?,?,?,?,?)",
                           [(self.now, "x", "bulk.row", f"t{i}", "ok", None) for i in range(120)])
        page1 = self.assert_page("/parent/audit", "下一页")
        page3 = self.assert_page("/parent/audit?page=3")
        self.assertNotIn("下一页", page3)
        self.assertEqual(self.get("/parent/audit?page=abc").status_code, 200)

    def test_firmware_unsupported_features_absent(self):
        for url in ("/parent/devices", BASE, f"{BASE}/profile", f"{BASE}/memory", f"{BASE}/notes", "/parent/audit"):
            text = self.get(url).text
            # Open chat got its own tab in S6 (/chat, tests/test_v6_open_chat.py); the rest stays absent.
            for word in ("声音包", "内容包", "open_chat", "SELECT_VOICE_PACK", "SET_PARENT_SETTINGS"):
                self.assertNotIn(word, text.replace("<title>", ""), (url, word))
        for url in (f"{BASE}/voice", f"{BASE}/content", f"{BASE}/settings", f"{BASE}/policy"):
            self.assertEqual(self.get(url).status_code, 404)

    def test_static_assets(self):
        css = self.get("/parent/static/panel.css")
        self.assertEqual((css.status_code, css.headers["content-type"].split(";")[0]), (200, "text/css"))
        js = self.get("/parent/static/panel.js")
        self.assertEqual(js.status_code, 200)
        self.assertEqual(js.headers["cache-control"], "no-store")
        self.assertEqual(self.get("/parent/static/..%2Fviews.py").status_code, 404)
        self.assertEqual(self.get("/parent/static/views.py").status_code, 404)


class XssTests(PanelBase):
    def setUp(self):
        super().setUp()
        self.signin()
        self.with_state()

    def test_stored_values_are_escaped(self):
        from services.pet_learning.db import connect
        payloads = ['<img src=x onerror=alert(1)>', '"><script>alert(2)</script>', "'/><svg onload=alert(3)>"]
        with connect(self.path) as db:
            db.execute("UPDATE devices SET fw_version=?, last_seen_ip=? WHERE device_id=?",
                       (payloads[0], payloads[1], DEVICE))
            db.execute("UPDATE device_state SET fw_version=? WHERE device_id=?", (payloads[0], DEVICE))
            db.execute("INSERT INTO audit_log(at,actor,action,target,result,ip) VALUES(?,?,?,?,?,?)",
                       (self.now, payloads[0], payloads[1], payloads[2], payloads[0], payloads[1]))
        notes = self.post(f"{BASE}/notes", {"nickname": "<b>x</b>&amp;", "birthday": "", "note": payloads[1] + payloads[2],
                                            })
        self.assertEqual(notes.status_code, 303)
        for url in ("/parent/devices", BASE, "/parent/audit", f"{BASE}/notes"):
            html = self.get(url).text
            for raw in ("<img src=x", "<script>alert", "<svg onload", "<b>x</b>"):
                self.assertNotIn(raw, html, (url, raw))
        audit = self.get("/parent/audit").text
        self.assertIn("&lt;img src=x onerror=alert(1)&gt;", audit)
        self.assertIn("&quot;&gt;&lt;script&gt;alert(2)&lt;/script&gt;", audit)
        self.assertIn("&lt;b&gt;x&lt;/b&gt;&amp;amp;", self.get(f"{BASE}/notes").text)

    def test_rejected_notes_form_echoes_escaped(self):
        bad = self.post(f"{BASE}/notes", {"nickname": "<i>" + "a" * 20 + "</i>", "birthday": "", "note": XSS})
        self.assertEqual(bad.status_code, 422)
        self.assertNotIn("<script>", bad.text)
        self.assertNotIn("<i>a", bad.text)
        self.assertIn("&lt;script&gt;alert(1)&lt;/script&gt;", bad.text)

    def test_path_and_query_input_never_reflected(self):
        for url in (f"/parent/devices/{quote(XSS)}", f"/parent/pairings/{quote(XSS)}",
                    f"/parent/devices?msg={quote(XSS)}", f"{BASE}?msg={quote(XSS)}", f"/parent/audit?page={quote(XSS)}"):
            text = self.get(url).text
            self.assertNotIn("<script>alert", text, url)
            self.assertNotIn("alert(1)", text, url)
        self.assertIn("已撤销", self.get(f"{BASE}?msg=revoked").text)  # known keys only

    def test_form_errors_do_not_reflect_input(self):
        bad = self.post(f"{BASE}/profile/init", {"pet_name": XSS, "temperament": XSS})
        self.assertNotIn("alert(1)", bad.text)
        self.assertNotIn("alert(1)", self.post("/parent/pairings/" + "a" * 24 + "/redeem", {"code": XSS}).text)

    def test_login_error_does_not_reflect_input(self):
        page = self.client.get("/parent/login")
        self.client.cookies.clear()
        page = self.client.get("/parent/login")
        result = self.client.post("/parent/login", follow_redirects=False, data={
            "username": XSS, "password": XSS, "otp": XSS, "csrf": field(page.text, "csrf")})
        self.assertEqual(result.status_code, 401)
        self.assertNotIn("alert(1)", result.text)


class OtherRoutesUnaffected(PanelBase):
    def test_json_api_and_device_routes_still_work(self):
        self.assertEqual(self.get("/healthz").json(), {"status": "ok"})
        self.assertEqual(self.get("/v1/parent/devices").status_code, 401)  # JSON API: no redirect, no HTML
        self.assertEqual(self.get("/v1/parent/devices").headers["content-type"], "application/json")
        self.assertEqual(self.client.get(f"/v1/devices/{DEVICE}/commands").status_code, 401)
        ok = self.client.get(f"/v1/devices/{DEVICE}/commands", headers={"Authorization": f"Bearer {TOKEN}"})
        self.assertEqual(ok.status_code, 200)
        self.put_state(self.snapshot())
        voice = self.client.post(f"/v1/voice/{DEVICE}/turn", json={"text": "hi"})
        self.assertIn(voice.status_code, {401, 403})
        self.assertEqual(voice.headers["content-type"], "application/json")
        # a panel session grants nothing on device or voice routes
        self.signin()
        self.assertEqual(self.client.get(f"/v1/devices/{DEVICE}/commands").status_code, 401)
        self.assertNotIn("content-security-policy", self.get("/healthz").headers)

    def test_json_login_still_works_beside_the_panel(self):
        result = self.client.post("/v1/parent/login", json={
            "username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now)})
        self.assertEqual(result.status_code, 200)
        self.assertEqual(self.get("/parent/devices").status_code, 200)  # same session cookie


if __name__ == "__main__":
    unittest.main()
