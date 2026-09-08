"""Shared PostgreSQL differential helpers for cold, incremental, and RF checks."""

from __future__ import annotations

import json
import os
import re
from typing import Any, Tuple

import psycopg2
from psycopg2 import sql

from shared_query_catalog import SharedQueryEntry
from tpch_rf import apply_rf_chunk


def run_query(cur, query, args=()):
    # psycopg2 treats bare '%' as parameter placeholders; literal LIKE patterns
    # need escaping when the query has no bound parameters.
    if not args and "%" in query:
        query = query.replace("%", "%%")
    cur.execute(query, args)
    return cur.fetchall()


def normalized(rows):
    return [tuple("NULL" if v is None else str(v) for v in row) for row in rows]


def expand_probe_select_limit(sql: str, limit: int) -> str:
    stripped = sql.strip()
    match = re.search(r"LIMIT\s+\d+\s*$", stripped, flags=re.IGNORECASE)
    if match:
        return stripped[: match.start()] + f"LIMIT {limit}"
    return stripped + f" LIMIT {limit}"


def fingerprint_base(cur, entry: SharedQueryEntry):
    return normalized(run_query(cur, entry.base_sql))


def probe_changes_base(conn, cur, entry: SharedQueryEntry, probe: dict, before) -> bool:
    cur.execute("BEGIN")
    try:
        cur.execute(probe["update"])
        after = fingerprint_base(cur, entry)
    finally:
        conn.rollback()
    return after != before


def collect_invalidating_probe(conn, cur, entry: SharedQueryEntry, candidate_limit: int = 256):
    """Return a catalog probe whose UPDATE changes this query's base_sql result.

    Grow LIMIT 1 → 8 → 32 → candidate_limit so expensive probes (Q21) do not
    materialize 256 candidates before testing the first hit.
    """
    if entry.build_probe is None:
        raise RuntimeError(
            f"missing probe builder for {entry.workload} Q{entry.query_id}"
        )
    before = fingerprint_base(cur, entry)
    conn.commit()
    limits = []
    for limit in (1, 8, 32, candidate_limit):
        if limit <= candidate_limit and limit not in limits:
            limits.append(limit)
    tried = 0
    last_count = 0
    for limit in limits:
        rows = run_query(cur, expand_probe_select_limit(entry.probe_select_sql, limit))
        conn.commit()
        last_count = len(rows)
        for row in rows[tried:]:
            probe = entry.build_probe(row)
            if probe_changes_base(conn, cur, entry, probe, before):
                return probe
        tried = len(rows)
    raise RuntimeError(
        f"no invalidating catalog probe for {entry.workload} Q{entry.query_id} "
        f"(candidates={last_count})"
    )


def connect(db):
    host = os.environ.get("PG_HOST", "127.0.0.1")
    port = int(os.environ.get("PG_PORT", "5434"))
    # Fail fast if the server dies mid-sweep (prior Q21 hang had no timeout).
    connect_timeout = int(os.environ.get("ACCEL_PG_CONNECT_TIMEOUT_SEC", "30"))
    statement_timeout_ms = int(
        os.environ.get("ACCEL_PG_STATEMENT_TIMEOUT_MS", str(30 * 60 * 1000))
    )
    conn = psycopg2.connect(
        host=host,
        port=port,
        user=os.environ.get("PGUSER", "postgres"),
        password=os.environ.get("PGPASSWORD", "postgres"),
        dbname=db,
        connect_timeout=connect_timeout,
        keepalives=1,
        keepalives_idle=30,
        keepalives_interval=10,
        keepalives_count=5,
        options=f"-c statement_timeout={statement_timeout_ms}",
    )
    return conn


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
    conn.autocommit = False
    cur = conn.cursor()
    try:
        base = run_query(cur, entry.base_sql)
        conn.commit()
        aux = run_query(cur, entry.aux_sql)
        conn.commit()
        compare_results(base, aux, entry, "cold")

        probe = collect_invalidating_probe(conn, cur, entry)
        before = fingerprint_base(cur, entry)
        conn.commit()

        cur.execute(probe["update"])
        conn.commit()

        base_after = fingerprint_base(cur, entry)
        conn.commit()
        if base_after == before:
            raise RuntimeError(
                f"selected probe did not change {entry.workload} Q{entry.query_id} "
                "base result — write did not invalidate observable query state"
            )
        aux_after = run_query(cur, entry.aux_sql)
        conn.commit()
        compare_results(base_after, aux_after, entry, "incremental")

        cur.execute(probe["restore"], probe["restore_args"])
        conn.commit()
        return len(base)
    finally:
        conn.close()


