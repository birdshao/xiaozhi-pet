"""Unit tests for tools/voice_probe.py (parsing, judging, report; no hardware, no network)."""
from __future__ import annotations

import datetime as dt
import importlib.util
import sys
from pathlib import Path

import pytest

_SPEC = importlib.util.spec_from_file_location(
    "voice_probe", Path(__file__).resolve().parents[1] / "tools" / "voice_probe.py")
vp = importlib.util.module_from_spec(_SPEC)
sys.modules["voice_probe"] = vp
_SPEC.loader.exec_module(vp)


def test_shipped_cases_load_and_cover_required_groups():
    cases = vp.load_cases(vp.DEFAULT_CASES)
    says = {c.say for c in cases}
    for s in ("团团睡觉", "几点了", "还有多少电", "讲故事", "下一页", "上一页", "喂团团", "玩游戏", "回首页"):
        assert s in says
    assert {"command", "chat", "nonsense", "quiet"} <= {c.group for c in cases}


@pytest.mark.parametrize("text,msg", [
    ('[[case]]\nid="a"\nsay="x"\nbogus=1', "unknown keys"),
    ('[[case]]\nid="a"\nsay="x"\n[[case]]\nid="a"\nsay="y"', "duplicate"),
    ('[[case]]\nid="a"\nsay="x"\nkind="llm"', "kind"),
    ('[[case]]\nid="a"\nsay="x"\nquiet="maybe"', "quiet"),
    ('x=1', "no .*case"),
])
def test_parse_cases_rejects_bad_input(text, msg):
    with pytest.raises(ValueError, match=msg):
        vp.parse_cases(text)


def test_quiet_window_wraps_midnight():
    w = vp.parse_quiet_window("I (1) x: PET_PARENT_SETTINGS src=ledger wake=07:00 quiet=20:30-07:00 nap=0")
    assert w == (20 * 60 + 30, 7 * 60)

    def at(h, m):
        return dt.datetime(2026, 11, 8, h, m)

    assert vp.in_quiet(w, at(21, 0)) and vp.in_quiet(w, at(3, 0)) and vp.in_quiet(w, at(20, 30))
    assert not vp.in_quiet(w, at(7, 0)) and not vp.in_quiet(w, at(13, 0))
    assert vp.in_quiet(None, at(1, 0)) is None


def test_case_applicable():
    only = vp.Case(id="q", say="x", quiet="only")
    never = vp.Case(id="n", say="x", quiet="never")
    assert vp.case_applicable(only, True)[0] and not vp.case_applicable(only, False)[0]
    assert vp.case_applicable(never, False)[0] and not vp.case_applicable(never, True)[0]
    assert vp.case_applicable(only, None)[0]


def test_parse_docker_ts():
    t, msg = vp.parse_docker_ts("2026-11-08T13:21:05.250000000Z hello")
    assert msg == "hello" and abs(t % 60 - 5.25) < 1e-6
    assert vp.parse_docker_ts("garbage") is None


def test_parse_xiaozhi_and_learning_lines():
    ev = vp.parse_xiaozhi_line(1.0, '[x]-INFO-收到listen消息：{"type":"listen","state":"detect","text":"你好小智"}')
    assert ev["ev"] == "wake" and ev["text"] == "你好小智"
    assert vp.parse_xiaozhi_line(2.0, "[a]-INFO-发送第一段语音: 好的，我们回首页。")["text"] == "好的，我们回首页。"
    assert vp.parse_xiaozhi_line(2.0, "[a]-INFO-识别文本: 回首页")["ev"] == "asr"
    assert vp.parse_xiaozhi_line(2.0, "nothing") is None
    ev = vp.parse_learning_line(3.0, "INFO:pet_learning.voice:voice_turn kind=fixed reason=tell_time ms=12")
    assert (ev["kind"], ev["reason"], ev["ms"]) == ("fixed", "tell_time", 12)


def test_parse_serial_line():
    ev = vp.parse_serial_line(1, "I (9) w: PET_VOICE_CMD name=GO_HOME result=done action=home hint=none scene=home")
    assert (ev["name_"], ev["result"]) == ("GO_HOME", "done")
    assert vp.parse_serial_line(1, "I (9) Application: >> 回首页")["ev"] == "stt"
    assert vp.parse_serial_line(1, "I (9) Application: STATE: speaking")["text"] == "speaking"
    assert vp.parse_serial_line(1, "I (9) boring") is None


def _obs(**kw):
    base = dict(speech_end=100.0, asr_texts=[(101.0, "回首页")], kind="local_command", reason="fixed_command",
                tts_text="我听到了", t_tts_server=101.5)
    base.update(kw)
    return vp.Observation(**base)


