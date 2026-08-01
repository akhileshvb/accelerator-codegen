"""Cursor SDK HTTP timeouts and dedicated bridge lifecycle for pipeline stages."""

from __future__ import annotations

import os
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

# cursor-sdk defaults: unary=60s, stream=600s — too low for CreateAgent + long runs.
DEFAULT_UNARY_TIMEOUT_SECONDS = 300.0
DEFAULT_STREAM_TIMEOUT_SECONDS = 7200.0
DEFAULT_BRIDGE_LAUNCH_TIMEOUT_SECONDS = 120.0
DEFAULT_BRIDGE_RETRIES = 2
# cursor-sdk tool callback tokens are urlsafe; ~1/64 start with "-" and the
# bundled bridge CLI misparses them as a missing --tool-callback-auth-token value.
DEFAULT_BRIDGE_LAUNCH_TOKEN_RETRIES = 8
def bridge_launch_token_retries() -> int:
    raw = os.environ.get("ACCEL_PIPELINE_CURSOR_BRIDGE_LAUNCH_TOKEN_RETRIES", "").strip()
    if not raw:
        return DEFAULT_BRIDGE_LAUNCH_TOKEN_RETRIES
    try:
        value = int(raw)
    except ValueError as exc:
        raise ValueError(
            f"ACCEL_PIPELINE_CURSOR_BRIDGE_LAUNCH_TOKEN_RETRIES must be an integer, got {raw!r}"
        ) from exc
    if value < 1:
        raise ValueError(f"ACCEL_PIPELINE_CURSOR_BRIDGE_LAUNCH_TOKEN_RETRIES must be >= 1, got {value}")
    return value


def cursor_timeout_seconds(name: str, default: float) -> float:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        value = float(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be a number of seconds, got {raw!r}") from exc
    if value <= 0:
        raise ValueError(f"{name} must be > 0, got {value}")
    return value


def bridge_retry_count() -> int:
    raw = os.environ.get("ACCEL_PIPELINE_CURSOR_BRIDGE_RETRIES", "").strip()
    if not raw:
        return DEFAULT_BRIDGE_RETRIES
    try:
        value = int(raw)
    except ValueError as exc:
        raise ValueError(f"ACCEL_PIPELINE_CURSOR_BRIDGE_RETRIES must be an integer, got {raw!r}") from exc
    if value < 0:
        raise ValueError(f"ACCEL_PIPELINE_CURSOR_BRIDGE_RETRIES must be >= 0, got {value}")
    return value


def reset_cursor_bridge() -> None:
    """Tear down any cached global Cursor SDK bridge (legacy singleton)."""
    try:
        from cursor_sdk._client import close_default_client
    except ImportError:
        return
    close_default_client()


def _bridge_launch_token_argparse_bug(exc: BaseException) -> bool:
    """True when cursor-sdk-bridge rejected a randomly dashed callback token."""
    message = str(exc).lower()
    return "missing value for --tool-callback-auth-token" in message


def is_bridge_failure(exc: BaseException) -> bool:
    """True when the local Cursor SDK bridge is unreachable or timed out."""
    if _bridge_launch_token_argparse_bug(exc):
        return True
    try:
        from cursor_sdk.errors import APITimeoutError, CursorSDKError, NetworkError

        if isinstance(exc, (NetworkError, APITimeoutError)):
            return True
        if isinstance(exc, CursorSDKError) and (
            "bridge exited before discovery" in str(exc).lower()
            or _bridge_launch_token_argparse_bug(exc)
        ):
            return True
    except ImportError:
        pass
    cause = exc
    for _ in range(4):
        cause = getattr(cause, "__cause__", None) or getattr(cause, "__context__", None)
        if cause is None:
            break
        name = type(cause).__name__
        if name in {"ConnectError", "ReadTimeout", "ConnectTimeout", "NetworkError"}:
            return True
        if _bridge_launch_token_argparse_bug(cause):
            return True
    message = str(exc).lower()
    return (
        "connection refused" in message
        or "timed out" in message
        or "bridge request failed" in message
        or "bridge exited before discovery" in message
        or _bridge_launch_token_argparse_bug(exc)
    )


def _external_bridge_config() -> tuple[str, str] | None:
    bridge_url = os.environ.get("CURSOR_SDK_BRIDGE_URL", "").strip()
    bridge_token = (
        os.environ.get("CURSOR_SDK_BRIDGE_TOKEN")
        or os.environ.get("CURSOR_SDK_BRIDGE_AUTH_TOKEN")
        or ""
    ).strip()
    if bridge_url and bridge_token:
        return bridge_url, bridge_token
    return None


def create_pipeline_client(repo_root: Path):
    """Create a Cursor SDK client with pipeline timeouts and correct workspace.

    Prefer a dedicated owned bridge launched at ``repo_root``. Avoids the SDK
    global singleton (``_default_client``), which launches with ``os.getcwd()``
    and loses bridge ownership when ``with_options()`` is applied.
    """
    from cursor_sdk import Client

    unary_timeout = cursor_timeout_seconds(
        "ACCEL_PIPELINE_CURSOR_UNARY_TIMEOUT",
        DEFAULT_UNARY_TIMEOUT_SECONDS,
    )
    stream_timeout = cursor_timeout_seconds(
        "ACCEL_PIPELINE_CURSOR_STREAM_TIMEOUT",
        DEFAULT_STREAM_TIMEOUT_SECONDS,
    )
    launch_timeout = cursor_timeout_seconds(
        "ACCEL_PIPELINE_CURSOR_BRIDGE_LAUNCH_TIMEOUT",
        DEFAULT_BRIDGE_LAUNCH_TIMEOUT_SECONDS,
    )

    external = _external_bridge_config()
    if external is not None:
        base_url, auth_token = external
        return Client(
            base_url=base_url,
            auth_token=auth_token,
            unary_timeout=unary_timeout,
            stream_timeout=stream_timeout,
            allow_api_key_env_fallback=False,
        )

    # Drop any stale global bridge before launching our own subprocess.
    reset_cursor_bridge()

    from cursor_sdk._bridge import Bridge

    max_launch_attempts = bridge_launch_token_retries()
    last_error: BaseException | None = None
    for attempt in range(max_launch_attempts):
        try:
            bridge = Bridge.launch(
                workspace=str(repo_root.resolve()),
                timeout=launch_timeout,
            )
            break
        except Exception as exc:
            last_error = exc
            if not _bridge_launch_token_argparse_bug(exc) or attempt + 1 >= max_launch_attempts:
                raise
    else:  # pragma: no cover - guarded by raise in loop
        assert last_error is not None
        raise last_error

    client = Client(
        bridge.endpoint,
        unary_timeout=unary_timeout,
        stream_timeout=stream_timeout,
        allow_api_key_env_fallback=True,
    )
    client._owned_bridge = bridge
    return client


@contextmanager
def pipeline_cursor_session(repo_root: Path) -> Iterator[Any]:
    """Yield a Cursor SDK client; always close the owned bridge on exit."""
    client = create_pipeline_client(repo_root)
    try:
        yield client
    finally:
        close_pipeline_client(client)


def close_pipeline_client(client: Any) -> None:
    try:
        client.close()
    except Exception:
        pass


def close_agent_safely(agent: Any) -> None:
    """Close an agent without raising if the bridge is already gone."""
    try:
        agent.close()
    except Exception:
        pass