def parse_explain_execution_us(payload: Any) -> tuple[float, int]:
    """Extract EXPLAIN ANALYZE Execution Time (µs) and root Actual Rows.

    Matches GenDB's compute-not-output convention: server executor time, not
    client fetch/materialization. DuckDB benches use JSON profiler ``latency``
    the same way. TIMING OFF still reports statement Execution Time without
    per-node clock overhead.
    """
    if isinstance(payload, str):
        payload = json.loads(payload)
    if isinstance(payload, list):
        if not payload:
            raise RuntimeError("empty EXPLAIN ANALYZE JSON")
        payload = payload[0]
    if not isinstance(payload, dict):
        raise RuntimeError(f"unexpected EXPLAIN ANALYZE payload: {type(payload)}")
    exec_ms = payload.get("Execution Time")
    if exec_ms is None:
        raise RuntimeError("EXPLAIN ANALYZE JSON missing Execution Time")
    plan = payload.get("Plan") or {}
    actual_rows = int(plan.get("Actual Rows") or 0)
    return float(exec_ms) * 1000.0, actual_rows


def measure_sql_execution_us(cur, sql_text: str) -> tuple[float, int]:
    """Run the statement under EXPLAIN ANALYZE and return (exec_us, actual_rows)."""
    # TIMING OFF: still reports statement Execution Time, without per-node
    # clock overhead. Analog of GenDB [TIMING] total minus [TIMING] output.
    explain = "EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF) " + sql_text.strip()
    rows = run_query(cur, explain)
    if not rows:
        raise RuntimeError("EXPLAIN ANALYZE returned no rows")
    return parse_explain_execution_us(rows[0][0])


def clone_database(source: str, dest: str) -> None:
    admin = connect("postgres")
    admin.autocommit = True
    cur = admin.cursor()
    try:
        for name in (dest, source):
            cur.execute(
                "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                "WHERE datname = %s AND pid <> pg_backend_pid()",
                (name,),
            )
        cur.execute(sql.SQL("DROP DATABASE IF EXISTS {}").format(sql.Identifier(dest)))
        cur.execute(
            sql.SQL("CREATE DATABASE {} TEMPLATE {}").format(
                sql.Identifier(dest), sql.Identifier(source)
            )
        )
    finally:
        admin.close()


def drop_database(name: str) -> None:
    admin = connect("postgres")
    admin.autocommit = True
    cur = admin.cursor()
    try:
        cur.execute(
            "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
            "WHERE datname = %s AND pid <> pg_backend_pid()",
            (name,),
        )
        cur.execute(sql.SQL("DROP DATABASE IF EXISTS {}").format(sql.Identifier(name)))
    finally:
        admin.close()


def rf_differential(
    source_db: str,
    entry: SharedQueryEntry,
    *,
    num_chunks: int = 4,
    order_count: int = 1,
) -> int:
    """SF1 RF correctness: aux must match base after RF chunks that invalidate.

    Uses a TEMPLATE clone so RF insert/delete does not pollute the source SF1 DB.
    Q11 is rejected: RF never touches partsupp/supplier/nation.
    """
    if entry.workload != "tpch":
        raise RuntimeError("RF differential is TPC-H only")
    if entry.query_id == 11:
        raise RuntimeError("Q11 is unaffected by RF (no orders/lineitem dependency)")
    if num_chunks <= 0 or order_count <= 0:
        raise RuntimeError("num_chunks and order_count must be positive")

    work_db = f"{source_db}_rf_q{entry.query_id}"
    clone_database(source_db, work_db)
    conn = connect(work_db)
    conn.autocommit = False
    cur = conn.cursor()
    try:
        base = run_query(cur, entry.base_sql)
        conn.commit()
        aux = run_query(cur, entry.aux_sql)
        conn.commit()
        compare_results(base, aux, entry, "rf-cold")

        before = fingerprint_base(cur, entry)
        conn.commit()
        changed = False
        orders = order_count
        seq = 0
        for _attempt in range(8):
            apply_rf_chunk(cur, seq, orders)
            conn.commit()
            seq += 1
            base_after = run_query(cur, entry.base_sql)
            conn.commit()
            aux_after = run_query(cur, entry.aux_sql)
            conn.commit()
            compare_results(base_after, aux_after, entry, f"rf-chunk-{seq - 1}")
            if normalized(base_after) != before:
                changed = True
                break
            orders = min(max(orders * 2, orders + 10), 200)

        if not changed:
            probe = collect_invalidating_probe(conn, cur, entry)
            cur.execute(probe["update"])
            conn.commit()
            base_after = fingerprint_base(cur, entry)
            conn.commit()
            if base_after == before:
                raise RuntimeError(
                    f"RF+probe did not change {entry.workload} Q{entry.query_id} "
                    "result — cannot confirm aux invalidation"
                )
            aux_after = run_query(cur, entry.aux_sql)
            conn.commit()
            compare_results(base_after, aux_after, entry, "rf-probe")
            changed = True

        while seq < num_chunks:
            apply_rf_chunk(cur, seq, order_count)
            conn.commit()
            seq += 1
            base_after = run_query(cur, entry.base_sql)
            conn.commit()
            aux_after = run_query(cur, entry.aux_sql)
            conn.commit()
            compare_results(base_after, aux_after, entry, f"rf-chunk-{seq - 1}")

        if not changed:
            raise RuntimeError("internal: RF differential exited without invalidation")
        return len(base)
    finally:
        conn.close()
        drop_database(work_db)
