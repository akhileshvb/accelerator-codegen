"""The run's test databases: join-closed cuts of the engine's dataset, made once through its host.sh."""

from __future__ import annotations

import pytest
from fake_engine import QUERY, make_engine

from accel_pipeline import test_db
from accel_pipeline.test_db import CutDatasets, cut_selects, ensure_cut_datasets, key_links

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
    strides = {rows: test_db._sample("t", rows, 100, ["k"], ["k"]).rsplit("% ", 1)[1].split(" ")[0] for rows in (1000, 6400, 99_999)}
    assert strides == {1000: "11", 6400: "67", 99_999: "1009"}


def test_a_sample_with_no_rows_is_an_error(tmp_path):
    with pytest.raises(ValueError, match="no rows"):
        test_db._write_cut(type("R", (), {"rows": lambda self, sql, seconds: []})(), {"t": "SELECT 1"}, tmp_path / "cut")


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
    monkeypatch.setattr(test_db, "SUBSET_ROOT_ROWS", 120)
    monkeypatch.setattr(test_db, "TINY_ROOT_ROWS", 30)
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
        m.setattr(test_db, "_cut", lambda *a: pytest.fail("cut again"))
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
    assert test_db.describe(manifest) == "subset 10 rows (of 100), tiny 2 rows; per table orders 10/2"
