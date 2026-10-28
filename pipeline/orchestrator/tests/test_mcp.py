"""The MCP tool surface: the data tools, the quick_check, background-gate and budget-footer tools, the allowlisted
shell command and the JSON Patch the analysis edit tool applies."""
from __future__ import annotations

import json
import time
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest
from fake_engine import make_engine

from accel_pipeline import mcp
from accel_pipeline.config import load_pipeline_config, PipelineConfig, QuickTierConfig
from accel_pipeline.gates import QuickResult
from accel_pipeline.mcp import (
    _command_allowed,
    _normalize_ctest_cmd,
    apply,
    check_read_only,
    make_data_tools,
    make_shell_tools,
    PatchError,
    ShellToolContext,
    ToolContext,
)
from accel_pipeline.repair import Failure


@pytest.fixture()
def ctx(tmp_path, monkeypatch):
    return make_engine(tmp_path, monkeypatch)


@pytest.fixture()
def tools(ctx):
    return make_data_tools(ctx)


def _log(ctx):
    return [json.loads(l) for l in (ctx.art / mcp.LOG_FILE).read_text().splitlines()]


@pytest.mark.parametrize("sql", [
    "INSERT INTO orders VALUES (1, 'a', 'r', '2020-01-01')",
    "UPDATE orders SET kind = 'x'",
    "DELETE FROM orders",
    "CREATE TABLE t(a INT)",
    "DROP TABLE orders",
    "ALTER TABLE orders ADD COLUMN z INT",
    "ATTACH 'other.db' AS o",
    "COPY orders TO '/tmp/x.csv'",
    "PRAGMA database_size",
    "SET threads = 1",
    "SELECT 1; SELECT 2",
    "WITH d AS (DELETE FROM orders RETURNING *) SELECT * FROM d",
    "SELECT * INTO t2 FROM orders",
    "SELECT * FROM read_csv('/etc/passwd')",
    "",
])
def test_writes_and_escapes_are_refused(sql):
    assert check_read_only(sql)


@pytest.mark.parametrize("sql", [
    "SELECT count(*) FROM orders;",
    "WITH a AS (SELECT 1 x) SELECT * FROM a",
    "EXPLAIN SELECT * FROM orders",
    "DESCRIBE orders",
    "SUMMARIZE orders",
    "SELECT 'DROP TABLE x; --' AS note, \"update\" FROM (SELECT 1 AS \"update\")",
])
def test_reads_are_allowed(sql):
    assert check_read_only(sql) is None


def test_refused_statement_reaches_no_engine_and_is_logged(tools, ctx):
    out = tools["query_data"]("DROP TABLE orders")
    assert out.startswith("error: refused")
    assert tools["query_data"]("SELECT count(*) AS n FROM orders").splitlines()[2] == "100"
    assert _log(ctx)[0]["error"]


def test_query_data_returns_csv_and_logs_the_call(tools, ctx):
    out = tools["query_data"]("SELECT kind, count(*) AS n FROM orders GROUP BY kind ORDER BY kind")
    head, header, *rows = out.splitlines()[:5]
    assert head == "-- 3 rows" and header == "kind,n" and rows[0] == "k0,33"
    record = _log(ctx)[0]
    assert record["tool"] == "query_data" and record["rows"] == 3 and record["seconds"] >= 0 and "GROUP BY" in record["sql"]


def test_engine_errors_come_back_as_text(tools):
    assert "no such table" in tools["query_data"]("SELECT * FROM missing")


def test_row_cap(tools):
    out = tools["query_data"]("SELECT * FROM orders", max_rows=7)
    assert "capped at 7 rows, more exist" in out
    assert len(out.splitlines()) == 1 + 1 + 7 + 1  # note, header, rows, timing


def test_time_box(tmp_path, monkeypatch):
    tools = make_data_tools(make_engine(tmp_path, monkeypatch, query_seconds=0.5))
    slow = ("WITH RECURSIVE r(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM r) "
            "SELECT count(*) FROM r")
    started = time.monotonic()
    out = tools["query_data"](slow)
    assert "time box" in out and time.monotonic() - started < 10


def test_engine_must_be_up(tmp_path, monkeypatch):
    tools = make_data_tools(make_engine(tmp_path, monkeypatch, up=False))
    for call in (lambda: tools["query_data"]("SELECT 1"), lambda: tools["column_stats"](["orders.id"])):
        assert "instance is not up" in call()


def test_column_stats(tools):
    out = tools["column_stats"](["orders.placed", "Orders.KIND", "events.amount"])
    assert "exact (tables are at most" in out
    assert "| orders.placed | INTEGER | 0.0% | 100 | 1 | 1 | 50 | 99 | 100 |" in out
    assert "| orders.kind | TEXT | 0.0% | 3 | k0 | - | - | - | k2 |" in out and "events.amount" in out


