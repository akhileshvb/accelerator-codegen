from __future__ import annotations

import json
import time

import pytest
from fake_engine import make_engine

from accel_pipeline.tools import data_tools
from accel_pipeline.tools.data_tools import check_read_only, make_data_tools


@pytest.fixture()
def ctx(tmp_path, monkeypatch):
    return make_engine(tmp_path, monkeypatch)


@pytest.fixture()
def tools(ctx):
    return make_data_tools(ctx)


def _log(ctx):
    return [json.loads(l) for l in (ctx.art / data_tools.LOG_FILE).read_text().splitlines()]


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
    monkeypatch.setattr(data_tools.data, "SAMPLE_ROWS", 40)
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

    package = Path(data_tools.__file__).parent.parent
    repo = package.parents[2]
    names = [p.name for p in (repo / "backends").iterdir() if p.is_dir()]
    names += [p.name for p in (repo / "workloads").iterdir() if p.is_dir()]
    forbidden = re.compile(r"\b(" + "|".join(map(re.escape, names)) + r")\b")
    for name in ("tools/data_tools.py", "data.py"):
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

    monkeypatch.setattr(data_tools.subprocess, "Popen", Probe)
    threads = [threading.Thread(target=tools["query_data"], args=("SELECT count(*) FROM orders",)) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert peak[0] == 1