def test_judge_pass():
    case = vp.Case(id="home", say="回首页", kind="local_command", reason="fixed_command", action="GO_HOME")
    r = vp.judge(case, _obs(device_cmds=[("GO_HOME", "done", "home")]))
    assert r["passed"], r


def test_judge_nothing_is_worst():
    r = vp.judge(vp.Case(id="x", say="回首页", kind="local_command"), vp.Observation(speech_end=1.0))
    assert not r["passed"] and vp.worst_severity(r["issues"]) == "none"


def test_judge_asr_wrong_action_missing_and_slow():
    case = vp.Case(id="home", say="回首页", kind="local_command", action="GO_HOME")
    r = vp.judge(case, _obs(asr_texts=[(101, "会收野")], t_tts_server=106.0))
    assert {"asr", "action", "slow"} <= {s for s, _ in r["issues"]}
    assert vp.worst_severity(r["issues"]) == "asr"


def test_judge_stray_action_on_chat():
    case = vp.Case(id="c", say="我不想睡觉", kind="fallback")
    obs = _obs(asr_texts=[(101, "我不想睡觉")], kind="fallback", reason="no_match",
               device_cmds=[("SLEEP_REQUEST", "done", "")])
    assert any("不该有设备动作" in t for _, t in vp.judge(case, obs)["issues"])


def test_wake_phrase_is_not_taken_as_the_sentence():
    obs = _obs(asr_texts=[(100.5, "你好小智。")])
    assert vp.judge(vp.Case(id="x", say="回首页"), obs)["asr"] == ""


def test_quiet_overrides():
    case = vp.Case(id="story", say="讲故事", kind="local_command", action="OPEN_READER",
                   kind_quiet="fixed", reason_quiet="quiet_hours", action_quiet="none")
    obs = _obs(asr_texts=[(101, "讲故事")], kind="fixed", reason="quiet_hours")
    assert vp.judge(case, obs, quiet=True)["passed"]
    assert not vp.judge(case, obs, quiet=False)["passed"]


def test_build_observation_windows_and_latency():
    xz = [{"t": 90.0, "ev": "tts_first", "text": "旧的"},
          {"t": 101.0, "ev": "asr", "text": "几点了"},
          {"t": 102.0, "ev": "tts_first", "text": "现在是9点整。"}]
    pl = [{"t": 101.5, "ev": "turn", "kind": "fixed", "reason": "tell_time", "ms": 5}]
    ser = [{"t": 103.0, "ev": "state", "text": "speaking"}]
    obs = vp.build_observation(99.0, 100.0, 110.0, xz, pl, ser)
    assert obs.tts_text == "现在是9点整。" and obs.kind == "fixed"
    assert vp.latency(obs) == 2.0


def test_redact_removes_tokens():
    s = vp.redact("headers: {'authorization': 'Bearer 4xVoQRJqTeji9PRaAtNNhsbmAa1IEs', 'client-id': 'abc'}")
    assert "4xVoQ" not in s and "client-id" in s
    assert "hunter22" not in vp.redact("password=hunter22")
    assert "SECRETVALUE" not in vp.redact("KID_PET_VOICE_TOKEN=SECRETVALUE123")


def test_render_report_orders_issues_by_severity():
    case = vp.Case(id="a", say="回首页", kind="local_command", action="GO_HOME")
    slow = vp.judge(case, _obs(device_cmds=[("GO_HOME", "done", "")], t_tts_server=105.0))
    bad = vp.judge(case, vp.Observation(speech_end=1.0))
    rows = [{"id": "a", "say": "回首页", "status": "ran", "asr": "回首页", "kind": "x", "reason": "y",
             "tts_text": "t", "device_cmds": [], "latency_s": 5.0, "passed": False, "issues": slow["issues"]},
            {"id": "b", "say": "上一页", "status": "ran", "asr": "", "kind": "", "reason": "", "tts_text": "",
             "device_cmds": [], "latency_s": None, "passed": False, "issues": bad["issues"]},
            {"id": "c", "say": "x", "status": "skipped", "skip_reason": "needs quiet hours"}]
    text = vp.render_report(rows, {"时间": "t"})
    assert text.index("完全没反应") < text.index("【慢】")
    assert "跳过：needs quiet hours" in text


def _write_wav(path, amplitude):
    import math
    import struct
    import wave
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b"".join(struct.pack("<h", int(amplitude * math.sin(i / 5))) for i in range(16000)))


def test_wav_dbfs_and_noise_verdict(tmp_path):
    quiet, loud = tmp_path / "q.wav", tmp_path / "l.wav"
    _write_wav(quiet, 30)
    _write_wav(loud, 8000)
    q, lo = vp.wav_dbfs(quiet), vp.wav_dbfs(loud)
    assert q < -55 < -20 < lo
    assert vp.noise_verdict(q, -45)[0]
    ok, msg = vp.noise_verdict(lo, -45)
    assert not ok and "太吵" in msg