def test_unknown_column_lists_the_choices(tools):
    assert "unknown column 'nope' in orders; columns: id, kind, region, placed" in tools["column_stats"](["orders.nope"])


def test_join_fanout(tools):
    out = tools["join_fanout"]("events.order_id", "orders.id")
    assert "events.order_id -> orders.id" in out and "orders.id -> events.order_id" in out
    assert "| 100 | 4 | 4 | 4 | 100.0% |" in out


def test_rows_qualifying_quotes_values(tools):
    out = tools["rows_qualifying"]("orders.placed < $1", ["0", "30", "200"])
    rows = [l for l in out.splitlines() if l.startswith("| 0") or l.startswith("| 30") or l.startswith("| 200")]
    assert [r.split("|")[2].strip() for r in rows] == ["0", "29", "100"]
    assert "table orders (100 rows)" in out
    assert tools["rows_qualifying"]("kind = $1", ["k1"]).count("| 34 |") == 1  # a bare column resolves its table


def test_rows_qualifying_refuses_writes_and_other_parameters(tools):
    assert "refused" in tools["rows_qualifying"]("id IN (SELECT 1; DROP TABLE orders)", ["1"])
    assert "only $1" in tools["rows_qualifying"]("id < $2", ["1"])


def test_sampling_is_stated_and_counts_rescaled(tools, monkeypatch):
    monkeypatch.setattr(mcp.data, "SAMPLE_ROWS", 40)
    out = tools["rows_qualifying"]("orders.placed < $1", ["200"])
    assert "sampled: about 40 rows of orders (100 rows)" in out
    count = int(out.splitlines()[-3].split("|")[2].replace(",", ""))
    assert 80 <= count <= 120  # key-modulo sample (step 3: 34 rows) scaled back


def test_column_spread_over_a_join(tools):
    out = tools["column_spread"]("events.happened", "orders.placed", "events.order_id = orders.id")
    assert "in units" in out and "| 400 |" in out
    row = [l for l in out.splitlines() if l.startswith("| 400")][0].split("|")
    assert (float(row[2]), float(row[6])) == (4, 10)  # min, max of happened - placed


def test_column_spread_needs_the_join(tools):
    assert "join_sql is required" in tools["column_spread"]("events.happened", "orders.placed")


def test_query_shape_runs_on_the_engine(tools):
    out = tools["query_shape"]()
    assert "events.order_id -> orders.id" in out and "orders.placed - events.happened" in out
    assert "### Parameterised predicates" in out


def test_no_code_path_names_an_engine_or_benchmark():
    import re
    from pathlib import Path

    package = Path(mcp.__file__).parent
    repo = package.parents[2]
    names = [p.name for p in (repo / "backends").iterdir() if p.is_dir()]
    names += [p.name for p in (repo / "workloads").iterdir() if p.is_dir()]
    forbidden = re.compile(r"\b(" + "|".join(map(re.escape, names)) + r")\b")
    for name in ("mcp.py", "data.py"):
        assert not forbidden.search((package / name).read_text().lower()), name


