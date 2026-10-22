"""Tests for worker resume reconstruction."""

from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.config import PipelineConfig, load_pipeline_config
from accel_pipeline.optimization_context import reconstruct_seed_from_artifacts
from accel_pipeline.optimization_worker import OptimizationWorker


def test_reconstruct_seed_from_artifacts(tmp_path: Path):
    repo = tmp_path
    (repo / "pipeline" / "config").mkdir(parents=True)
    default_src = Path(__file__).resolve().parents[2] / "config" / "default.yaml"
    (repo / "pipeline" / "config" / "default.yaml").write_text(
        default_src.read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = {
        "name": "tpch_q9_aux",
        "workload_kind": "aux_state",
        "benchmark_name": "tpch",
        "engine_name": "duckdb",
        "query_number": 9,
        "artifact_namespace": "tpch",
    }
    ckpt = {
        "recent_hypotheses": [{"attempt": 1, "bottleneck": "x", "verdict": "reject"}],
        "pending_feedback": "try again",
    }
    seed = reconstruct_seed_from_artifacts(pipeline_config, workload, 9, checkpoint=ckpt)
    assert "resume context" in seed.lower() or "Optimization worker" in seed
    assert "try again" in seed


def test_worker_one_runner_per_candidate_cycle(monkeypatch):
    monkeypatch.setenv("OPENROUTER_API_KEY", "test")
    calls: list[str] = []

    class FakeBackend:
        name = "fake"

        def run_stage(self, request):
            calls.append("run")
            from accel_pipeline.backends.base import StageRunResult

            return StageRunResult(
                backend="fake",
                stage=request.stage,
                status="finished",
                final_text='{"bottleneck":"x"}\nCANDIDATE_READY',
                metadata={"tool_calls": 2, "usage": {"input_tokens": 1, "output_tokens": 1}},
            )

    from accel_pipeline.config import ModelConfig

    worker = OptimizationWorker(
        pipeline_config=PipelineConfig(
            repo_root=Path("/tmp"),
            pipeline_root=Path("/tmp/pipeline"),
            build_dir=Path("/tmp/build"),
            tpch_db_sf1=Path("/tmp/sf1"),
            tpch_db_sf10=Path("/tmp/sf10"),
            optimization_turn_delta_max_chars=4000,
            optimization_checkpoint_file="optimization_worker_checkpoint.json",
        ),
        model_config=ModelConfig(
            api_key_env="OPENROUTER_API_KEY",
            presets={"default": "gpt-test", "code": "gpt-test"},
            stages={"optimization_worker": "code", "optimization": "code"},
            stage_providers={"optimization_worker": "codex_exec"},
        ),
        workload_name="w",
        query=9,
        workload={"name": "w"},
        art_dir=Path("/tmp/art"),
        backend=FakeBackend(),
        instructions="inst",
        provider="codex_exec",
        seeded=True,
    )
    worker.run_candidate_cycle("go")
    worker.run_candidate_cycle("go again")
    assert calls == ["run", "run"]
