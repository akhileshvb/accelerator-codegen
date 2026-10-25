"""Measurements on the subset that quick_check and run_profile add to correctness.

They run the aux bench binary through the host's `run` verb, in the engine instance the quick gate just loaded
the subset into: read latency against the engine's own SQL for the same bindings, `perf stat` counters of the
read loop and of the build, and `perf record` samples of the read, write and build paths.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline import perf_profile
from accel_pipeline.optimization.aux_bench import parse_aux_benchmark_output
from accel_pipeline.config import PipelineConfig, resolve_workload_paths
from accel_pipeline.engine_host import host_info, host_script, instance_env

BENCH_SECONDS = 120.0
READ_QUERIES = 20  # paired measured queries for the latency medians
COUNTER_LOOP_SECONDS = 0.5  # the read loop the counters cover
WRITE_MIX_PCT = 50  # share of operations that are writes in the write-path profile


@dataclass(frozen=True)
class Bench:
    """The aux bench binary on the quick instance (`instance`: the gate result's engine_instance, engine_state_dir, dataset)."""
    cfg: PipelineConfig
    workload: dict[str, Any]
    query: int
    instance: dict[str, str]

    @property
    def engine(self) -> str:
        return str(self.workload.get("engine_name") or "")

    @property
    def target(self) -> Path:
        return self.cfg.build_dir / resolve_workload_paths(self.cfg, self.workload, self.query)["aux_bench_target"]

    def env(self, work_dir: str, queries: int, pct: int = 0) -> dict[str, str]:
        """The bench's environment: this process's, the instance's, the write frequency and the repetitions
        (bench_sweep_contract.md)."""
        paths = resolve_workload_paths(self.cfg, self.workload, self.query)
        return {
            **os.environ,
            **instance_env(self.cfg.repo_root, self.engine, self.cfg.build_dir),
            "ACCEL_ENGINE_INSTANCE": self.instance["engine_instance"],
            "ACCEL_ENGINE_STATE_DIR": self.instance["engine_state_dir"],
            "ACCEL_DATASET_DIR": self.instance["dataset"],
            f"ACCEL_Q{self.query}_BENCH_SUITE": paths["aux_bench_suites"],
            "ACCEL_AUX_BENCHMARK": str(self.workload.get("benchmark_name") or ""),
            "ACCEL_AUX_BENCH_WORK_DIR": work_dir,
            "ACCEL_AUX_WRITE_FREQUENCIES": str(pct),
            "ACCEL_BENCH_WARMUP": "3",
            "ACCEL_BENCH_QUERIES": str(queries),
            "ACCEL_BENCH_REF_SAMPLES": "2",
            "ACCEL_BENCH_BASE_WRITE_SAMPLES": "1",
            "ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB": "1",
        }

    def command(self, pct: int = 0) -> list[str]:
        return [str(self.target), "--query", str(self.query), str(pct)]

    def run(self, command: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[str]:
        """`command` through `host.sh run` (it reaches the instance and applies its env file)."""
        return subprocess.run([str(host_script(self.cfg.repo_root, self.engine)), "run", *command], cwd=self.cfg.repo_root,
                              env=env, capture_output=True, text=True, check=False, timeout=BENCH_SECONDS)

    def row(self, pct: int = 0) -> dict[str, Any]:
        """One short bench run at write frequency `pct`: its result row (medians of the engine's SQL and of the
        accelerator over the same bindings, hit rate, state build, visited-state counters)."""
        with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
            done = self.run(self.command(pct), self.env(work, READ_QUERIES, pct))
        rows = parse_aux_benchmark_output(done.stdout or "")
        if done.returncode != 0 or not rows:
            raise RuntimeError(f"the bench exited {done.returncode}: {((done.stderr or '') + (done.stdout or ''))[-600:]}")
        return rows[0]

    def counters(self, row: dict[str, Any]) -> dict[str, Any]:
        """perf stat counters: {"read": per query of the read loop, "build": the whole run of a one-query bench},
        or {"unavailable": why}. `row`: this bench's read-only row."""
        if gap := self.profile_gap():
            return {"unavailable": gap}
        with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
            read = perf_profile.profile_hit_path(self.command(), self.env(work, 1), cwd=self.cfg.repo_root,
                                                 runner=self.perf_runner(), hit_us=row["accel_hit_median_us"],
                                                 loop_seconds=COUNTER_LOOP_SECONDS, record=False)
            build = self.run(["perf", "stat", "-x,", "-e", "instructions,cycles,cache-misses", "-o", f"{work}/build.stat",
                              "--", *self.command()], self.env(work, 1))
            stat = Path(work) / "build.stat"
            built = perf_profile.parse_stat(stat.read_text(encoding="utf-8")) if stat.is_file() else {}
        if read["status"] != "ok" or build.returncode != 0:
            return {"unavailable": read.get("reason") or f"perf stat of the build exited {build.returncode}"}
        return {"read": read["counters_per_query"], "build": built}

    def profile(self, seconds: float) -> dict[str, dict[str, Any]]:
        """Samples of the read path (read-only point), the write path (a WRITE_MIX_PCT% write mix) and the build (a
        one-query run), the read and write loops about `seconds` long: {path: perf_profile result}."""
        if gap := self.profile_gap():
            return {path: perf_profile.not_applicable(gap) for path in ("read", "write", "build")}
        out = {}
        for path, pct in (("read", 0), ("write", WRITE_MIX_PCT)):
            row = self.row(pct)
            op_us = (float(row["accel_hit_median_us"]) * (100 - pct) + float(row.get("accel_write_median_us") or 0) * pct) / 100.0
            with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
                out[path] = perf_profile.profile_hit_path(
                    self.command(pct), self.env(work, 1, pct), cwd=self.cfg.repo_root, runner=self.perf_runner(), hit_us=op_us,
                    loop_seconds=seconds, diagnostics=row if pct == 0 else None)
        with tempfile.TemporaryDirectory(prefix="accel-bench-") as work:
            out["build"] = perf_profile.profile_build(self.command(), self.env(work, 1), cwd=self.cfg.repo_root,
                                                      runner=self.perf_runner())
        return out

    def profile_gap(self) -> str:
        """Why the bench cannot be profiled with perf on this engine ("" when it can)."""
        return perf_profile.engine_profile_gap(host_info(self.cfg.repo_root, self.engine, self.cfg.build_dir))

    def perf_runner(self):
        """subprocess.run that sends the outermost `perf` command (the bench inside it) through host.sh."""
        def run(command, *, env, **kwargs):
            if Path(command[0]).name == "perf" and command[1] in ("stat", "record"):
                command = [str(host_script(self.cfg.repo_root, self.engine)), "run", *command]
            return subprocess.run(command, env=env, **kwargs)
        return run