def test_concurrent_calls_run_one_statement_at_a_time_on_the_instance(ctx, tools, monkeypatch):
    """An embedded engine's database file takes one process: parallel tool calls queue instead of failing on the lock."""
    import subprocess
    import threading

    active, peak, guard = [0], [0], threading.Lock()
    real = subprocess.Popen

    class Probe(real):
        def __init__(self, *a, **k):
            super().__init__(*a, **k)
            with guard:
                active[0] += 1
                peak[0] = max(peak[0], active[0])

        def wait(self, *a, **k):
            try:
                return super().wait(*a, **k)
            finally:
                with guard:
                    active[0] -= 1

    monkeypatch.setattr(mcp.subprocess, "Popen", Probe)
    threads = [threading.Thread(target=tools["query_data"], args=("SELECT count(*) FROM orders",)) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert peak[0] == 1


CONFIG = SimpleNamespace(quick_tier=QuickTierConfig())
WORKLOAD = {"workload_kind": "aux_state", "benchmark_name": "tpch"}


def test_quick_check_passes_its_arguments_on_and_renders_the_result(monkeypatch):
    seen = {}

    def fake(config, workload, query, *, targets, differential, sanitizer, threads):
        seen.update(query=query, targets=targets, differential=differential, sanitizer=sanitizer, threads=threads)
        return QuickResult("pass", {"host_build": 0.5}, summary="quick_check passed")

    monkeypatch.setattr(mcp, "run_quick_check", fake)
    quick_check = mcp.make_quick_tools(ToolContext(CONFIG, 6, WORKLOAD))
    out = json.loads(quick_check(["my_test"], False, "thread", 6))
    assert seen == {"query": 6, "targets": ["my_test"], "differential": False, "sanitizer": "thread", "threads": 6}
    assert out == {"status": "pass", "summary": "quick_check passed", "phase_seconds": {"host_build": 0.5}}


def test_a_failed_quick_check_shows_the_failure_and_how_to_reproduce(monkeypatch):
    failure = Failure("code", "quick_check", "quick_check", "differential failed", evidence="row 3", reproduce="host.sh run d 6")
    monkeypatch.setattr(mcp, "run_quick_check", lambda *a, **k: QuickResult("fail", failure=failure, summary="x"))
    out = json.loads(mcp.make_quick_tools(ToolContext(CONFIG, 6, WORKLOAD))())
    assert out["status"] == "fail" and "Reproduce: `host.sh run d 6`" in out["failure"]


def test_request_gate_in_the_background_starts_it_and_returns_at_once(monkeypatch):
    started = {}

    def fake_start(config, workload, query, gate, *, workload_name):
        started.update(gate=gate, workload_name=workload_name)
        return {"gate": gate, "status": "running", "pid": 1, "stamp_start": "s"}

    monkeypatch.setattr(mcp, "start_background_gate", fake_start)
    monkeypatch.setattr(mcp, "run_gate", lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran in the foreground")))
    request_gate, _ = mcp.make_gate_tools(ToolContext(CONFIG, 6, WORKLOAD, workload_name="tpch_q6_aux"))
    out = json.loads(request_gate("aux_unit", background=True))
    assert started == {"gate": "aux_unit", "workload_name": "tpch_q6_aux"}
    assert out == {"gate": "aux_unit", "status": "running"}


def test_gate_status_reports_a_finished_gate_compactly_and_whether_it_is_stale(monkeypatch):
    state = {"gate": "aux_unit", "status": "fail", "stale": True,
             "data": {"status": "fail", "reason": "boom", "log_tail": "tail"}}
    monkeypatch.setattr(mcp, "background_status", lambda *a: state)
    _, gate_status = mcp.make_gate_tools(ToolContext(CONFIG, 6, WORKLOAD))
    out = json.loads(gate_status("aux_unit"))
    assert out["status"] == "fail" and out["stale"] is True and out["reason"] == "boom"
    assert "log_tail" not in out and "data" not in out


def test_the_budget_footer_is_appended_after_a_blank_line(monkeypatch):
    monkeypatch.setattr("accel_pipeline.mcp.budget_footer", lambda config, stage, query: "WRAP UP: checkpoint now")
    assert mcp._budget_footer(CONFIG, "optimization", 6) == "\n\nWRAP UP: checkpoint now"


def test_an_empty_footer_adds_nothing_and_a_broken_one_never_fails_the_call(monkeypatch):
    monkeypatch.setattr("accel_pipeline.mcp.budget_footer", lambda config, stage, query: "")
    assert mcp._budget_footer(CONFIG, "s", 1) == ""

    def broken(config, stage, query):
        raise RuntimeError("no session")

    monkeypatch.setattr("accel_pipeline.mcp.budget_footer", broken)
    assert mcp._budget_footer(CONFIG, "s", 1) == ""


def test_heavy_gates_are_refused_before_a_passing_quick_check_in_the_foreground_and_the_background(monkeypatch, tmp_path):
    monkeypatch.setattr(mcp, "start_background_gate", lambda *a, **k: (_ for _ in ()).throw(AssertionError("started")))
    monkeypatch.setattr(mcp, "run_gate", lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran")))
    config = SimpleNamespace(repo_root=tmp_path, quick_tier=CONFIG.quick_tier)  # not a git tree: no stamp, no admission
    request_gate, _ = mcp.make_gate_tools(ToolContext(config, 6, WORKLOAD))
    for background in (False, True):
        out = json.loads(request_gate("aux_correctness", background=background))
        assert out["status"] == "refused" and "run quick_check first" in out["reason"]


def test_an_admitted_gate_runs(monkeypatch):
    ran = []
    monkeypatch.setattr(mcp, "run_gate", lambda config, name, *a, **k: ran.append(name) or SimpleNamespace(name=name, data={"status": "pass"}))
    monkeypatch.setattr(mcp, "admission_refusal_payload", lambda ctx, gate: "")
    request_gate, *_ = mcp.make_gate_tools(ToolContext(CONFIG, 6, WORKLOAD))
    assert json.loads(request_gate("aux_correctness"))["status"] == "pass" and ran == ["aux_correctness"]


def test_run_correctness_is_refused_like_request_gate(tmp_path):
    root = Path(__file__).resolve().parents[3]
    config = replace(load_pipeline_config(repo_root=root), repo_root=tmp_path)
    tools = mcp.build_mcp_tool_callables(config, WORKLOAD, "w", 6, "implementation", 0, tmp_path)
    out = json.loads(tools["run_correctness"]())
    assert out["gate"] == "aux_correctness" and out["status"] == "refused"


def test_normalize_ctest_cmd_adds_test_dir(tmp_path: Path) -> None:
    repo = tmp_path
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        allowed_commands=[],
    )
    assert _normalize_ctest_cmd(
        "ctest -R tpch_q21_aux --output-on-failure", pipeline_config
    ) == "ctest --test-dir build-release -R tpch_q21_aux --output-on-failure"
    assert _normalize_ctest_cmd(
        "ctest --test-dir build-release -R tpch_q21_aux", pipeline_config
    ) == "ctest --test-dir build-release -R tpch_q21_aux"


def test_builtin_diagnostic_commands_allowed_without_yaml_patterns(tmp_path: Path) -> None:
    pipeline_config = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release",
        allowed_commands=[],
    )
    run_command = make_shell_tools(ShellToolContext(pipeline_config, 1))
    (tmp_path / "CMakeLists.txt").write_text("# cmake\n", encoding="utf-8")

    assert "exit=0" in run_command("pwd", timeout=5)
    assert "exit=0" in run_command("ls CMakeLists.txt", timeout=5)
    assert "exit=0" in run_command("pwd && ls CMakeLists.txt", timeout=5)

    with pytest.raises(PermissionError, match="not allowlisted"):
        run_command("cat CMakeLists.txt", timeout=5)


def test_command_allowed_helper() -> None:
    assert _command_allowed("pwd", [])
    assert _command_allowed("ls build-release", [])
    assert not _command_allowed("rm -rf build-release", [])


def test_run_command_kills_process_group_on_timeout(tmp_path: Path) -> None:
    repo = tmp_path
    (repo / "sleep.sh").write_text("#!/bin/sh\nsleep 30\n", encoding="utf-8")
    (repo / "sleep.sh").chmod(0o755)
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        allowed_commands=[r"^./sleep\.sh$"],
    )
    run_command = make_shell_tools(ShellToolContext(pipeline_config, 1))
    start = time.monotonic()
    out = run_command("./sleep.sh", timeout=1)
    elapsed = time.monotonic() - start
    assert "exit=124" in out
    assert "timed out" in out
    assert elapsed < 5


