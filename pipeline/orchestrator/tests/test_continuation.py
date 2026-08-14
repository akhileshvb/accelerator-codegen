"""Tests for OpenAI continuation helpers."""

from __future__ import annotations

from types import SimpleNamespace

from accel_pipeline.backends.continuation import (
    ContinuationState,
    continuation_profile,
    extract_continuation_from_result,
    resolve_run_kwargs,
)


def test_resolve_run_kwargs_openai_first_invocation():
    kwargs = resolve_run_kwargs(
        provider="openai",
        last_response_id=None,
        auto_previous_response_id=True,
        is_first_invocation=True,
    )
    assert kwargs.get("auto_previous_response_id") is True


def test_resolve_run_kwargs_openai_chained():
    kwargs = resolve_run_kwargs(
        provider="openai",
        last_response_id="resp_abc",
        auto_previous_response_id=True,
        is_first_invocation=False,
    )
    assert kwargs["previous_response_id"] == "resp_abc"


def test_resolve_run_kwargs_openrouter_empty():
    kwargs = resolve_run_kwargs(
        provider="openrouter",
        last_response_id="resp_abc",
        auto_previous_response_id=True,
        is_first_invocation=False,
    )
    assert kwargs == {}


def test_extract_continuation_from_result():
    result = SimpleNamespace(last_response_id="resp_xyz")
    assert extract_continuation_from_result(result) == "resp_xyz"


def test_continuation_state_roundtrip():
    state = ContinuationState(provider="openai", last_response_id="resp_1", runner_invocations=3)
    restored = ContinuationState.from_dict(state.to_dict())
    assert restored.last_response_id == "resp_1"
    assert restored.runner_invocations == 3


def test_continuation_profile():
    assert continuation_profile("openai").value == "openai_response_chain"
    assert continuation_profile("openrouter").value == "openrouter_in_memory_history"
