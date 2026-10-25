from __future__ import annotations

import pytest
from fake_engine import QUERY, make_engine

from accel_pipeline import data_shape_stats
from accel_pipeline.data_shape import build_data_shape_markdown
from accel_pipeline.tools.data_tools import Dataset


@pytest.fixture()
def dataset(tmp_path, monkeypatch):
    return Dataset(make_engine(tmp_path, monkeypatch))


def test_analysis_finds_roles_joins_params_and_spreads(dataset):
    shape = data_shape_stats.analyze_query(QUERY, dataset.schema, "sqlite")
    assert shape.roles[("orders", "region")] == {"G"}
    assert shape.roles[("events", "order_id")] == {"J"}
    assert len(shape.joins) == 1
    ops = {(p.table, p.column, p.op) for p in shape.params}
    assert ops == {("orders", "kind", "="), ("orders", "placed", "<"), ("events", "happened", ">"), ("events", "amount", ">")}
    assert [(a[1], b[1]) for a, b, _ in shape.spread_pairs] == [("placed", "happened")]


def _markdown(dataset):
    return build_data_shape_markdown(QUERY, dataset.runner, dataset.schema, "fake", 10, 30)


def test_spread_values_are_the_real_differences(dataset):
    facts = data_shape_stats.compute(QUERY, dataset.runner, dataset.schema)
    (_, _, _, res, sampled), = facts["spreads"]
    n, lo, qs, hi = res
    assert n == 400 and not sampled
    expected = [i % 100 + 5 + i % 7 - (1 + i % 100) for i in range(400)]
    assert (int(float(lo)), int(float(hi))) == (-max(expected), -min(expected))


def test_large_tables_are_sampled(dataset, monkeypatch):
    monkeypatch.setattr(data_shape_stats, "SAMPLE_ROWS", 50)
    facts = data_shape_stats.compute(QUERY, dataset.runner, dataset.schema)
    assert facts["spreads"][0][4] is True
    assert facts["rows"] == {"events": 400, "orders": 100}


def test_date_difference_is_built_for_the_engine_to_transpile():
    import sqlglot
    from sqlglot import exp

    tree = sqlglot.parse_one(f"SELECT {data_shape_stats.diff_sql('a', 'b', 'date')}")
    assert tree.find(exp.DateDiff) is not None
    assert "CAST" in data_shape_stats.diff_sql("a", "b", "number")


def test_percentiles_are_window_functions_not_engine_functions():
    sql = data_shape_stats._quantile_cells([0.5]) + data_shape_stats._ranked("SELECT 1 AS v")
    assert "OVER" in sql and "quantile" not in sql.lower() and "percentile" not in sql.lower()
