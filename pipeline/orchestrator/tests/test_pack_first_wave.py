"""run_pack `first_wave: N`: the first N runs finish before the rest start."""

from __future__ import annotations

import threading
import time

import pytest

from accel_pipeline import config

from accel_pipeline.engine_backend import host_bootstrap

from accel_pipeline.pack import repo_context_pack

from accel_pipeline.pack import run as run_pack


@pytest.fixture()
def events(monkeypatch, tmp_path):
    log: list[tuple[str, int]] = []
    lock = threading.Lock()

    def fake_run(*, entry, **_):
        with lock:
            log.append(("start", entry.query))
        time.sleep(0.05 if entry.query <= 2 else 0)
        with lock:
            log.append(("end", entry.query))
        return {"query": entry.query, "workload": entry.workload, "engine": entry.engine, "ok": True}

    monkeypatch.setattr(run_pack, "run_pack_query", fake_run)
    monkeypatch.setattr(repo_context_pack, "ensure_repo_context_pack", lambda *a, **k: "ctx.md")
    monkeypatch.setattr(host_bootstrap, "ensure_host_bootstrap_for_pack", lambda *a, **k: [])
    monkeypatch.setattr(config, "load_pipeline_config", lambda *a, **k: None)
    monkeypatch.setattr(config, "load_model_config", lambda *a, **k: None)
    monkeypatch.setattr(run_pack, "pipeline_root", lambda root: tmp_path)
    return log


def run(first_wave: int, queries: int = 6):
    pack = {"queries": list(range(1, queries + 1)), "concurrency": 6, "first_wave": first_wave}
    return run_pack.run_pack(pack, repo_root=run_pack.Path("."))


def test_first_wave_completes_before_the_rest_start(events):
    summary = run(2)
    assert summary["ok_count"] == 6
    last_early_end = max(i for i, (kind, q) in enumerate(events) if kind == "end" and q <= 2)
    first_late_start = min(i for i, (kind, q) in enumerate(events) if kind == "start" and q > 2)
    assert last_early_end < first_late_start


def test_no_first_wave_fans_out_at_once(events):
    run(0)
    first_end = min(i for i, (kind, _) in enumerate(events) if kind == "end")
    assert sum(kind == "start" for kind, _ in events[:first_end]) > 2


def test_first_wave_covering_everything_is_one_wave(events):
    assert run(9)["ok_count"] == 6
