"""The run's data: the query-shape statistics the agents query, the test cuts made out of the dataset, and the
per-query test suite (format validation, seeded draws, freezing, the stage flow and its gate plumbing)."""
from __future__ import annotations

import json
import os
import subprocess
import sys
from dataclasses import asdict
from pathlib import Path

import pytest
import yaml
from fake_engine import make_engine, QUERY

from accel_pipeline import data as data_mod, repair as repair_mod
from accel_pipeline.config import PipelineConfig, RepairConfig, SuiteConfig
from accel_pipeline.data import (
    build_data_shape_markdown,
    cut_selects,
    CutDatasets,
    Dataset,
    ensure_cut_datasets,
    foreign_key_links,
    key_links,
)
from accel_pipeline.gates import GateResult
from accel_pipeline.layout import layer_write_verdict
from accel_pipeline.optimization.bench import aggregate_sweep_repeats
from accel_pipeline.repair import Failure, failure_from_gate


@pytest.fixture()
def dataset(tmp_path, monkeypatch):
    return Dataset(make_engine(tmp_path, monkeypatch))


def test_analysis_finds_roles_joins_params_and_spreads(dataset):
    shape = data_mod.analyze_query(QUERY, dataset.schema, "sqlite")
    assert shape.roles[("orders", "region")] == {"G"}
    assert shape.roles[("events", "order_id")] == {"J"}
    assert len(shape.joins) == 1
    ops = {(p.table, p.column, p.op) for p in shape.params}
    assert ops == {("orders", "kind", "="), ("orders", "placed", "<"), ("events", "happened", ">"), ("events", "amount", ">")}
    assert [(a[1], b[1]) for a, b, _ in shape.spread_pairs] == [("placed", "happened")]


def _markdown(dataset):
    return build_data_shape_markdown(QUERY, dataset.runner, dataset.schema, "fake", 10, 30)


def test_spread_values_are_the_real_differences(dataset):
    facts = data_mod.compute(QUERY, dataset.runner, dataset.schema)
    (_, _, _, res, sampled), = facts["spreads"]
    n, lo, qs, hi = res
    assert n == 400 and not sampled
    expected = [i % 100 + 5 + i % 7 - (1 + i % 100) for i in range(400)]
    assert (int(float(lo)), int(float(hi))) == (-max(expected), -min(expected))


def test_large_tables_are_sampled(dataset, monkeypatch):
    monkeypatch.setattr(data_mod, "SAMPLE_ROWS", 50)
    facts = data_mod.compute(QUERY, dataset.runner, dataset.schema)
    assert facts["spreads"][0][4] is True
    assert facts["rows"] == {"events": 400, "orders": 100}


def test_date_difference_is_built_for_the_engine_to_transpile():
    import sqlglot
    from sqlglot import exp

    tree = sqlglot.parse_one(f"SELECT {data_mod.diff_sql('a', 'b', 'date')}")
    assert tree.find(exp.DateDiff) is not None
    assert "CAST" in data_mod.diff_sql("a", "b", "number")


def test_percentiles_are_window_functions_not_engine_functions():
    sql = data_mod._quantile_cells([0.5]) + data_mod._ranked("SELECT 1 AS v")
    assert "OVER" in sql and "quantile" not in sql.lower() and "percentile" not in sql.lower()


SCHEMA = """CREATE TABLE orders(id INTEGER, kind TEXT, region TEXT, placed INTEGER);
CREATE TABLE events(order_id INTEGER, happened INTEGER, amount NUMERIC);
"""
JOINS = [(("orders", "o_custkey"), ("customer", "c_custkey")), (("lineitem", "l_orderkey"), ("orders", "o_orderkey"))]
ROWS = {"lineitem": 6000, "orders": 1500, "customer": 150}
INTEGERS = {"lineitem": ["l_orderkey", "l_linenumber"], "orders": ["o_orderkey", "o_custkey"], "customer": ["c_custkey"]}


def selects(root_rows=600, rows=ROWS):
    return cut_selects(["customer", "orders", "lineitem"], JOINS, rows, INTEGERS, root_rows)


