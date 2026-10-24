"""Facts the prompts must state consistently with the code they describe."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

PROMPTS = Path(__file__).resolve().parents[2] / "prompts"
OPTIMIZATION_PROMPTS = (
    "03_optimization_worker.md",
    "03_optimization_unified.md",
    "aux_state/03_optimization_worker.md",
    "aux_state/03_optimization_unified.md",
)


@pytest.mark.parametrize("prompt", OPTIMIZATION_PROMPTS)
def test_optimization_prompts_inline_the_measurement_trust_text(prompt):
    assert "{{measurement_trust}}" in (PROMPTS / prompt).read_text(encoding="utf-8")


def test_no_prompt_sends_the_agent_to_read_the_measurement_trust_file():
    assert not [p for p in PROMPTS.rglob("*.md") if "measurement_trust.md" in p.read_text(encoding="utf-8")]


def test_the_bench_contract_requires_the_measurement_columns():
    contract = (PROMPTS / "aux_state/shared/bench_sweep_contract.md").read_text(encoding="utf-8")
    header = re.search(r"The columns, by name:\n\n(.*?)\n\n", contract, re.DOTALL).group(1)
    for column in ("accel_hit_rate", "num_queries", "accel_fallback_reads", "state_builds"):
        assert f"`{column}`" in header
    assert "required" in contract


GOAL_PROMPTS = {
    "02_implementation.md": "low_level_execution.md",
    "03_optimization_worker.md": "low_level_execution.md",
    "03_optimization_unified.md": "low_level_execution.md",
    "aux_state/02_implementation.md": "cow_primitives.md",
    "aux_state/03_optimization_worker.md": "cow_primitives.md",
    "aux_state/03_optimization_unified.md": "cow_primitives.md",
}


@pytest.mark.parametrize("prompt,guide", GOAL_PROMPTS.items())
def test_goal_prompts_name_their_guides_and_include_the_fast_loop(prompt, guide):
    text = (PROMPTS / prompt).read_text(encoding="utf-8")
    assert guide in text and "{{fast_loop}}" in text
    assert list(PROMPTS.rglob(guide))
    assert "low_level_execution.md" in text
