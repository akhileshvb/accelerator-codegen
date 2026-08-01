from __future__ import annotations

from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from accel_pipeline.backends.cursor_timeouts import (
    DEFAULT_BRIDGE_LAUNCH_TIMEOUT_SECONDS,
    DEFAULT_STREAM_TIMEOUT_SECONDS,
    DEFAULT_UNARY_TIMEOUT_SECONDS,
    _external_bridge_config,
    bridge_retry_count,
    create_pipeline_client,
    cursor_timeout_seconds,
    is_bridge_failure,
)


def test_cursor_timeout_seconds_defaults(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.delenv("ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT", raising=False)
    assert cursor_timeout_seconds("ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT", 42.0) == 42.0


def test_cursor_timeout_seconds_env_override(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT", "900")
    assert cursor_timeout_seconds("ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT", 42.0) == 900.0


def test_cursor_timeout_seconds_rejects_invalid(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT", "nope")
    with pytest.raises(ValueError, match="must be a number"):
        cursor_timeout_seconds("ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT", DEFAULT_UNARY_TIMEOUT_SECONDS)


def test_default_constants():
    assert DEFAULT_UNARY_TIMEOUT_SECONDS > 60
    assert DEFAULT_STREAM_TIMEOUT_SECONDS > DEFAULT_UNARY_TIMEOUT_SECONDS
    assert DEFAULT_BRIDGE_LAUNCH_TIMEOUT_SECONDS >= 30


def test_bridge_retry_count_default(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.delenv("ACCEL_PIPELINE_CURSOR_BRIDGE_RETRIES", raising=False)
    assert bridge_retry_count() == 2


def test_is_bridge_failure_detects_connection_refused():
    err = Exception("Bridge request failed: ConnectError: [Errno 111] Connection refused")
    assert is_bridge_failure(err) is True


def test_is_bridge_failure_detects_dashed_tool_callback_token_bug():
    err = Exception(
        "Bridge exited before discovery with status 1: "
        "Missing value for --tool-callback-auth-token"
    )
    assert is_bridge_failure(err) is True


def test_external_bridge_config(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("CURSOR_SDK_BRIDGE_URL", "http://127.0.0.1:1234")
    monkeypatch.setenv("CURSOR_SDK_BRIDGE_TOKEN", "token")
    assert _external_bridge_config() == ("http://127.0.0.1:1234", "token")


@patch("accel_pipeline.backends.cursor_timeouts.reset_cursor_bridge")
def test_create_pipeline_client_retries_dashed_tool_callback_token(mock_reset, tmp_path: Path):
    pytest.importorskip("cursor_sdk")
    from cursor_sdk.errors import CursorSDKError

    from accel_pipeline.backends import cursor_timeouts

    endpoint = MagicMock()
    endpoint.url = "http://127.0.0.1:9"
    endpoint.auth_token = "tok"
    bridge = MagicMock()
    bridge.endpoint = endpoint
    dashed_token_error = CursorSDKError(
        "Bridge exited before discovery with status 1: Missing value for --tool-callback-auth-token"
    )

    with patch("cursor_sdk._bridge.Bridge.launch", side_effect=[dashed_token_error, bridge]) as mock_launch:
        client = cursor_timeouts.create_pipeline_client(tmp_path)

    assert mock_launch.call_count == 2
    assert client._owned_bridge is bridge