def test_the_largest_table_is_sampled_by_its_join_key_and_the_others_follow_the_joins():
    out = selects()
    assert out["lineitem"] == "SELECT * FROM lineitem WHERE l_orderkey % 11 = 0"  # every 11th key: 10 rounded up to a prime
    assert out["orders"] == f"SELECT * FROM orders WHERE o_orderkey IN (SELECT l_orderkey FROM ({out['lineitem']}) AS kept)"
    assert out["customer"] == f"SELECT * FROM customer WHERE c_custkey IN (SELECT o_custkey FROM ({out['orders']}) AS kept)"


def test_a_table_within_the_row_target_is_kept_whole():
    assert selects(root_rows=6000)["lineitem"] == "SELECT * FROM lineitem"


def test_tables_no_join_reaches_are_cut_on_their_own():
    out = cut_selects(["orders", "nation"], [], {"orders": 1000, "nation": 5000}, {"orders": ["o_orderkey"], "nation": ["n_key"]}, 500)
    assert out["nation"] == "SELECT * FROM nation ORDER BY n_key LIMIT 500"
    assert out["orders"] == "SELECT * FROM orders ORDER BY o_orderkey LIMIT 500"


def test_the_stride_is_a_prime_so_it_cannot_line_up_with_blocks_of_keys():
    strides = {rows: data_mod._sample("t", rows, 100, ["k"], ["k"], []).rsplit("% ", 1)[1].split(" ")[0] for rows in (1000, 6400, 99_999)}
    assert strides == {1000: "11", 6400: "67", 99_999: "1009"}


def test_a_sample_with_no_rows_is_an_error(tmp_path):
    with pytest.raises(ValueError, match="no rows"):
        data_mod._write_cut(type("R", (), {"rows": lambda self, sql, seconds: []})(), {"t": "SELECT 1"}, tmp_path / "cut")


def test_a_table_no_key_links_keeps_its_first_rows():
    # SEC-EDGAR's pre, for a query that reads num and sub: its integers are a few report and line numbers, and a
    # modulo of them by the stride kept no row at all
    out = cut_selects(["num", "pre", "notes"], [], {"num": 50, "pre": 9_600_799, "notes": 10},
                      {"num": ["ddate"], "pre": ["report", "line"], "notes": []}, 5)
    assert out["pre"] == "SELECT * FROM pre ORDER BY report, line LIMIT 5"
    assert out["notes"] == "SELECT * FROM notes LIMIT 5" and "%" not in out["pre"]


def test_a_text_join_key_is_sampled_by_rank_not_by_another_integer_column():
    # SEC-EDGAR: num joins sub on adsh (text); num's integer columns are a handful of report dates and quarter counts
    joins = [(("num", "adsh"), ("sub", "adsh"))]
    out = cut_selects(["num", "sub"], joins, {"num": 1000, "sub": 50}, {"num": ["ddate", "qtrs"], "sub": ["cik"]}, 100)
    assert "ROW_NUMBER() OVER (ORDER BY adsh)" in out["num"] and "key_rank % 11 = 0" in out["num"] and "ddate" not in out["num"]
    assert out["sub"].startswith("SELECT * FROM sub WHERE adsh IN (SELECT adsh FROM (")


def test_key_columns_with_the_same_name_link_tables_the_query_does_not_join():
    schema = {"lineitem": {"l_orderkey": "BIGINT", "l_partkey": "BIGINT", "l_comment": "VARCHAR"},
              "orders": {"o_orderkey": "BIGINT", "o_totalprice": "DECIMAL"},
              "partsupp": {"ps_partkey": "BIGINT"}, "part": {"p_partkey": "BIGINT"},
              "visits": {"customer_id": "INTEGER", "id": "INTEGER"}, "customers": {"customer_id": "INTEGER", "id": "INTEGER", "year": "INTEGER"}}
    assert key_links(schema) == [
        (("lineitem", "l_orderkey"), ("orders", "o_orderkey")),
        (("lineitem", "l_partkey"), ("partsupp", "ps_partkey")), (("lineitem", "l_partkey"), ("part", "p_partkey")),
        (("partsupp", "ps_partkey"), ("part", "p_partkey")),
        (("visits", "customer_id"), ("customers", "customer_id"))]  # not the bare `id`s, not text or decimals


