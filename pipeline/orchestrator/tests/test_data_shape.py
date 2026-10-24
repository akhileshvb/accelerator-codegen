from __future__ import annotations

import duckdb
import pytest

from accel_pipeline import data_shape_stats
from accel_pipeline.data_shape import build_data_shape_markdown

QUERY = """
SELECT o.region, SUM(e.amount) AS total
FROM orders o, events e
WHERE o.kind = $1 AND e.order_id = o.id AND o.placed < $2 AND e.happened > $2 AND e.amount > $3
GROUP BY o.region ORDER BY total DESC
"""


@pytest.fixture()
def dataset(tmp_path):
    path = tmp_path / "acme.duckdb"
    con = duckdb.connect(str(path))
    con.execute("CREATE TABLE orders(id BIGINT, kind VARCHAR, region VARCHAR, placed DATE)")
    con.execute("CREATE TABLE events(order_id BIGINT, happened DATE, amount DECIMAL(10,2))")
    con.execute(
        "INSERT INTO orders SELECT i, 'k' || (i % 3), 'r' || (i % 2), DATE '2020-01-01' + CAST(i AS INTEGER) "
        "FROM range(1, 101) t(i)"
    )
    con.execute(
        "INSERT INTO events SELECT 1 + (i % 100), DATE '2020-01-01' + CAST(i % 100 AS INTEGER) + 5 + CAST(i % 7 AS INTEGER), "
        "i % 10 FROM range(0, 400) t(i)"
    )
    con.close()
    return path


def _schema(path):
    con = duckdb.connect(str(path), read_only=True)
    schema: dict = {}
    for t, c, d in con.execute("SELECT table_name, column_name, data_type FROM information_schema.columns").fetchall():
        schema.setdefault(t, {})[c] = d
    con.close()
    return schema


def test_analysis_finds_roles_joins_params_and_spreads(dataset):
    shape = data_shape_stats.analyze_query(QUERY, _schema(dataset))
    assert shape.roles[("orders", "region")] == {"G"}
    assert shape.roles[("events", "order_id")] == {"J"}
    assert len(shape.joins) == 1
    ops = {(p.table, p.column, p.op) for p in shape.params}
    assert ops == {("orders", "kind", "="), ("orders", "placed", "<"), ("events", "happened", ">"), ("events", "amount", ">")}
    assert [(a[1], b[1]) for a, b, _ in shape.spread_pairs] == [("placed", "happened")]


def test_markdown_has_each_fact_family(dataset, tmp_path):
    repo = tmp_path / "repo"
    (repo / "workloads" / "acme").mkdir(parents=True)
    (repo / "workloads" / "acme" / "q1.sql").write_text(QUERY)
    text = build_data_shape_markdown({"benchmark_name": "acme"}, dataset, repo=repo, query=1)
    assert "### Join keys" in text and "events.order_id -> orders.id" in text
    assert "3 distinct values" in text  # equality predicate: value frequencies
    assert "rows qualifying with the parameter at column min=" in text
    assert "orders.placed - events.happened" in text and "joined rows" in text
    assert "scale" in text and "SF3000" in text


def test_spread_values_are_the_real_differences(dataset):
    schema = _schema(dataset)
    con = data_shape_stats.scratch_connection(str(dataset), read_only=True)
    try:
        shape = data_shape_stats.analyze_query(QUERY, schema)
        probe = data_shape_stats.Probe(con)
        a, b, _ = shape.spread_pairs[0]
        res, sampled = data_shape_stats.spread(probe, a, b, shape, {"orders": 100, "events": 400}, schema)
        expected = con.execute(
            "SELECT count(*), min(d), max(d) FROM (SELECT date_diff('day', e.happened, o.placed) d "
            "FROM orders o JOIN events e ON e.order_id = o.id)"
        ).fetchone()
        assert (res[0], res[1], res[3]) == expected and not sampled
    finally:
        con.close()


def test_large_tables_are_sampled(dataset, monkeypatch):
    monkeypatch.setattr(data_shape_stats, "SAMPLE_ROWS", 50)
    facts = data_shape_stats.compute(QUERY, str(dataset))
    assert all(sampled for *_, sampled in facts["spreads"])


def test_unparameterised_or_missing_database_falls_back_to_counts(tmp_path):
    text = build_data_shape_markdown({"benchmark_name": "acme", "dependent_tables": [{"name": "orders"}]}, tmp_path / "none.duckdb")
    assert "Live DB unavailable" in text
