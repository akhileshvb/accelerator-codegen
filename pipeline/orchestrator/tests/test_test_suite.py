"""The per-query test suite: format validation, seeded draws, freezing, the stage flow and its gate plumbing."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from dataclasses import asdict
from pathlib import Path

import pytest
import yaml

from accel_pipeline import repair as repair_mod
from accel_pipeline import test_suite as ts
from accel_pipeline.aux_bench import aggregate_sweep_repeats
from accel_pipeline.config import PipelineConfig, RepairConfig
from accel_pipeline.failure import Failure, failure_from_gate
from accel_pipeline.gates import GateResult
from accel_pipeline.state import STAGES, PipelineState
from accel_pipeline.write_ownership import layer_write_verdict

QUERY_SQL = "SELECT k, SUM(v) FROM t WHERE seg = $1 AND d < $2 GROUP BY k ORDER BY 2 DESC LIMIT 10\n"
GATES = Path(__file__).resolve().parents[2] / "gates"
PROMPTS = Path(__file__).resolve().parents[2] / "prompts"

MANIFEST = {
    "version": 1,
    "query": 3,
    "rationale": "t is written by every kind; seg and d decide membership.",
    "edge_coverage": {
        "in_spec": [1, 2],
        "off_spec": [3],
        "null_param": [5],
        "empty_result": [4],
        "limit_ties": "n/a: the toy answer has no ties at the cut",
        "extreme_values": [3],
        "large_keys": "n/a: no key parameter in the toy query",
    },
    "txns": {
        "001.sql": {"kind": "point_insert"},
        "002.sql": {"kind": "point_update"},
        "003.sql": {"kind": "point_delete"},
        "004.sql": {"kind": "rollback"},
        "005.sql": {"kind": "key_move"},
        "006.sql": {"kind": "bulk_insert"},
    },
    "point_writes": {"min_count": 3, "max_probes": 8, "tables": ["t"]},
    "bulk_writes": {"required": True, "min_chunks": 2},
    "scale_scenarios": ["keys beyond 2^32 written by 006"],
}
DOMAIN = {
    "version": 1,
    "params": [
        {"name": "seg", "type": "choice", "values": ["'A'", "'B'"]},
        {"name": "d", "type": "date", "min": "1992-01-02", "max": "1998-12-01"},
    ],
}
WRITE_MIX = {
    "version": 1,
    "frequency_pct": {"min": 1, "max": 50, "samples": 3, "distribution": "log"},
    "kinds": {"point_update": 5, "point_insert": 2, "bulk_insert": 1},
}
TXNS = {
    "001.sql": "INSERT INTO t VALUES (1, 'A', DATE '1995-01-01', 1);",
    "002.sql": "UPDATE t SET v = v + 1 WHERE k = 1;",
    "003.sql": "DELETE FROM t WHERE k = 1;",
    "004.sql": "-- rollback\nUPDATE t SET v = 0;",
    "005.sql": "UPDATE t SET seg = 'B' WHERE k = 2;",
    "006.sql": "INSERT INTO t SELECT k + 4294967296, seg, d, v FROM t;",
}
PARAMS = ["'A', DATE '1995-03-15'", "'B', DATE '1996-01-01'", "'a', DATE '9999-12-31'", "'NONE', DATE '1995-03-15'",
          "NULL, DATE '1995-03-15'"]


def write_suite(root: Path, *, params=PARAMS, manifest=MANIFEST, domain=DOMAIN, mix=WRITE_MIX, txns=TXNS) -> Path:
    suite = root / "suite"
    (suite / "txns").mkdir(parents=True, exist_ok=True)
    (suite / "params.sql").write_text("-- fixed\n" + "\n".join(params) + "\n")
    for name, body in (("suite.yaml", manifest), ("domain.yaml", domain), ("write_mix.yaml", mix)):
        if body is not None:
            (suite / name).write_text(yaml.safe_dump(body))
    for name, sql in txns.items():
        (suite / "txns" / name).write_text(sql)
    return suite


def problems(suite: Path, **kw) -> list[str]:
    args = dict(query_sql=QUERY_SQL, param_names=["seg", "d"], dependent_tables=["t"], aux=True)
    return ts.validate_static(suite, **{**args, **kw})


def edited(base: dict, path: list[str], value) -> dict:
    out = json.loads(json.dumps(base))
    node = out
    for key in path[:-1]:
        node = node[key]
    node[path[-1]] = value
    return out


def test_a_complete_suite_validates(tmp_path):
    assert problems(write_suite(tmp_path)) == []


BAD = {
    "a tuple of the wrong arity": (dict(params=PARAMS + ["'A'"]), "has 1 values, the query takes 2"),
    "no manifest": (dict(manifest=None), "suite.yaml"),
    "no domain": (dict(domain=None), "domain.yaml"),
    "a domain of the wrong width": (dict(domain=edited(DOMAIN, ["params"], DOMAIN["params"][:1])), "one `params` entry per query parameter"),
    "a derived parameter without a date source": (dict(domain=edited(DOMAIN, ["params", 1], {"name": "d", "type": "derived", "from": "seg", "add_months": 1})), "cannot draw tuples"),
    "a domain type that does not exist": (dict(domain=edited(DOMAIN, ["params", 0, "type"], "uuid")), "needs a name and a type"),
    "a domain without its range": (dict(domain=edited(DOMAIN, ["params", 1], {"name": "d", "type": "date", "min": "1992-01-01"})), "`max` missing"),
    "domain names that are not the workload's": (dict(domain=edited(DOMAIN, ["params", 0, "name"], "other")), "must be the workload's"),
    "no write mix": (dict(mix=None), "write_mix.yaml"),
    "a write frequency range above 100": (dict(mix=edited(WRITE_MIX, ["frequency_pct", "max"], 150)), "0 < min <= max <= 100"),
    "no write kinds": (dict(mix=edited(WRITE_MIX, ["kinds"], {})), "relative weights"),
    "a weight for a kind no transaction has": (dict(mix=edited(WRITE_MIX, ["kinds", "bulk_delete"], 1)), "kinds no transaction has"),
    "an edge class left out": (dict(manifest={**MANIFEST, "edge_coverage": {k: v for k, v in MANIFEST["edge_coverage"].items() if k != "null_param"}}), "edge_coverage.null_param"),
    "an edge tuple out of range": (dict(manifest=edited(MANIFEST, ["edge_coverage", "in_spec"], [1, 99])), "edge_coverage.in_spec"),
    "a waiver without a reason": (dict(manifest=edited(MANIFEST, ["edge_coverage", "large_keys"], "n/a")), "edge_coverage.large_keys"),
    "transactions the manifest does not list": (dict(manifest=edited(MANIFEST, ["txns"], {"001.sql": {"kind": "point_insert"}})), "exactly the files"),
    "a transaction kind that does not exist": (dict(manifest=edited(MANIFEST, ["txns", "001.sql", "kind"], "magic")), "must be one of"),
    "no transactions": (dict(txns={}), "no write transactions"),
    "no DELETE on a dependent table": (dict(txns={**TXNS, "003.sql": "UPDATE t SET v = 0;"}), "no transaction runs DELETE on dependent table t"),
    "no rollback transaction": (dict(manifest=edited(MANIFEST, ["txns", "004.sql", "kind"], "other")), "no `rollback` transaction"),
    "a rollback transaction without the marker": (dict(txns={**TXNS, "004.sql": "UPDATE t SET v = 0;"}), "must be `-- rollback`"),
    "point writes below the minimum": (dict(manifest=edited(MANIFEST, ["point_writes", "min_count"], 2)), "min_count"),
    "bulk required without a bulk transaction": (dict(manifest=edited(MANIFEST, ["txns", "006.sql", "kind"], "other")), "no bulk_* transaction"),
    "no scale scenarios": (dict(manifest=edited(MANIFEST, ["scale_scenarios"], [])), "scale_scenarios"),
    "no rationale": (dict(manifest=edited(MANIFEST, ["rationale"], " ")), "rationale"),
}


@pytest.mark.parametrize("name", BAD)
def test_each_validation_rule_fails_a_crafted_bad_suite(tmp_path, name):
    kwargs, expected = BAD[name]
    found = problems(write_suite(tmp_path, **kwargs))
    assert any(expected in p for p in found), found


def test_a_stateful_suite_needs_the_analysis_tables_to_check_coverage(tmp_path):
    assert any("no dependent tables" in p for p in problems(write_suite(tmp_path), dependent_tables=[]))


def test_waivers_with_a_reason_stand_in_for_missing_writes(tmp_path):
    manifest = {**MANIFEST, "not_applicable": {"t:DELETE": "rows leave the answer only through the update of 005"}}
    suite = write_suite(tmp_path, manifest=manifest, txns={**TXNS, "003.sql": "UPDATE t SET v = 0;"})
    assert problems(suite) == []


def test_a_stateless_kernel_needs_no_writes(tmp_path):
    suite = write_suite(tmp_path, manifest={k: MANIFEST[k] for k in ("version", "query", "rationale", "edge_coverage")},
                        mix=None, txns={})
    assert problems(suite, aux=False) == []


def report(**over) -> dict:
    return {"errors": [], "tuple_rows": [3, 2, 1, 0, 1],
            "txn_effects": [{"txn": n, "changes_answers_of": 2} for n in TXNS], **over}


def test_a_clean_run_report_passes(tmp_path):
    assert ts.validate_report(report(), write_suite(tmp_path), ["t"], True) == []


def test_a_table_no_transaction_changes_the_answer_of_is_reported(tmp_path):
    silent = report(txn_effects=[{"txn": n, "changes_answers_of": 0} for n in TXNS])
    found = ts.validate_report(silent, write_suite(tmp_path), ["t"], True)
    assert any("no transaction on t changes the answer" in p for p in found)


def test_edge_claims_are_checked_against_the_run(tmp_path):
    suite = write_suite(tmp_path)
    found = ts.validate_report(report(tuple_rows=[3, 2, 1, 7, 1]), suite, ["t"], True)
    assert any("empty_result: tuple 4 returns 7 rows" in p for p in found)
    found = ts.validate_report(report(), write_suite(tmp_path / "x", params=PARAMS[:4] + ["'A', DATE '1995-03-15'"]), ["t"], True)
    assert any("null_param: tuple 5 contains no NULL" in p for p in found)


def test_errors_of_the_run_are_problems(tmp_path):
    assert ts.validate_report(report(errors=["txn 2: boom"]), write_suite(tmp_path), ["t"], True) == ["txn 2: boom"]


def test_draws_are_reproducible_from_the_seed(tmp_path):
    suite = write_suite(tmp_path)
    domain = yaml.safe_load((suite / "domain.yaml").read_text())
    first = ts.draw_tuples(domain, 20, 42)
    assert first == ts.draw_tuples(domain, 20, 42) != ts.draw_tuples(domain, 20, 43)
    assert all(len(ts.split_tuple(t)) == 2 for t in first) and len(set(first)) > 10
    mix = yaml.safe_load((suite / "write_mix.yaml").read_text())
    freqs = ts.draw_write_frequencies(mix, 3, 42)
    assert freqs == ts.draw_write_frequencies(mix, 3, 42)
    assert freqs[0] == 0 and len(freqs) == 4 and all(1 <= f <= 50 for f in freqs[1:]) and freqs == sorted(set(freqs))
    assert len({tuple(ts.draw_write_frequencies(mix, 3, s)) for s in range(30)}) > 5


def test_derived_parameters_follow_their_source():
    domain = {"params": [{"name": "start", "type": "date", "min": "1993-01-01", "max": "1997-12-31"},
                         {"name": "end", "type": "derived", "from": "start", "add_months": 3},
                         {"name": "pattern", "type": "pattern", "template": "%{}%", "words": ["red", "green"]},
                         {"name": "q", "type": "decimal", "min": 1, "max": 2, "scale": 2},
                         {"name": "n", "type": "int", "min": 5, "max": 6}]}
    for tuple_ in ts.draw_tuples(domain, 30, 1):
        start, end, pattern, q, n = ts.split_tuple(tuple_)
        month = int(start[-6:-4]) - 1
        assert int(end[-6:-4]) - 1 == (month + 3) % 12
        assert pattern in ("'%red%'", "'%green%'") and 1 <= float(q) <= 2 and n in ("5", "6")


def test_a_run_environment_is_its_seed_and_tier(tmp_path):
    suite = write_suite(tmp_path)
    cfg = ts.SuiteConfig(acceptance_random_tuples=7, quick_random_tuples=2, quick_txns=3, differential_every=4)
    full = ts.run_environment(suite, tier="full", cfg=cfg, seed=5, params_file=tmp_path / "full.sql")
    again = ts.run_environment(suite, tier="full", cfg=cfg, seed=5, params_file=tmp_path / "again.sql")
    assert (tmp_path / "full.sql").read_text() == (tmp_path / "again.sql").read_text()
    assert len((tmp_path / "full.sql").read_text().splitlines()) == 7
    assert full["ACCEL_SUITE_SEED"] == "5" and full["ACCEL_SUITE_EVERY"] == "4" and "ACCEL_SUITE_TXNS" not in full
    quick = ts.run_environment(suite, tier="quick", cfg=cfg, seed=5, params_file=tmp_path / "quick.sql")
    assert len((tmp_path / "quick.sql").read_text().splitlines()) == 2 and quick["ACCEL_SUITE_EVERY"] == "0"
    assert quick["ACCEL_SUITE_TXNS"].split(",") == ["001.sql", "002.sql", "003.sql"]
    fresh = ts.run_environment(suite, tier="full", cfg=cfg, seed=None, params_file=tmp_path / "fresh.sql")
    assert fresh["ACCEL_SUITE_SEED"] != "0" and again == {**full, "ACCEL_SUITE_PARAMS_FILE": again["ACCEL_SUITE_PARAMS_FILE"]}


def test_the_quick_tier_takes_the_first_transaction_of_each_kind_and_table(tmp_path):
    suite = write_suite(tmp_path, txns={**TXNS, "002.sql": "INSERT INTO t VALUES (9, 'A', DATE '1995-01-01', 1);"},
                        manifest=edited(MANIFEST, ["txns", "002.sql", "kind"], "point_insert"))
    assert ts.quick_txn_names(suite, 10) == ["001.sql", "003.sql", "004.sql", "005.sql", "006.sql"]


def test_a_frozen_suite_cannot_be_edited_and_a_change_request_is_the_way(tmp_path):
    suite, art = write_suite(tmp_path), tmp_path / "art"
    art.mkdir()
    assert "never frozen" in ts.frozen_problem(art, suite)
    record = ts.freeze(art, suite, query=3)
    assert ts.frozen_problem(art, suite) == "" and record["hash"] == ts.content_hash(suite)
    (suite / "txns" / "001.sql").write_text("INSERT INTO t VALUES (2, 'B', DATE '1995-01-01', 1);")
    assert "was edited" in ts.frozen_problem(art, suite) and "change request" in ts.frozen_problem(art, suite)
    (suite / "txns" / "001.sql").write_text(TXNS["001.sql"])
    assert ts.frozen_problem(art, suite) == ""
    (suite / "extra.sql").write_text("-- new file\n")
    assert ts.frozen_problem(art, suite)
    (art / ts.CHANGE_REQUEST).write_text("001.sql inserts a duplicate key\n")
    assert ts.pending_change_request(art) == "001.sql inserts a duplicate key"
    ts.archive_change_request(art, "the tester kept the suite as it was")
    assert ts.pending_change_request(art) == ""
    assert "kept the suite" in (art / "test_suite_change_requests" / "01.md").read_text()


def test_old_pipeline_states_resume_at_the_renamed_stage(tmp_path):
    path = tmp_path / "pipeline_state.json"
    path.write_text(json.dumps({"query": 3, "workload": "w", "engine": "fake", "stage": "adversarial_tests"}))
    assert PipelineState.load(path).stage == "test_suite" and "adversarial_tests" not in STAGES
    assert STAGES.index("test_suite") == STAGES.index("analysis") + 1 == STAGES.index("implementation") - 1


def test_only_the_test_suite_stage_writes_the_suite():
    suite_file = "accelerators/toybench/q3/tests/workload/txns/001.sql"
    other_file = "accelerators/toybench/q3/aux/module.cpp"
    for stage in ("implementation", "optimization_worker", "crash_debug", "audit"):
        ok, why = layer_write_verdict(suite_file, query=3, benchmark="toybench", stage=stage)
        assert not ok and "test_suite_change_request" in why
        assert layer_write_verdict(other_file, query=3, benchmark="toybench", stage=stage)[0]
    assert layer_write_verdict(suite_file, query=3, benchmark="toybench", stage="test_suite")[0]
    ok, why = layer_write_verdict(other_file, query=3, benchmark="toybench", stage="test_suite")
    assert not ok and "tests/workload" in why


# ---- the stage under the repair loop

class World:
    """A tmp repo whose tester is scripted: each turn writes the next suite; the gate is the real static check."""

    def __init__(self, tmp_path, monkeypatch, suites):
        from accel_pipeline.stages import test_suite as stage

        self.stage, self.tmp, self.messages, self.roles = stage, tmp_path, [], []
        self.config = PipelineConfig(repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
                                     tpch_db_sf1=tmp_path, tpch_db_sf10=tmp_path, sandbox="none", write_policy="off",
                                     repair=RepairConfig())
        self.workload = {"benchmark_name": "toybench", "workload_kind": "aux_state"}
        for module in (stage, repair_mod):
            monkeypatch.setattr(module, "load_run_workload", lambda cfg, name: self.workload)
        self.art = tmp_path / "pipeline" / "artifacts" / "toybench" / "q3"
        self.art.mkdir(parents=True)
        (self.art / "analysis.json").write_text("{}")
        monkeypatch.setattr(stage, "query_artifacts_dir", lambda *a, **k: self.art)
        monkeypatch.setattr(stage, "analysis_artifact_path", lambda *a, **k: self.art / "analysis.json")
        self.directory = tmp_path / "accelerators" / "toybench" / "q3" / "tests" / "workload"
        self.suites = list(suites)
        monkeypatch.setattr(stage, "run_agent_stage", self._agent)
        monkeypatch.setattr(repair_mod, "_run_gates", self._gates)
        monkeypatch.setattr(stage, "append_experiment_log", lambda *a, **k: None)
        monkeypatch.setattr(repair_mod, "append_experiment_log", lambda *a, **k: None)

    def _agent(self, **kw):
        self.messages.append(kw["user_message"])
        self.roles.append(kw["session_role"])
        suite = self.suites.pop(0) if len(self.suites) > 1 else self.suites[0]
        write_suite(self.tmp / "tmp_suite", **suite)
        import shutil
        shutil.rmtree(self.directory, ignore_errors=True)
        shutil.copytree(self.tmp / "tmp_suite" / "suite", self.directory)
        return {}

    def _gates(self, ctx, names):
        found = ts.validate_static(self.directory, query_sql=QUERY_SQL, param_names=["seg", "d"], dependent_tables=["t"], aux=True)
        if not found:
            return [GateResult(names[0], "pass", {})], None
        result = GateResult(names[0], "fail", {"log_tail": "\n".join(found), "returncode": 1})
        return [result], result

    def run(self, **kw):
        return self.stage.run(self.config, None, "toy", 3, **kw)


def test_a_suite_that_fails_validation_goes_back_to_the_tester_then_is_frozen(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, [dict(manifest=edited(MANIFEST, ["scale_scenarios"], [])), {}])
    world.run()
    assert len(world.messages) == 2 and "scale_scenarios" in world.messages[1]
    assert world.roles == ["tester", "tester"]
    assert ts.frozen_problem(world.art, world.directory) == ""


def test_a_change_request_reruns_the_tester_and_archives_the_outcome(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, [{}, dict(txns={**TXNS, "001.sql": "INSERT INTO t VALUES (7, 'A', DATE '1995-01-01', 1);"})])
    world.run()
    (world.art / ts.CHANGE_REQUEST).write_text("001.sql: key 1 already exists in t")
    world.stage.revise(world.config, None, "toy", 3)
    assert "key 1 already exists" in world.messages[-1] and "change request" in world.messages[-1]
    assert ts.pending_change_request(world.art) == "" and ts.frozen_problem(world.art, world.directory) == ""
    assert "changed the suite" in (world.art / "test_suite_change_requests" / "01.md").read_text()


def test_a_repair_turn_of_another_stage_hands_a_pending_change_request_to_the_tester(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, [{}])
    world.run()
    (world.art / ts.CHANGE_REQUEST).write_text("003.sql deletes a row that is gone")
    ctx = repair_mod.RepairCtx(world.config, None, "toy", 3, stage="implementation")
    ctx.repair_turn = lambda failure: pytest.fail("the implementer must not repair around a suite the tester has to judge")
    repair_mod.repair_turn(ctx, Failure(kind="code", stage="implementation", source="gate:aux_correctness", summary="x"))
    assert "deletes a row that is gone" in world.messages[-1]


# ---- the gates' side

def gate_world(tmp_path, *, frozen=True) -> dict:
    suite = write_suite(tmp_path)
    art = tmp_path / "art"
    (art / "gates").mkdir(parents=True)
    (tmp_path / "q.sql").write_text(QUERY_SQL)
    if frozen:
        ts.freeze(art, suite, query=3)
    settings = {"dir": str(suite), "query_sql": str(tmp_path / "q.sql"), "function": "accel_toy_q3_aux", "arity": 2,
                "param_names": ["seg", "d"], "aux": True, "schema_sql": "", "config": asdict(ts.SuiteConfig())}
    ctx = {"test_suite": settings, "artifacts_dir": str(art), "query_number": 3}
    (tmp_path / "ctx.json").write_text(json.dumps(ctx))
    return ctx


def suite_gate(tmp_path, *args, **env) -> subprocess.CompletedProcess:
    root = Path(__file__).resolve().parents[3]
    return subprocess.run([sys.executable, str(GATES / "suite_gate.py"), str(root), *args], capture_output=True, text=True,
                          env={**os.environ, "ACCEL_GATE_CONTEXT": str(tmp_path / "ctx.json"), **env})


def exports(output: str) -> dict[str, str]:
    import shlex
    return {k: shlex.split(v)[0] if v else "" for k, _, v in (line.removeprefix("export ").partition("=") for line in output.splitlines())}


def test_a_gate_run_prints_its_environment_and_a_seed_reproduces_it(tmp_path):
    gate_world(tmp_path)
    first = suite_gate(tmp_path, "prepare", "full")
    assert first.returncode == 0, first.stderr
    env = exports(first.stdout)
    assert env["ACCEL_SUITE_FUNCTION"] == "accel_toy_q3_aux" and env["ACCEL_SUITE_ARITY"] == "2" and env["ACCEL_SUITE_HASH"]
    params = Path(env["ACCEL_SUITE_PARAMS_FILE"]).read_text()
    assert len(params.splitlines()) == 20
    other = exports(suite_gate(tmp_path, "prepare", "full").stdout)
    assert other["ACCEL_SUITE_SEED"] != env["ACCEL_SUITE_SEED"]
    again = suite_gate(tmp_path, "prepare", "full", ACCEL_SUITE_SEED=env["ACCEL_SUITE_SEED"])
    assert exports(again.stdout)["ACCEL_SUITE_SEED"] == env["ACCEL_SUITE_SEED"]
    assert Path(env["ACCEL_SUITE_PARAMS_FILE"]).read_text() == params


def test_a_gate_refuses_an_edited_suite(tmp_path):
    ctx = gate_world(tmp_path)
    (Path(ctx["test_suite"]["dir"]) / "params.sql").write_text("'A', DATE '2000-01-01'\n")
    proc = suite_gate(tmp_path, "prepare", "full")
    assert proc.returncode == 3 and "was edited" in proc.stderr and not proc.stdout
    assert suite_gate(tmp_path, "prepare", "validate").returncode == 0  # the stage's own validation runs before the freeze


def test_a_query_without_a_suite_runs_the_gates_as_before(tmp_path):
    ctx = gate_world(tmp_path)
    ctx["test_suite"]["dir"] = str(tmp_path / "none")
    (tmp_path / "ctx.json").write_text(json.dumps(ctx))
    for command in (("prepare", "full"), ("mix", "acceptance")):
        proc = suite_gate(tmp_path, *command)
        assert proc.returncode == 0 and proc.stdout == ""


def test_the_bench_draws_its_write_frequencies_from_the_suite(tmp_path):
    gate_world(tmp_path)
    draws = [json.loads(suite_gate(tmp_path, "mix", "acceptance").stdout) for _ in range(4)]
    assert all(d["frequencies"][0] == 0 and len(d["frequencies"]) == 4 for d in draws)
    assert len({d["seed"] for d in draws}) == 4
    pinned = [suite_gate(tmp_path, "mix", "acceptance", ACCEL_SUITE_MIX_SEED="9").stdout for _ in range(2)]
    assert pinned[0] == pinned[1]


def test_the_gate_summary_carries_seed_tuples_and_counts(tmp_path):
    (tmp_path / "r.json").write_text(json.dumps({"ok": False, "seed": 11, "params": 3, "param_tuples": ["1"], "txns": 4,
                                                 "checks": 9, "mismatches": 1, "extra": "dropped"}))
    summary = json.loads(suite_gate(tmp_path, "summary", str(tmp_path / "r.json"), ACCEL_SUITE_HASH="abc").stdout)
    assert summary["seed"] == 11 and summary["param_tuples"] == ["1"] and summary["hash"] == "abc" and "extra" not in summary
    assert json.loads(suite_gate(tmp_path, "summary", str(tmp_path / "missing.json")).stdout) == {}


def test_a_failed_gate_hands_its_reproduce_command_to_the_repair_loop(tmp_path):
    result = GateResult("aux_correctness", "fail", {"log_tail": "boom", "reproduce": "ACCEL_SUITE_SEED=7 bash run.sh 3"})
    assert failure_from_gate(result, stage="implementation", workload={}).reproduce == "ACCEL_SUITE_SEED=7 bash run.sh 3"


def test_repeated_sweeps_report_the_median_with_its_spread():
    def sweep(score, speedups):
        return {"status": "ok", "row_count": len(speedups), "composite_score": score, "composite_workload_speedup": score,
                "read_only_query_speedup": 10.0,
                "by_write_frequency_pct": {str(f): {"write_frequency_pct": f, "workload_speedup": s} for f, s in speedups.items()}}

    runs = [sweep(2.0, {0: 9.0, 3: 2.0}), sweep(4.0, {0: 11.0, 5: 4.0}), sweep(3.0, {0: 10.0, 3: 3.0})]
    draws = [{"seed": i, "frequencies": [0]} for i in range(3)]
    merged = aggregate_sweep_repeats(runs, draws)
    assert merged["composite_score"] == 3.0 and merged["score_spread"]["composite_score"] == {"median": 3.0, "min": 2.0, "max": 4.0}
    zero = merged["by_write_frequency_pct"]["0"]
    assert zero["workload_speedup"] == 10.0 and zero["repeats"] == {"n": 3, "workload_speedup": {"median": 10.0, "min": 9.0, "max": 11.0}}
    assert merged["by_write_frequency_pct"]["3"]["repeats"]["n"] == 2 and "5" in merged["by_write_frequency_pct"]
    assert [r["seed"] for r in merged["repeats"]] == [0, 1, 2]


def test_the_optimization_bench_samples_the_suites_distribution_up_to_its_cap(tmp_path, monkeypatch):
    from accel_pipeline import aux_write_freqs as freqs

    (tmp_path / "pipeline").mkdir()
    (tmp_path / "CMakeLists.txt").write_text("")
    suite = tmp_path / "accelerators" / "toybench" / "q3" / "tests" / "workload"
    suite.mkdir(parents=True)
    (suite / "write_mix.yaml").write_text(yaml.safe_dump(WRITE_MIX))
    monkeypatch.chdir(tmp_path)
    workload = {"benchmark_name": "toybench", "query_number": 3,
                "aux_bench_sweep_optimization": {"write_frequencies": [0], "write_mix_sample": {"enabled": True, "count": 2, "max_pct": 10}}}
    seen = set()
    for attempt in range(25):
        drawn = freqs.ensure_attempt_write_frequencies(tmp_path / f"art{attempt}", workload, optimization_iter=1, attempt=attempt)
        assert drawn[0] == 0 and all(1 <= f <= 10 for f in drawn[1:])
        seen.add(tuple(drawn))
    assert len(seen) > 3


# ---- prompts

@pytest.mark.parametrize("prompt", ("aux_state/01b_test_suite.md", "01b_test_suite.md"))
def test_the_tester_prompt_states_goal_ownership_and_the_mechanical_exit(prompt):
    text = (PROMPTS / prompt).read_text(encoding="utf-8")
    assert "## Goal" in text and "**Owns:**" in text and "**Never touches:**" in text and "**Judged by:**" in text
    assert "test_suite_validate" in text and "domain.yaml" in text and "fresh random tuples" in text
    assert "{{hard_guidelines}}" in text and "{{change_request}}" in text and "shared/test_suite.md" in text


def test_the_stateful_tester_prompt_covers_writes_and_the_write_mix():
    text = (PROMPTS / "aux_state/01b_test_suite.md").read_text(encoding="utf-8")
    for needle in ("each dependent table", "-- rollback", "write_mix.yaml", "scale_factor_contract.md", "bulk"):
        assert needle in text


@pytest.mark.parametrize("prompt", ("02_implementation.md", "aux_state/02_implementation.md", "03_optimization_worker.md",
                                    "03_optimization_unified.md", "aux_state/03_optimization_worker.md",
                                    "aux_state/03_optimization_unified.md"))
def test_the_implementing_stages_treat_the_suite_as_frozen(prompt):
    text = (PROMPTS / prompt).read_text(encoding="utf-8")
    assert "frozen" in text and "test_suite_change_request.md" in text and "never edit it" in text


@pytest.mark.parametrize("prompt", ("04_audit.md", "aux_state/04_audit.md"))
def test_the_audit_checks_the_suite_covers_the_query(prompt):
    text = (PROMPTS / prompt).read_text(encoding="utf-8")
    assert "frozen suite" in text and "every parameter" in text


def test_no_prompt_still_names_the_retired_stage():
    assert not [p for p in PROMPTS.rglob("*.md") if "adversarial_write_scenarios" in p.read_text(encoding="utf-8")]