def test_scale_is_full_rows_per_subset_row_and_one_when_unknown():
    cuts = CutDatasets(None, None, {"source_rows": {"orders": 1000, "events": 5000}, "subset_rows": {"orders": 10, "events": 100}})
    assert cuts.scale(["orders"]) == 100 and cuts.scale(["ORDERS", "events"]) == pytest.approx(6000 / 110)
    assert cuts.scale(["nation"]) == 1.0 and cuts.scale() == pytest.approx(6000 / 110)
    assert CutDatasets(None, None, {"source_rows": {}, "subset_rows": {}}).scale(["orders"]) == 1.0


@pytest.fixture()
def cut_env(tmp_path, monkeypatch):
    """The fake SQLite engine (host.sh sql) holding 100 orders and 400 events, and a workload over them."""
    ctx = make_engine(tmp_path, monkeypatch)
    repo = ctx.pipeline_config.repo_root
    (repo / "workloads" / "acme" / "schema.sql").write_text(SCHEMA)
    (repo / "workloads" / "acme" / "inst.sql").write_text(QUERY.replace("$1", "'k1'").replace("$2", "50").replace("$3", "2"))
    data = tmp_path / "data"
    data.mkdir()
    (data / "orders.tbl").write_text("1|k|r|1|\n")
    (data / "events.tbl").write_text("1|1|1|\n")
    monkeypatch.setattr(data_mod, "SUBSET_ROOT_ROWS", 120)
    monkeypatch.setattr(data_mod, "TINY_ROOT_ROWS", 30)
    workload = {**ctx.workload, "workload_kind": "aux_state", "dataset_dir": str(data),
                "reference_sql_path": str(repo / "workloads" / "acme" / "inst.sql")}
    return ctx.pipeline_config, workload, data


def rows_of(directory, table):
    return [line.rstrip("|").split("|") for line in (directory / f"{table}.tbl").read_text().splitlines()]


def test_the_cuts_are_join_closed_and_the_tiny_one_is_smaller(cut_env):
    cfg, workload, _ = cut_env
    cuts = ensure_cut_datasets(cfg, workload, 1)
    events, orders = rows_of(cuts.subset, "events"), rows_of(cuts.subset, "orders")
    assert 0 < len(events) < 400 and len(orders) < 100
    assert {e[0] for e in events} == {o[0] for o in orders}  # every kept event has its order, and no order is kept without events
    assert len(rows_of(cuts.tiny, "events")) < len(events)
    assert len(rows_of(cuts.subset, "events")) == cuts.manifest["subset_rows"]["events"]
    assert cuts.manifest["source_rows"] == {"events": 400, "orders": 100} and cuts.scale(["events"]) > 1


def test_the_cut_keeps_the_rows_the_query_answers_from_at_its_binding(cut_env):
    cfg, workload, _ = cut_env
    reference = Path(workload["reference_sql_path"])
    reference.write_text("SELECT e.amount FROM events e JOIN orders o ON e.order_id = o.id WHERE o.id IN (98, 99) ORDER BY e.amount DESC")
    even = ensure_cut_datasets(cfg, {**workload, "reference_sql_path": ""}, 1)  # no bound query: the even sample alone
    assert not {"98", "99"} & {e[0] for e in rows_of(even.subset, "events")}
    cuts = ensure_cut_datasets(cfg, workload, 1)
    assert {"98", "99"} <= {e[0] for e in rows_of(cuts.subset, "events")}
    assert {"98", "99"} <= {o[0] for o in rows_of(cuts.subset, "orders")}  # the parents follow
    assert sum(e[0] == "98" for e in rows_of(cuts.subset, "events")) == 4  # all children of a kept key
    assert len(rows_of(cuts.subset, "events")) < 400


