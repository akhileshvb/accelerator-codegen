from __future__ import annotations

from accel_pipeline.optimization_episodic import _episode_instructions


def test_episode_two_uses_short_instructions():
    full = "# Stage 3\n\nLong optimization prompt " + ("x" * 5000)
    ep2 = _episode_instructions(
        full_prompt=full,
        episode=2,
        episodes=4,
        optimization_iter=3,
        runtime_rel="pipeline/artifacts/sec_edgar/q3/optimization_runtime.md",
        provider="cursor",
    )
    assert len(ep2) < 500
    assert "continuation" in ep2.lower()
    assert "optimization_runtime.md" in ep2
    assert "x" * 100 not in ep2


def test_episode_one_keeps_full_prompt_for_cursor():
    full = "# Stage 3\n\nFull prompt"
    ep1 = _episode_instructions(
        full_prompt=full,
        episode=1,
        episodes=4,
        optimization_iter=1,
        runtime_rel="pipeline/artifacts/sec_edgar/q3/optimization_runtime.md",
        provider="cursor",
    )
    assert ep1 == full
