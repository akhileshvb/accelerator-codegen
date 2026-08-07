"""Shared PostgreSQL differential helpers for cold and incremental checks."""

from __future__ import annotations

import os
from typing import Tuple

import psycopg2

from shared_query_catalog import SharedQueryEntry


def run_query(cur, query, args=()):
    # psycopg2 treats bare '%' as parameter placeholders; literal LIKE patterns
    # need escaping when the query has no bound parameters.
    if not args and "%" in query:
        query = query.replace("%", "%%")
    cur.execute(query, args)
    return cur.fetchall()


def normalized(rows):
    return [tuple(None if v is None else str(v) for v in row) for row in rows]


def connect(db):
    host = os.environ.get("PG_HOST", "127.0.0.1")
    port = int(os.environ.get("PG_PORT", "5434"))
    return psycopg2.connect(host=host, port=port, user="postgres", dbname=db)


def approx_equal(expected: str, actual: str) -> bool:
    if expected == actual:
        return True
    if expected == "NULL" or actual == "NULL":
        return expected == actual
    try:
        expected_value = float(expected)
        actual_value = float(actual)
        scale = max(1.0, abs(expected_value), abs(actual_value))
        return abs(expected_value - actual_value) <= 1e-6 * scale
    except ValueError:
        return False


def rows_equal(base, aux, compare_unordered: bool) -> bool:
    if compare_unordered:
        base = sorted(base)
        aux = sorted(aux)
    if len(base) != len(aux):
        return False
    for left, right in zip(base, aux):
        if len(left) != len(right):
            return False
        for expected, actual in zip(left, right):
            if not approx_equal(expected, actual):
                return False
    return True


def compare_results(
    base: list[Tuple],
    aux: list[Tuple],
    entry: SharedQueryEntry,
    phase: str,
) -> None:
    base_norm = normalized(base)
    aux_norm = normalized(aux)
    if not rows_equal(base_norm, aux_norm, entry.compare_unordered):
        raise RuntimeError(
            f"{phase} differential mismatch for {entry.workload} Q{entry.query_id}: "
            f"base_rows={len(base_norm)} aux_rows={len(aux_norm)} "
            f"base_sample={base_norm[:2]} aux_sample={aux_norm[:2]}"
        )


def base_kernel_differential(db: str, entry: SharedQueryEntry) -> int:
    conn = connect(db)
    cur = conn.cursor()
    try:
        base = run_query(cur, entry.base_sql)
        conn.commit()
    finally:
        conn.close()

    conn = connect(db)
    cur = conn.cursor()
    try:
        kernel = run_query(cur, entry.kernel_sql)
        conn.commit()
    finally:
        conn.close()

    compare_results(base, kernel, entry, "base-kernel")
    return len(base)


def differential(db: str, entry: SharedQueryEntry) -> int:
    conn = connect(db)
    cur = conn.cursor()
    try:
        base = run_query(cur, entry.base_sql)
        conn.commit()
    finally:
        conn.close()

    conn = connect(db)
    cur = conn.cursor()
    try:
        aux = run_query(cur, entry.aux_sql)
        conn.commit()
    finally:
        conn.close()

    compare_results(base, aux, entry, "cold")
    return len(base)


def incremental_differential(db: str, entry: SharedQueryEntry) -> int:
    if entry.build_probe is None:
        raise RuntimeError(
            f"missing probe builder for {entry.workload} Q{entry.query_id}"
        )

    conn = connect(db)
    conn.autocommit = True
    cur = conn.cursor()
    try:
        base = run_query(cur, entry.base_sql)
        aux = run_query(cur, entry.aux_sql)
        compare_results(base, aux, entry, "cold")

        probe_row = run_query(cur, entry.probe_select_sql)[0]
        probe = entry.build_probe(probe_row)

        conn.autocommit = False
        cur.execute("BEGIN")
        cur.execute(probe["update"])
        conn.commit()
        conn.autocommit = True

        base_after = run_query(cur, entry.base_sql)
        aux_after = run_query(cur, entry.aux_sql)
        compare_results(base_after, aux_after, entry, "incremental")

        conn.autocommit = False
        cur.execute(probe["restore"], probe["restore_args"])
        conn.commit()
        return len(base)
    finally:
        conn.close()