def test_a_top_k_binding_keeps_its_top_rows_first_within_the_bound(cut_env):
    cfg, workload, _ = cut_env
    runner = data_mod.HostSql(data_mod.DataToolContext(cfg, workload, 1, cfg.repo_root))
    keys = data_mod.AnswerKeys(runner, "SELECT order_id FROM events GROUP BY order_id ORDER BY order_id DESC LIMIT 50",
                               {"events": {"order_id": "INTEGER"}}, {"events": 400})
    assert keys("events", "order_id", ["order_id"], 40) == ["100", "99"]  # 40 rows / 4 = 10 rows = 2 of the 4-row keys


def test_an_order_by_an_aggregate_alias_is_dropped_and_one_by_a_column_alias_is_kept(cut_env):
    cfg, workload, _ = cut_env
    runner = data_mod.HostSql(data_mod.DataToolContext(cfg, workload, 1, cfg.repo_root))
    schema, rows = {"events": {"order_id": "INTEGER", "happened": "INTEGER"}}, {"events": 400}
    by_total = data_mod.AnswerKeys(runner, "SELECT order_id, SUM(happened) AS total FROM events GROUP BY order_id ORDER BY total DESC", schema, rows)
    by_time = data_mod.AnswerKeys(runner, "SELECT order_id, happened AS at FROM events ORDER BY at DESC, order_id", schema, rows)
    assert len(by_total("events", "order_id", ["order_id"], 40)) == 2  # still runs: unordered
    assert "ORDER BY" not in by_total._key_select(by_total.scopes[0].expression, "events", "order_id", 5)
    assert "ORDER BY events.happened DESC" in by_time._key_select(by_time.scopes[0].expression, "events", "order_id", 5).replace('"', "")


def test_the_children_of_a_kept_key_are_all_kept(cut_env):
    cfg, workload, _ = cut_env
    cuts = ensure_cut_datasets(cfg, workload, 1)
    kept = {e[0] for e in rows_of(cuts.subset, "events")}
    all_events = [(1 + i % 100) for i in range(400)]  # fake_engine.make_dataset
    assert len(rows_of(cuts.subset, "events")) == sum(1 for o in all_events if str(o) in kept)


def test_every_table_of_the_dataset_is_cut_even_when_the_query_does_not_read_it(cut_env, tmp_path):
    cfg, workload, data = cut_env
    cfg.repo_root.joinpath("workloads", "acme", "q1.sql").write_text("SELECT COUNT(*) FROM events WHERE amount > $1")  # no orders
    (data / "orders.tbl").write_text("1|k|r|1|\n")
    cuts = ensure_cut_datasets(cfg, workload, 1)
    assert {p.name for p in cuts.subset.glob("*.tbl")} == {"events.tbl", "orders.tbl"} and rows_of(cuts.subset, "events")


def test_the_cuts_are_made_once_per_dataset(cut_env, monkeypatch):
    cfg, workload, data = cut_env
    first = ensure_cut_datasets(cfg, workload, 1)
    with monkeypatch.context() as m:
        m.setattr(data_mod, "_cut", lambda *a: pytest.fail("cut again"))
        assert ensure_cut_datasets(cfg, workload, 1).subset == first.subset
    (data / "orders.tbl").write_text("1|k|r|1|\n2|k|r|2|\n")  # another dataset: another key
    assert ensure_cut_datasets(cfg, workload, 1).subset != first.subset


def test_an_engine_that_cannot_answer_leaves_no_half_made_cut(cut_env, monkeypatch, capsys):
    cfg, workload, _ = cut_env
    with monkeypatch.context() as m:
        m.setenv("FAKE_ENGINE_DB", "/nonexistent.sqlite")
        assert ensure_cut_datasets(cfg, workload, 1) is None
    assert "test databases not made" in capsys.readouterr().err
    assert not list(cfg.build_dir.parent.glob("build.subset/*/.done"))
    assert ensure_cut_datasets(cfg, workload, 1) is not None  # a later run makes them


def test_no_dataset_directory_means_no_cuts(cut_env, tmp_path):
    cfg, workload, _ = cut_env
    assert ensure_cut_datasets(cfg, {**workload, "dataset_dir": str(tmp_path / "missing")}, 1) is None