DOC = {"design": {"storage": [{"component": "a", "primitive": "X"}, {"component": "b", "primitive": "X"}]}}


def test_replace_add_remove_on_nested_lists_and_keys():
    out = apply(DOC, [
        {"op": "replace", "path": "/design/storage/1/primitive", "value": "CowMap"},
        {"op": "add", "path": "/design/storage/-", "value": {"component": "c", "primitive": "CowPagedArray"}},
        {"op": "add", "path": "/design/note", "value": "grouped"},
        {"op": "remove", "path": "/design/storage/0"},
    ])
    assert [s["primitive"] for s in out["design"]["storage"]] == ["CowMap", "CowPagedArray"]
    assert out["design"]["note"] == "grouped" and DOC["design"]["storage"][1]["primitive"] == "X"  # input untouched


def test_escaped_tokens_and_errors():
    assert apply({"a/b": {"~k": 1}}, [{"op": "replace", "path": "/a~1b/~0k", "value": 2}]) == {"a/b": {"~k": 2}}
    for op in ({"op": "replace", "path": "/design/missing", "value": 1},
               {"op": "remove", "path": "/design/storage/5"},
               {"op": "move", "path": "/design"},
               {"op": "add", "path": "design", "value": 1}):
        with pytest.raises(PatchError):
            apply(DOC, [op])


def test_the_reanalysis_tool_names_every_bottleneck_type_it_accepts():
    import re

    from accel_pipeline.stages import BOTTLENECK_TYPES

    text = (Path(__file__).parents[1] / "accel_pipeline" / "mcp.py").read_text()
    named = re.search(r"bottleneck_type: exactly one of ([^(]+)\(", text).group(1)
    assert {t.strip() for t in named.replace("\n", " ").split(",")} == set(BOTTLENECK_TYPES)



def test_a_gate_loading_its_dataset_says_the_wait_is_expected():
    from accel_pipeline.mcp import LOADING_NOTE, _compact_background_state

    state = {"gate": "aux_correctness", "status": "running", "phase": "host load workloads/b/schema.sql", "elapsed_sec": 228}
    assert _compact_background_state(state)["note"] == LOADING_NOTE
    assert "note" not in _compact_background_state({**state, "phase": "run diff_bin"})
