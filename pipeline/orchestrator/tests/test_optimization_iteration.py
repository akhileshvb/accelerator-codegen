"""The attempt loop of one optimization iteration, on a scripted worker and scripted gates."""

from __future__ import annotations

from dataclasses import dataclass, field
from types import SimpleNamespace

import pytest

from accel_pipeline import optimization_attempt as attempt_mod
from accel_pipeline.optimization_eval import AttemptOutcome, BenchmarkSample, MultiSampleBenchmark
from accel_pipeline.optimization_pending import PendingFeedback
from accel_pipeline.optimization_worker import CandidateCycleResult, EpisodeMetadata

WORKLOAD = {"workload_kind": "base", "name": "w"}


def bench(score: float) -> MultiSampleBenchmark:
    return MultiSampleBenchmark([BenchmarkSample({"score": score}, score)], score, {"score": score})


def outcome(verdict: str) -> AttemptOutcome:
    return AttemptOutcome(verdict, improvement_pct=1.0, baseline_score=1.0, candidate_score=1.0, diagnostic_vector={})


@dataclass
class Worker:
    cycles: list[CandidateCycleResult]
    pending: PendingFeedback = field(default_factory=PendingFeedback)
    episode_meta: EpisodeMetadata = field(default_factory=EpisodeMetadata)
    messages: list[str] = field(default_factory=list)
    feedback: list[str] = field(default_factory=list)

    def run_candidate_cycle(self, message: str) -> CandidateCycleResult:
        self.messages.append(message)
        return self.cycles.pop(0)

    def save_checkpoint(self) -> None:
        if self.pending.peek():
            self.feedback.append(self.pending.peek())


def ready(name: str = "h") -> CandidateCycleResult:
    return CandidateCycleResult("ready", hypothesis={"bottleneck": name})


@pytest.fixture
def world(tmp_path, monkeypatch):
    world = SimpleNamespace(log=[], reverts=0, builds=[], outcomes=[], benches=[], infra=None, best=[])
    noop = lambda *a, **k: None  # noqa: E731
    for name in ("begin_attempt_checkpoint", "clear_attempt_checkpoint", "log_event", "save_experimental_branch",
                 "maybe_record_reject_trap", "record_design_revision"):
        monkeypatch.setattr(attempt_mod, name, noop)
    monkeypatch.setattr(attempt_mod, "query_artifacts_dir", lambda *a, **k: tmp_path)
    monkeypatch.setattr(attempt_mod, "eval_config_from_pipeline", lambda cfg: SimpleNamespace(quick_benchmark_samples=1))
    monkeypatch.setattr(attempt_mod, "ensure_attempt_write_frequencies", lambda *a, **k: [0])
    monkeypatch.setattr(attempt_mod, "load_experimental_branch", lambda art: {})
    monkeypatch.setattr(attempt_mod, "load_best_valid_baseline", lambda art: {})
    monkeypatch.setattr(attempt_mod, "advance_best_valid_baseline", lambda *a, score, **k: world.best.append(score))
    monkeypatch.setattr(attempt_mod, "append_attempt_log", lambda art, entry: world.log.append(entry))
    monkeypatch.setattr(attempt_mod, "revert_attempt_checkpoint", lambda *a: setattr(world, "reverts", world.reverts + 1))
    monkeypatch.setattr(attempt_mod, "run_build", lambda *a: world.builds.pop(0) if world.builds else (True, ""))
    monkeypatch.setattr(attempt_mod, "run_smoke_correctness_repeated", lambda *a, **k: SimpleNamespace(passed=True))
    monkeypatch.setattr(attempt_mod, "run_optimization_probe", lambda *a: SimpleNamespace(passed=True))
    monkeypatch.setattr(attempt_mod, "run_multi_sample_benchmark", lambda *a, **k: world.benches.pop(0))
    monkeypatch.setattr(attempt_mod, "evaluate_attempt_outcome", lambda **k: world.outcomes.pop(0))
    monkeypatch.setattr(attempt_mod, "handle_infra_failure", lambda *a, **k: world.infra)
    monkeypatch.setattr(attempt_mod, "is_transient_provider_failure", lambda reason: "reset" in (reason or ""))
    monkeypatch.setattr(attempt_mod.time, "sleep", noop)
    world.config = SimpleNamespace(
        repo_root=tmp_path, measurement=None, optimization_max_attempts_per_iteration=3,
        optimization_consecutive_fail_stop=0, optimization_smoke_runs=1, optimization_turn_delta_max_chars=4000,
        repair=SimpleNamespace(provider_retries=1, provider_backoff=lambda retry: 0),
    )
    return world


def run(world, worker: Worker):
    iteration = attempt_mod._Iteration(world.config, WORKLOAD, 3, 1, worker, bench(1.0))
    return iteration, iteration.run(deadline=None)


def test_an_accepted_candidate_becomes_the_baseline_and_a_regression_is_reverted(world):
    worker = Worker([ready("a"), ready("b"), CandidateCycleResult("failed", reason="no marker")])
    world.benches = [bench(2.0), bench(1.5)]
    world.outcomes = [outcome("accept"), outcome("reject_regress")]
    iteration, _ = run(world, worker)
    assert [entry["verdict"] for entry in world.log] == ["accept", "reject_regress", "candidate_failed"]
    assert world.best == [2.0] and iteration.baseline.median_score == 2.0
    assert world.reverts == 2  # the regression and the failed cycle
    assert "**reject_regress**" in worker.feedback[-1] and "reverted" in worker.feedback[-1]
    assert "Prior gate feedback" in worker.messages[2]


def test_an_infra_failure_of_the_build_ends_the_iteration(world):
    worker = Worker([ready(), ready()])
    world.builds = [(False, "No space left on device")]
    world.infra = {"infra_reason": "disk_full"}
    iteration, _ = run(world, worker)
    assert [entry["verdict"] for entry in world.log] == ["infra_fail_fast:disk_full"]
    assert iteration.done and len(worker.messages) == 1


def test_a_transient_provider_failure_retries_the_same_attempt(world):
    worker = Worker([CandidateCycleResult("failed", reason="connection reset"), ready()])
    world.config.optimization_max_attempts_per_iteration = 1
    world.benches = [bench(2.0)]
    world.outcomes = [outcome("accept")]
    run(world, worker)
    assert len(worker.messages) == 2 and worker.messages[0] == worker.messages[1]
    assert [entry["verdict"] for entry in world.log] == ["accept"]


def test_consecutive_failures_stop_the_iteration(world):
    world.config.optimization_consecutive_fail_stop = 2
    worker = Worker([CandidateCycleResult("failed", reason="x") for _ in range(3)])
    run(world, worker)
    assert len(worker.messages) == 2


def test_an_honored_saturation_claim_ends_the_iteration(world, monkeypatch):
    monkeypatch.setattr(attempt_mod, "_try_honor_saturation", lambda **k: (True, "at the floor"))
    worker = Worker([CandidateCycleResult("saturated", saturation={"reason": "stuck"}), ready()])
    _, result = run(world, worker)
    assert result.status == "saturated" and len(worker.messages) == 1
