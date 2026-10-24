#!/usr/bin/env python3
"""The gates' side of the frozen test suite (accel_pipeline.test_suite): what a gate script needs of it.

    suite_gate.py <root> prepare <quick|full|validate>   exports for the run: verifies the freeze, draws the run's random
                                                tuples under a fresh seed (ACCEL_SUITE_SEED reproduces one), prints the
                                                ACCEL_SUITE_* environment (host_contract.md, "Test suite")
    suite_gate.py <root> validate [report.json] the suite's problems: its files, or a validate run's report (exit 1)
    suite_gate.py <root> summary <report.json>  the run's report as the gate result's `suite` field
    suite_gate.py <root> mix <acceptance|quick> the write frequencies of one benchmark run: seed and the list
    suite_gate.py <root> freeze                 record the suite's content hash (the test_suite stage does it)

Exit 3: the suite is edited (or never frozen); the reason is on stderr.  No suite directory: nothing is printed.
"""

from __future__ import annotations

import json
import os
import shlex
import sys
from pathlib import Path


def _context() -> dict:
    return json.loads(Path(os.environ["ACCEL_GATE_CONTEXT"]).read_text(encoding="utf-8"))


def _exports(env: dict[str, str]) -> str:
    return "\n".join(f"export {k}={shlex.quote(v)}" for k, v in env.items())


def _suite(ctx: dict) -> Path | None:
    """The suite directory of this run, or None: the query has no suite (older trees, or an engine-backend run)."""
    settings = ctx.get("test_suite")
    return Path(settings["dir"]) if settings and Path(settings["dir"]).is_dir() else None


def prepare(ts, ctx: dict, suite: Path, tier: str) -> int:
    settings, art = ctx["test_suite"], Path(ctx["artifacts_dir"])
    if tier != "validate" and (problem := ts.frozen_problem(art, suite)):
        print(problem, file=sys.stderr)
        return 3
    seed = int(os.environ.get("ACCEL_SUITE_SEED") or 0) or None
    cfg = ts.SuiteConfig(**settings["config"])
    out = Path(os.environ.get("ACCEL_GATE_OUT_DIR") or art / "gates")
    env = ts.run_environment(suite, tier=tier, cfg=cfg, seed=seed, params_file=out / f"suite_{tier}_params.sql")
    env.update(
        ACCEL_SUITE_REPORT=str(out / f"suite_{tier}_report.json"),
        ACCEL_SUITE_QUERY_SQL=settings["query_sql"],
        ACCEL_SUITE_FUNCTION=settings["function"],
        ACCEL_SUITE_ARITY=str(settings["arity"]),
        ACCEL_SUITE_SCHEMA_SQL=settings["schema_sql"],
        ACCEL_SUITE_HASH=ts.content_hash(suite),
    )
    print(_exports(env))
    return 0


def validate(ts, ctx: dict, suite: Path, report_path: str | None) -> int:
    """The suite's own problems: its files (no report), or the harness's report of running it on plain SQL."""
    settings, art = ctx["test_suite"], Path(ctx["artifacts_dir"])
    tables = ts.dependent_tables(art)
    if report_path is None:
        problems = ts.validate_static(suite, query_sql=Path(settings["query_sql"]).read_text(encoding="utf-8"),
                                      param_names=settings["param_names"], dependent_tables=tables, aux=settings["aux"])
    else:
        problems = ts.validate_report(json.loads(Path(report_path).read_text(encoding="utf-8")), suite, tables, settings["aux"])
    print("\n".join(problems))
    return 1 if problems else 0


def summary(report_path: str) -> int:
    try:
        report = json.loads(Path(report_path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        print("{}")
        return 0
    keep = ("ok", "seed", "params", "txns", "checks", "mismatches", "order_mismatches", "write_errors", "reference_errors",
            "executes", "seconds", "examples", "write_error_examples")
    print(json.dumps({**{k: report[k] for k in keep if k in report},
                      "param_tuples": report.get("param_tuples"), "hash": os.environ.get("ACCEL_SUITE_HASH", "")}))
    return 0


def mix(ts, ctx: dict, suite: Path, tier: str) -> int:
    cfg = ts.SuiteConfig(**ctx["test_suite"]["config"])
    spec = ts.read_write_mix(suite) if (suite / ts.WRITE_MIX).is_file() else cfg.write_mix
    if not spec:
        return 0
    seed = int(os.environ.get("ACCEL_SUITE_MIX_SEED") or 0) or ts.fresh_seed()
    samples = int(spec["frequency_pct"]["samples"]) if tier == "acceptance" else int(os.environ.get("ACCEL_SUITE_QUICK_SAMPLES", 2))
    print(json.dumps({"seed": seed, "frequencies": ts.draw_write_frequencies(spec, samples, seed)}))
    return 0


def main(argv: list[str]) -> int:
    root, command, rest = Path(argv[0]), argv[1], argv[2:]
    if command == "summary":
        return summary(rest[0])
    ctx = _context()
    suite = _suite(ctx)
    if suite is None:
        return 0
    sys.path.insert(0, str(root / "pipeline" / "orchestrator"))
    from accel_pipeline import test_suite as ts

    if command == "prepare":
        return prepare(ts, ctx, suite, rest[0])
    if command == "validate":
        return validate(ts, ctx, suite, rest[0] if rest else None)
    if command == "mix":
        return mix(ts, ctx, suite, rest[0])
    if command == "freeze":
        print(json.dumps(ts.freeze(Path(ctx["artifacts_dir"]), suite, query=int(ctx["query_number"]))))
        return 0
    raise SystemExit(f"unknown command {command}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