def test_ready_made_databases_from_the_environment_win(cut_env, monkeypatch):
    cfg, workload, _ = cut_env
    monkeypatch.setenv("ACCEL_SUBSET_DATASET_DIR", "/given")
    cuts = ensure_cut_datasets(cfg, workload, 1)
    assert str(cuts.subset) == str(cuts.tiny) == "/given" and cuts.scale(["events"]) == 1.0


def test_the_description_names_the_sizes():
    manifest = {"source_rows": {"orders": 100}, "subset_rows": {"orders": 10}, "tiny_rows": {"orders": 2}}
    assert data_mod.describe(manifest) == "subset 10 rows (of 100), tiny 2 rows; per table orders 10/2"


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
    return data_mod.validate_static(suite, **{**args, **kw})


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
    assert data_mod.validate_report(report(), write_suite(tmp_path), ["t"], True) == []


def test_a_table_no_transaction_changes_the_answer_of_is_reported(tmp_path):
    silent = report(txn_effects=[{"txn": n, "changes_answers_of": 0} for n in TXNS])
    found = data_mod.validate_report(silent, write_suite(tmp_path), ["t"], True)
    assert any("no transaction on t changes the answer" in p for p in found)


def test_edge_claims_are_checked_against_the_run(tmp_path):
    suite = write_suite(tmp_path)
    found = data_mod.validate_report(report(tuple_rows=[3, 2, 1, 7, 1]), suite, ["t"], True)
    assert any("empty_result: tuple 4 returns 7 rows" in p for p in found)
    found = data_mod.validate_report(report(), write_suite(tmp_path / "x", params=PARAMS[:4] + ["'A', DATE '1995-03-15'"]), ["t"], True)
    assert any("null_param: tuple 5 contains no NULL" in p for p in found)


def test_errors_of_the_run_are_problems(tmp_path):
    assert data_mod.validate_report(report(errors=["txn 2: boom"]), write_suite(tmp_path), ["t"], True) == ["txn 2: boom"]


def test_draws_are_reproducible_from_the_seed(tmp_path):
    suite = write_suite(tmp_path)
    domain = yaml.safe_load((suite / "domain.yaml").read_text())
    first = data_mod.draw_tuples(domain, 20, 42)
    assert first == data_mod.draw_tuples(domain, 20, 42) != data_mod.draw_tuples(domain, 20, 43)
    assert all(len(data_mod.split_tuple(t)) == 2 for t in first) and len(set(first)) > 10
    mix = yaml.safe_load((suite / "write_mix.yaml").read_text())
    freqs = data_mod.draw_write_frequencies(mix, 3, 42)
    assert freqs == data_mod.draw_write_frequencies(mix, 3, 42)
    assert freqs[0] == 0 and len(freqs) == 4 and all(1 <= f <= 50 for f in freqs[1:]) and freqs == sorted(set(freqs))
    assert len({tuple(data_mod.draw_write_frequencies(mix, 3, s)) for s in range(30)}) > 5


def test_derived_parameters_follow_their_source():
    domain = {"params": [{"name": "start", "type": "date", "min": "1993-01-01", "max": "1997-12-31"},
                         {"name": "end", "type": "derived", "from": "start", "add_months": 3},
                         {"name": "pattern", "type": "pattern", "template": "%{}%", "words": ["red", "green"]},
                         {"name": "q", "type": "decimal", "min": 1, "max": 2, "scale": 2},
                         {"name": "n", "type": "int", "min": 5, "max": 6}]}
    for tuple_ in data_mod.draw_tuples(domain, 30, 1):
        start, end, pattern, q, n = data_mod.split_tuple(tuple_)
        month = int(start[-6:-4]) - 1
        assert int(end[-6:-4]) - 1 == (month + 3) % 12
        assert pattern in ("'%red%'", "'%green%'") and 1 <= float(q) <= 2 and n in ("5", "6")


