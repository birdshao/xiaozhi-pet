"""Pluggable model provider for S6 open chat (ADR-015).

`GlmProvider` calls Zhipu GLM's OpenAI-compatible chat completions endpoint over
HTTPS; `FakeChatProvider` is deterministic and offline for tests and demos.
The API key comes only from settings (KID_PET_GLM_API_KEY in providers.env) and
never appears in logs, exceptions or reprs. One request per turn: system prompt
plus the child's single utterance, no history.
"""
from __future__ import annotations

import json
from time import monotonic
from dataclasses import dataclass
from typing import Any, Protocol

import httpx

GLM_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions"
TOOL_NAME = "self_pet_request"
TIMEOUT_S = 5.0          # per connect/read phase
TOTAL_TIMEOUT_S = 6.0    # the whole request (V5_V6 plan §6.5: LLM 6 s)
MAX_RESPONSE_BYTES = 64 * 1024
MAX_TOKENS = 300
# Models with a "thinking" switch: off, a child waits for the answer.
_THINKING_PREFIXES = ("glm-4.5", "glm-4.6", "glm-4.7", "glm-5")


class ProviderError(Exception):
    """kind: timeout | http | format. Carries no response body, URL or key."""

    def __init__(self, kind: str) -> None:
        self.kind = kind
        super().__init__(kind)


@dataclass(frozen=True)
class ChatReply:
    text: str | None = None
    tool_action: str | None = None
    # "input": the platform refused the request; "output": it cut the reply.
    filtered: str | None = None
    # The provider stopped at its token limit: the last sentence may be cut off.
    truncated: bool = False


class ChatProvider(Protocol):
    configured: bool

    def complete(self, system: str, user: str, tools: list[dict[str, Any]]) -> ChatReply: ...


class FakeChatProvider:
    """Scripted replies (ChatReply or ProviderError); records (system, user, tools)."""

    def __init__(self, *, configured: bool = True) -> None:
        self.configured = configured
        self.calls: list[tuple[str, str, list[dict[str, Any]]]] = []
        self._script: list[ChatReply | ProviderError] = []

    def script(self, *items: ChatReply | ProviderError) -> None:
        self._script.extend(items)

    def complete(self, system: str, user: str, tools: list[dict[str, Any]]) -> ChatReply:
        self.calls.append((system, user, tools))
        item = self._script.pop(0) if self._script else ChatReply(text="好呀。")
        if isinstance(item, Exception):
            raise item
        return item


class GlmProvider:
    def __init__(self, settings, *, transport: httpx.BaseTransport | None = None) -> None:
        self._key = settings.glm_api_key
        self.model = settings.glm_model
        self._transport = transport

    @property
    def configured(self) -> bool:
        return bool(self._key)

    def __repr__(self) -> str:
        return f"GlmProvider(model={self.model!r}, configured={self.configured})"

    def complete(self, system: str, user: str, tools: list[dict[str, Any]]) -> ChatReply:
        if not self._key:
            raise ProviderError("http")
        body: dict[str, Any] = {
            "model": self.model,
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
            "temperature": 0.6, "max_tokens": MAX_TOKENS, "stream": False,
        }
        if tools:
            body["tools"] = tools
            body["tool_choice"] = "auto"
        if self.model.startswith(_THINKING_PREFIXES):
            body["thinking"] = {"type": "disabled"}
        # httpx timeouts are per phase / per read; the deadline bounds the whole request so the
        # guard (SERVICE_TIMEOUT_S = 9 s) always gets an answer before it gives up.
        deadline = monotonic() + TOTAL_TIMEOUT_S
        try:
            with httpx.Client(timeout=httpx.Timeout(TIMEOUT_S, connect=2.0), follow_redirects=False,
                              trust_env=False, transport=self._transport) as client:
                with client.stream("POST", GLM_URL, json=body,
                                   headers={"Authorization": "Bearer " + self._key}) as response:
                    status = response.status_code
                    raw = bytearray()
                    for chunk in response.iter_bytes():
                        raw += chunk
                        if monotonic() > deadline:
                            raise ProviderError("timeout")
                        if len(raw) > MAX_RESPONSE_BYTES:
                            raise ProviderError("format")
        except httpx.TimeoutException:
            raise ProviderError("timeout") from None
        except httpx.HTTPError:
            raise ProviderError("http") from None
        if monotonic() > deadline:
            raise ProviderError("timeout")
        try:
            data = json.loads(bytes(raw))
        except ValueError:
            data = None
        if status == 400 and _error_code(data) == "1301":
            return ChatReply(filtered="input")  # the platform's own filter refused the request
        if status != 200:
            raise ProviderError("http")
        try:
            return _parse(data)
        except (ValueError, KeyError, IndexError, TypeError, AttributeError):
            raise ProviderError("format") from None


def _error_code(data: Any) -> str | None:
    try:
        return str(data["error"]["code"])
    except (KeyError, TypeError):
        return None


def _parse(data: dict[str, Any]) -> ChatReply:
    choice = data["choices"][0]
    if choice.get("finish_reason") == "sensitive":
        return ChatReply(filtered="output")
    if choice.get("finish_reason") == "network_error":
        raise ProviderError("http")
    message = choice["message"]
    calls = message.get("tool_calls") or []
    if calls:
        function = calls[0]["function"]
        if function["name"] != TOOL_NAME:
            raise ValueError("unknown tool")
        arguments = function["arguments"]
        if isinstance(arguments, str):
            arguments = json.loads(arguments)
        action = arguments["action"]
        if not isinstance(action, str):
            raise ValueError("action")
        return ChatReply(tool_action=action)
    content = message.get("content")
    if not isinstance(content, str):
        raise ValueError("content")
    return ChatReply(text=content, truncated=choice.get("finish_reason") == "length")


def from_settings(settings) -> ChatProvider:
    return GlmProvider(settings)
