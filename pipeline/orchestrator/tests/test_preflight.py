"""Preflight checks: each catches the failure it was written for, and passes on this repository."""

from __future__ import annotations

import stat
from dataclasses import replace
from pathlib import Path

import pytest

from accel_pipeline import preflight
from accel_pipeline.backends.codex_exec import CodexExecBackend
from accel_pipeline.config import load_model_config, load_pipeline_config, load_workload, workload_supported_engines

REPO_ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture
def probe(tmp_path, monkeypatch) -> preflight.Probe:
    monkeypatch.setattr(CodexExecBackend, "_resolve_codex_bin", lambda self: "codex")
    monkeypatch.setenv("OPENROUTER_API_KEY", "unused")
    engine = workload_supported_engines("tpch_q14_aux", REPO_ROOT)[0]
    cfg = replace(load_pipeline_config(None, REPO_ROOT), sandbox="none", engine_name=engine, codex_home=tmp_path / "codex_home")
    workload = load_workload("tpch_q14_aux", REPO_ROOT, engine=engine)
    models = load_model_config(None, repo_root=REPO_ROOT)
    return preflight.Probe(cfg, models, workload, "tpch_q14_aux", 14)


def fake_perf(tmp_path, monkeypatch, stderr: str) -> None:
    script = tmp_path / "bin" / "perf"
    script.parent.mkdir()
    script.write_text(f"#!/bin/sh\ncat >&2 <<'EOF'\n{stderr}\nEOF\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("PATH", f"{script.parent}:/usr/bin:/bin")


def test_perf_without_pmu_is_reported(probe, tmp_path, monkeypatch):
    fake_perf(tmp_path, monkeypatch, "<not supported>,,cycles,0,100.00,,\n<not supported>,,instructions,0,100.00,,")
    assert "no counters" in preflight.perf_counters(probe)[0]


def test_perf_with_counters_passes(probe, tmp_path, monkeypatch):
    fake_perf(tmp_path, monkeypatch, "1234,,cycles,100,100.00,,\n5678,,instructions,100,100.00,,")
    assert preflight.perf_counters(probe) == []


def test_codex_line_carries_the_configured_window_and_compaction(probe):
    assert preflight.codex_line(probe) == []


@pytest.mark.parametrize("field", ["codex_model_context_window", "codex_auto_compact_tokens", "codex_compact_prompt_file"])
def test_codex_line_without_a_setting_is_reported(probe, field):
    unset = replace(probe, models=replace(probe.models, **{field: 0 if "file" not in field else ""}))
    assert any("no" in p for p in preflight.codex_line(unset))


def test_codex_line_with_a_missing_compact_prompt_file_is_reported(probe):
    missing = replace(probe, models=replace(probe.models, codex_compact_prompt_file="pipeline/prompts/shared/nope.md"))
    assert any("does not exist" in p for p in preflight.codex_line(missing))


def test_gate_writers_of_this_repository_paste_no_log_text(probe):
    assert preflight.gate_writers(probe) == []


@pytest.mark.parametrize("script, flagged", [
    ("python3 - <<PY\nprint('${LOG}')\nPY\n", True),                                  # the B7 pattern: log text in source
    ('python3 -c "print(\'${REASON}\')"\n', True),
    ("python3 - <<PY\nprint(${QUERY}, '${OUT}')\nPY\n", False),                        # numbers and paths are fine
    ("LOG_TEXT=\"${LOG}\" python3 - <<'PY'\nimport os\nprint(os.environ['LOG_TEXT'])\nPY\n", False),  # quoted heredoc
])
def test_gate_writer_scan(probe, tmp_path, script, flagged):
    gates = tmp_path / "gates"
    gates.mkdir()
    (gates / "run_x.sh").write_text(script)
    scanned = replace(probe, cfg=replace(probe.cfg, pipeline_root=tmp_path))
    assert bool(preflight.gate_writers(scanned)) is flagged


def test_prompt_problems(tmp_path):
    (tmp_path / "pipeline").mkdir()
    (tmp_path / "pipeline" / "real.md").touch()
    text = "{{ghost}} `pipeline/real.md` `pipeline/gone.md` `pipeline/artifacts/tpch/q1/analysis.json`"
    assert preflight.prompt_problems(text, tmp_path) == ["unresolved placeholder {{ghost}}", "names pipeline/gone.md, which does not exist"]


def test_stage_prompts_of_this_repository_render_clean(probe):
    assert preflight.prompts(probe) == []


def test_the_live_model_probe_has_a_provider():
    from accel_pipeline.config import ModelConfig, resolve_stage_provider

    models = ModelConfig(api_key_env="K", presets={"default": "m"}, stages={}, stage_providers={"analysis": "codex_exec"})
    assert resolve_stage_provider("preflight_live", models) == "codex_exec"