def test_a_run_environment_is_its_seed_and_tier(tmp_path):
    suite = write_suite(tmp_path)
    cfg = SuiteConfig(acceptance_random_tuples=7, differential_every=4)
    full = data_mod.run_environment(suite, tier="full", cfg=cfg, seed=5, params_file=tmp_path / "full.sql")
    again = data_mod.run_environment(suite, tier="full", cfg=cfg, seed=5, params_file=tmp_path / "again.sql")
    assert (tmp_path / "full.sql").read_text() == (tmp_path / "again.sql").read_text()
    assert len((tmp_path / "full.sql").read_text().splitlines()) == 7
    assert full["ACCEL_SUITE_SEED"] == "5" and full["ACCEL_SUITE_EVERY"] == "4" and "ACCEL_SUITE_TXNS" not in full
    quick = data_mod.run_environment(suite, tier="quick", cfg=cfg, seed=5, params_file=tmp_path / "quick.sql")
    assert len((tmp_path / "quick.sql").read_text().splitlines()) == 7 and quick["ACCEL_SUITE_EVERY"] == "4"  # as many as the full tier
    assert "ACCEL_SUITE_TXNS" not in quick  # every transaction of the suite
    iterate = data_mod.run_environment(suite, tier="iterate", cfg=SuiteConfig(acceptance_random_tuples=7, iterate_random_tuples=3, iterate_every=10),
                                 seed=5, params_file=tmp_path / "iterate.sql")
    assert len((tmp_path / "iterate.sql").read_text().splitlines()) == 3 and iterate["ACCEL_SUITE_EVERY"] == "10"
    assert "ACCEL_SUITE_TXNS" not in iterate
    fresh = data_mod.run_environment(suite, tier="full", cfg=cfg, seed=None, params_file=tmp_path / "fresh.sql")
    assert fresh["ACCEL_SUITE_SEED"] != "0" and again == {**full, "ACCEL_SUITE_PARAMS_FILE": again["ACCEL_SUITE_PARAMS_FILE"]}


def test_transactions_are_profiled_by_declared_kind_and_written_tables(tmp_path):
    suite = write_suite(tmp_path)
    profiles = data_mod.txn_profiles(suite)
    assert profiles["001.sql"] == ("point_insert", ("t",)) and list(profiles) == list(data_mod.txn_files(suite))


def test_a_frozen_suite_cannot_be_edited_and_a_change_request_is_the_way(tmp_path):
    suite, art = write_suite(tmp_path), tmp_path / "art"
    art.mkdir()
    assert "never frozen" in data_mod.frozen_problem(art, suite)
    record = data_mod.freeze(art, suite, query=3)
    assert data_mod.frozen_problem(art, suite) == "" and record["hash"] == data_mod.content_hash(suite)
    (suite / "txns" / "001.sql").write_text("INSERT INTO t VALUES (2, 'B', DATE '1995-01-01', 1);")
    assert "was edited" in data_mod.frozen_problem(art, suite) and "change request" in data_mod.frozen_problem(art, suite)
    (suite / "txns" / "001.sql").write_text(TXNS["001.sql"])
    assert data_mod.frozen_problem(art, suite) == ""
    (suite / "extra.sql").write_text("-- new file\n")
    assert data_mod.frozen_problem(art, suite)
    (art / data_mod.CHANGE_REQUEST).write_text("001.sql inserts a duplicate key\n")
    assert data_mod.pending_change_request(art) == "001.sql inserts a duplicate key"
    data_mod.archive_change_request(art, "the tester kept the suite as it was")
    assert data_mod.pending_change_request(art) == ""
    assert "kept the suite" in (art / "test_suite_change_requests" / "01.md").read_text()
    data_mod.archive_change_request(art, "filed already by the tester")   # nothing pending: not an error, nothing archived
    assert len(list((art / "test_suite_change_requests").glob("*.md"))) == 1


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
        from accel_pipeline import stages as stage

        self.stage, self.tmp, self.messages, self.roles = stage, tmp_path, [], []
        self.config = PipelineConfig(repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
                                     sandbox="none", write_policy="off",
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
        found = data_mod.validate_static(self.directory, query_sql=QUERY_SQL, param_names=["seg", "d"], dependent_tables=["t"], aux=True)
        if not found:
            return [GateResult(names[0], "pass", {})], None
        result = GateResult(names[0], "fail", {"log_tail": "\n".join(found), "returncode": 1})
        return [result], result

    def run(self, **kw):
        return self.stage.run_test_suite(self.config, None, "toy", 3, **kw)


def test_a_suite_that_fails_validation_goes_back_to_the_tester_then_is_frozen(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, [dict(manifest=edited(MANIFEST, ["scale_scenarios"], [])), {}])
    world.run()
    assert len(world.messages) == 2 and "scale_scenarios" in world.messages[1]
    assert world.roles == ["tester", "tester"]
    assert data_mod.frozen_problem(world.art, world.directory) == ""


def test_a_change_request_reruns_the_tester_and_archives_the_outcome(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, [{}, dict(txns={**TXNS, "001.sql": "INSERT INTO t VALUES (7, 'A', DATE '1995-01-01', 1);"})])
    world.run()
    (world.art / data_mod.CHANGE_REQUEST).write_text("001.sql: key 1 already exists in t")
    world.stage.revise_test_suite(world.config, None, "toy", 3)
    assert "key 1 already exists" in world.messages[-1] and "change request" in world.messages[-1]
    assert data_mod.pending_change_request(world.art) == "" and data_mod.frozen_problem(world.art, world.directory) == ""
    assert "changed the suite" in (world.art / "test_suite_change_requests" / "01.md").read_text()


def test_a_repair_turn_of_another_stage_hands_a_pending_change_request_to_the_tester(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, [{}])
    world.run()
    (world.art / data_mod.CHANGE_REQUEST).write_text("003.sql deletes a row that is gone")
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
        data_mod.freeze(art, suite, query=3)
    settings = {"dir": str(suite), "query_sql": str(tmp_path / "q.sql"), "function": "accel_toy_q3_aux", "arity": 2,
                "param_names": ["seg", "d"], "aux": True, "schema_sql": "", "config": asdict(SuiteConfig())}
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
    assert summary["ran"] is True
    assert json.loads(suite_gate(tmp_path, "summary", str(tmp_path / "missing.json")).stdout)["ran"] is False


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
    from accel_pipeline.optimization import bench as freqs

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


def test_the_format_docs_worked_example_passes_the_static_checks(tmp_path):
    """The example suite in pipeline/prompts/shared/test_suite.md is what a tester copies: it must be valid."""
    import re

    doc = (Path(__file__).resolve().parents[2] / "prompts" / "shared" / "test_suite.md").read_text(encoding="utf-8")
    example = doc.split("## Worked example")[1].split("## How to validate")[0]
    blocks = re.findall(r"```(\w+)\n(.*?)```", example, re.S)
    sql = [b for lang, b in blocks if lang == "sql"]
    suite = tmp_path / "workload"
    (suite / "txns").mkdir(parents=True)
    (suite / "params.sql").write_text(sql[0], encoding="utf-8")
    txns = {"001": sql[1], "002": sql[2], "003": "INSERT INTO events SELECT * FROM events LIMIT 1;",
            "004": "DELETE FROM events WHERE event_id = 1;", "005": "UPDATE events SET account_id = account_id + 1;",
            "006": "INSERT INTO events SELECT * FROM events LIMIT 5;"}
    for name, text in txns.items():
        (suite / "txns" / f"{name}.sql").write_text(text, encoding="utf-8")
    (suite / "suite.yaml").write_text([b for lang, b in blocks if lang == "yaml"][-1], encoding="utf-8")
    (suite / "domain.yaml").write_text("version: 1\nparams:\n  - {name: a, type: date, min: 1992-01-02, max: 1998-12-01}\n"
                                       "  - {name: b, type: date, min: 1992-01-02, max: 1998-12-01}\n  - {name: c, type: choice, values: [\"'F'\"]}\n",
                                       encoding="utf-8")
    (suite / "write_mix.yaml").write_text("version: 1\nfrequency_pct: {min: 1, max: 50, samples: 3, distribution: log}\n"
                                          "kinds: {point_update: 5, point_insert: 2, point_delete: 2, bulk_insert: 1}\n", encoding="utf-8")
    query = "SELECT kind, count(*) FROM events WHERE ts >= $1 AND ts < $2 AND status = $3 GROUP BY 1 ORDER BY 1"
    assert data_mod.validate_static(suite, query_sql=query, param_names=["a", "b", "c"], dependent_tables=["events"], aux=True) == []


def test_fixed_tuples_must_follow_a_derived_parameter():
    from accel_pipeline.data import _check_derived

    domain = {"params": [{"name": "start", "type": "date"}, {"name": "end", "type": "derived", "from": "start", "add_months": 1}]}
    good = "DATE '1995-01-31', DATE '1995-02-28'"
    assert _check_derived(domain, [good, "NULL, NULL"]) == []
    bad = _check_derived(domain, ["DATE '1995-01-01', DATE '1995-01-01'"])
    assert len(bad) == 1 and "DATE '1995-02-01'" in bad[0]


def test_a_declared_foreign_key_links_a_child_to_the_rows_of_its_parent():
    # SEC-EDGAR's pre references sub by a text key: cut on its own, its first rows named filings absent from the sampled
    # sub and the subset failed to load (foreign key violation)
    schema_sql = """CREATE TABLE sub (adsh VARCHAR PRIMARY KEY, cik INTEGER);
    CREATE TABLE num (adsh VARCHAR, value DOUBLE, FOREIGN KEY (adsh) REFERENCES sub(adsh));
    CREATE TABLE pre (adsh VARCHAR REFERENCES sub (adsh), report INTEGER, line INTEGER);"""
    links = foreign_key_links(schema_sql)
    assert links == [(("num", "adsh"), ("sub", "adsh")), (("pre", "adsh"), ("sub", "adsh"))]
    out = cut_selects(["num", "sub", "pre"], links, {"num": 1000, "sub": 50, "pre": 900},
                      {"num": [], "sub": ["cik"], "pre": ["report", "line"]}, 100)
    assert out["sub"].startswith("SELECT * FROM sub WHERE adsh IN (SELECT adsh FROM (")
    assert out["pre"].startswith("SELECT * FROM pre WHERE adsh IN (SELECT adsh FROM (")


def test_parse_csv_keeps_null_and_the_empty_string_apart():
    assert data_mod.parse_csv('a,b\n,""\n"x,""y""",\n') == [["a", "b"], [None, ""], ['x,"y"', None]]
    assert data_mod.parse_csv('a\n"line\nbreak"\n') == [["a"], ["line\nbreak"]]


def test_rows_tell_null_from_empty_and_query_shows_both_empty(dataset):
    runner = dataset.runner
    sql = "SELECT NULL AS n, '' AS e, 'x\"y' AS q"
    assert runner.rows(sql, 10) == [[None, "", 'x"y']]
    assert runner.query(sql, 10, 5)[0][1] == ["", "", 'x"y']


def test_the_host_sql_keeps_stderr_out_of_the_csv_and_reports_it_on_failure(tmp_path, monkeypatch):
    ctx = make_engine(tmp_path, monkeypatch)
    host = ctx.pipeline_config.repo_root / "backends" / "fake" / "host.sh"
    text = host.read_text().replace('sql) exec', 'sql) echo "warning: engine chatter" >&2; exec')
    host.write_text(text)
    runner = data_mod.HostSql(ctx)
    assert runner.rows("SELECT 1 AS a", 10) == [["1"]]
    with pytest.raises(RuntimeError, match="warning: engine chatter"):
        runner.rows("SELECT * FROM nowhere", 10)


def test_a_cut_writes_null_bare_and_the_empty_string_quoted(tmp_path):
    class Rows:
        def rows(self, select, seconds):
            return [["1", None, "", 'a"b']]

    data_mod._write_cut(Rows(), {"t": "SELECT 1"}, tmp_path / "cut")
    assert (tmp_path / "cut" / "t.tbl").read_text() == '1||""|"a""b"|\n'
