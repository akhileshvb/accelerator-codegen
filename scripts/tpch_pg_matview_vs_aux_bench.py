"""Bench native / REFRESH matview / pg_ivm IMMV / Postgres aux for one TPC-H query.

Read-only: median latency + speedups.
Write-mix: compose workload time at write_frequency_pct points from measured
read medians + maintenance costs:
  native  = touch UPDATE
  matview = touch UPDATE + REFRESH MATERIALIZED VIEW
  ivm     = touch UPDATE with pg_ivm IMMV triggers (immediate maintenance)
  aux     = touch UPDATE with mutation triggers (ApplyDelta on commit)

Uses seed_1-compatible bindings (same as matview reference.sql / query.sql).
IMMV is optional per query (see eval/bench/postgres/baselines/tpch/q*/immv/).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import statistics
import sys
import time
from pathlib import Path

import psycopg2

ROOT = Path(__file__).resolve().parents[1]

# Aux calls aligned with matview seed_1 / manifests (not always query_catalog defaults).
AUX_SQL: dict[int, str] = {
    1: "SELECT * FROM accel_tpch_q1_aux(103)",
    2: "SELECT * FROM accel_tpch_q2_aux(10, '%BRASS', 'AFRICA')",
    3: "SELECT * FROM accel_tpch_q3_aux('FURNITURE', DATE '1995-03-10')",
    4: "SELECT * FROM accel_tpch_q4_aux(DATE '1996-11-01')",
    5: "SELECT * FROM accel_tpch_q5_aux('ASIA', DATE '1996-01-01')",
    6: "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
    7: "SELECT * FROM accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
    8: "SELECT * FROM accel_tpch_q8_aux('INDIA', 'ASIA', 'BRASS')",
    9: "SELECT * FROM accel_tpch_q9_aux('powder')",
    10: "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
    11: "SELECT * FROM accel_tpch_q11_aux('ROMANIA', 0.0001)",
    12: "SELECT * FROM accel_tpch_q12_aux('REG AIR', 'SHIP', DATE '1996-01-01')",
    13: "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
    14: "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')",
    15: "SELECT * FROM accel_tpch_q15_aux(DATE '1994-11-01')",
    16: (
        "SELECT * FROM accel_tpch_q16_aux("
        "'Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)"
    ),
    17: "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LARGE BRUSHED')",
    18: "SELECT * FROM accel_tpch_q18_aux(312)",
    19: (
        "SELECT * FROM accel_tpch_q19_aux("
        "'Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)"
    ),
    20: "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
    21: "SELECT * FROM accel_tpch_q21_aux('ALGERIA')",
    22: (
        "SELECT * FROM accel_tpch_q22_aux("
        "'30', '19', '14', '28', '11', '22', '32')"
    ),
}

# Per-query probes: change a watched contribution so ApplyDelta / IMMV triggers
# do real work (identity l_discount updates often NoOp or net-zero-republish).
DEFAULT_TOUCH_SQL = (
    "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
    "WHERE ctid = (SELECT ctid FROM lineitem LIMIT 1)"
)

TOUCH_SQL_BY_QUERY: dict[int, str] = {
    # Revenue-bearing lineitem change (Q1 groups, Q3/Q6/Q8/Q14/Q15/Q19 cubes).
    1: DEFAULT_TOUCH_SQL,
    3: (
        "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
        "WHERE ctid = ("
        "  SELECT l.ctid FROM lineitem l "
        "  JOIN orders o ON o.o_orderkey = l.l_orderkey "
        "  JOIN customer c ON c.c_custkey = o.o_custkey "
        "  WHERE c.c_mktsegment = 'BUILDING' "
        "  LIMIT 1)"
    ),
    5: (
        "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
        "WHERE ctid = ("
        "  SELECT l.ctid FROM lineitem l "
        "  JOIN orders o ON o.o_orderkey = l.l_orderkey "
        "  JOIN customer c ON c.c_custkey = o.o_custkey "
        "  JOIN supplier s ON s.s_suppkey = l.l_suppkey "
        "  WHERE c.c_nationkey = s.s_nationkey "
        "  LIMIT 1)"
    ),
    6: (
        "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
        "WHERE ctid = ("
        "  SELECT ctid FROM lineitem "
        "  WHERE l_shipdate >= DATE '1994-01-01' "
        "    AND l_shipdate < DATE '1995-01-01' "
        "    AND l_discount BETWEEN 0.05 AND 0.07 "
        "    AND l_quantity < 24 "
        "  LIMIT 1)"
    ),
    7: (
        "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
        "WHERE ctid = ("
        "  SELECT ctid FROM lineitem "
        "  WHERE l_shipdate >= DATE '1995-01-01' "
        "    AND l_shipdate < DATE '1997-01-01' "
        "  LIMIT 1)"
    ),
    8: DEFAULT_TOUCH_SQL,
    10: (
        "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
        "WHERE ctid = ("
        "  SELECT ctid FROM lineitem WHERE l_returnflag = 'R' LIMIT 1)"
    ),
    # Q11 watches partsupp, not lineitem.
    11: (
        "UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.01 "
        "WHERE ctid = ("
        "  SELECT ps.ctid FROM partsupp ps "
        "  JOIN supplier s ON s.s_suppkey = ps.ps_suppkey "
        "  JOIN nation n ON n.n_nationkey = s.s_nationkey "
        "  WHERE n.n_name = 'GERMANY' "
        "  LIMIT 1)"
    ),
    # Q12 projection is dates/shipmode — discount updates are ignored.
    12: (
        "UPDATE lineitem SET l_receiptdate = l_receiptdate + 1 "
        "WHERE ctid = ("
        "  SELECT ctid FROM lineitem "
        "  WHERE l_shipmode IN ('MAIL', 'SHIP') "
        "  LIMIT 1)"
    ),
    14: (
        "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
        "WHERE ctid = ("
        "  SELECT ctid FROM lineitem "
        "  WHERE l_shipdate >= DATE '1995-09-01' "
        "    AND l_shipdate < DATE '1995-10-01' "
        "  LIMIT 1)"
    ),
    15: (
        "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
        "WHERE ctid = ("
        "  SELECT ctid FROM lineitem "
        "  WHERE l_shipdate >= DATE '1996-01-01' "
        "    AND l_shipdate < DATE '1996-04-01' "
        "  LIMIT 1)"
    ),
    19: DEFAULT_TOUCH_SQL,
}


def touch_sql_for(query: int) -> str:
    return TOUCH_SQL_BY_QUERY.get(query, DEFAULT_TOUCH_SQL)


def prepare_touch_sql(cur, query: int) -> str:
    """Pin expensive probe keys once so write timing measures maintenance, not joins."""
    # Q1: pin a cold shipdate (before the Aug-1998 hot window). Overlay CoW
    # keeps ApplyDelta to a single tiny partition publish.
    if query == 1:
        cur.execute(
            "CREATE TEMP TABLE IF NOT EXISTS accel_bench_touch_li "
            "(orderkey bigint, linenumber integer)"
        )
        cur.execute("TRUNCATE accel_bench_touch_li")
        cur.execute(
            """
            INSERT INTO accel_bench_touch_li
            SELECT l_orderkey, l_linenumber FROM lineitem
            WHERE l_shipdate < DATE '1998-08-01'
            LIMIT 1
            """
        )
        return (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
            "WHERE (l_orderkey, l_linenumber) = "
            "(SELECT orderkey, linenumber FROM accel_bench_touch_li)"
        )
    # Q5: qualifying lineitem needs cust.nation = supp.nation; resolve once by PK
    # (ctid changes on UPDATE, so it cannot be reused across samples).
    if query == 5:
        cur.execute(
            "CREATE TEMP TABLE IF NOT EXISTS accel_bench_touch_li "
            "(orderkey bigint, linenumber integer)"
        )
        cur.execute("TRUNCATE accel_bench_touch_li")
        cur.execute(
            """
            INSERT INTO accel_bench_touch_li
            SELECT l.l_orderkey, l.l_linenumber
            FROM lineitem l
            JOIN orders o ON o.o_orderkey = l.l_orderkey
            JOIN customer c ON c.c_custkey = o.o_custkey
            JOIN supplier s ON s.s_suppkey = l.l_suppkey
            WHERE c.c_nationkey = s.s_nationkey
            LIMIT 1
            """
        )
        return (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
            "WHERE (l_orderkey, l_linenumber) = "
            "(SELECT orderkey, linenumber FROM accel_bench_touch_li)"
        )
    # Q8: market-share cube only covers order years 1995–1996.
    if query == 8:
        cur.execute(
            "CREATE TEMP TABLE IF NOT EXISTS accel_bench_touch_li "
            "(orderkey bigint, linenumber integer)"
        )
        cur.execute("TRUNCATE accel_bench_touch_li")
        cur.execute(
            """
            INSERT INTO accel_bench_touch_li
            SELECT l.l_orderkey, l.l_linenumber
            FROM lineitem l
            JOIN orders o ON o.o_orderkey = l.l_orderkey
            WHERE o.o_orderdate >= DATE '1995-01-01'
              AND o.o_orderdate < DATE '1997-01-01'
            LIMIT 1
            """
        )
        return (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
            "WHERE (l_orderkey, l_linenumber) = "
            "(SELECT orderkey, linenumber FROM accel_bench_touch_li)"
        )
    # Q12: receipt-date bump must hit a qualifying shipmode window row.
    # Each sample bumps receiptdate; keep the PK fixed and still qualify initially.
    if query == 12:
        cur.execute(
            "CREATE TEMP TABLE IF NOT EXISTS accel_bench_touch_li "
            "(orderkey bigint, linenumber integer)"
        )
        cur.execute("TRUNCATE accel_bench_touch_li")
        cur.execute(
            """
            INSERT INTO accel_bench_touch_li
            SELECT l_orderkey, l_linenumber FROM lineitem
            WHERE l_shipmode IN ('REG AIR', 'SHIP')
              AND l_commitdate < l_receiptdate
              AND l_shipdate < l_commitdate
            LIMIT 1
            """
        )
        return (
            "UPDATE lineitem SET l_receiptdate = l_receiptdate + 1 "
            "WHERE (l_orderkey, l_linenumber) = "
            "(SELECT orderkey, linenumber FROM accel_bench_touch_li)"
        )
    # Q14: query residual uses June 1995; pin a lineitem in that month.
    if query == 14:
        cur.execute(
            "CREATE TEMP TABLE IF NOT EXISTS accel_bench_touch_li "
            "(orderkey bigint, linenumber integer)"
        )
        cur.execute("TRUNCATE accel_bench_touch_li")
        cur.execute(
            """
            INSERT INTO accel_bench_touch_li
            SELECT l_orderkey, l_linenumber FROM lineitem
            WHERE l_shipdate >= DATE '1995-06-01'
              AND l_shipdate < DATE '1995-07-01'
            LIMIT 1
            """
        )
        return (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
            "WHERE (l_orderkey, l_linenumber) = "
            "(SELECT orderkey, linenumber FROM accel_bench_touch_li)"
        )
    # Q19: only AIR/AIR REG + DELIVER IN PERSON rows with a SM/MED/LG part
    # contribute to aggregates — pin one that ApplyDelta will actually mutate.
    if query == 19:
        cur.execute(
            "CREATE TEMP TABLE IF NOT EXISTS accel_bench_touch_li "
            "(orderkey bigint, linenumber integer)"
        )
        cur.execute("TRUNCATE accel_bench_touch_li")
        cur.execute(
            """
            INSERT INTO accel_bench_touch_li
            SELECT l.l_orderkey, l.l_linenumber
            FROM lineitem l
            JOIN part p ON p.p_partkey = l.l_partkey
            WHERE l.l_shipmode IN ('AIR', 'AIR REG')
              AND l.l_shipinstruct = 'DELIVER IN PERSON'
              AND (
                (p.p_container IN ('SM CASE','SM BOX','SM PACK','SM PKG')
                   AND p.p_size BETWEEN 1 AND 5)
                OR (p.p_container IN ('MED BAG','MED BOX','MED PKG','MED PACK')
                   AND p.p_size BETWEEN 1 AND 10)
                OR (p.p_container IN ('LG CASE','LG BOX','LG PACK','LG PKG')
                   AND p.p_size BETWEEN 1 AND 15)
              )
            LIMIT 1
            """
        )
        return (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
            "WHERE (l_orderkey, l_linenumber) = "
            "(SELECT orderkey, linenumber FROM accel_bench_touch_li)"
        )
    return touch_sql_for(query)


WRITE_MIX_FIELDNAMES = [
    "query",
    "write_frequency_pct",
    "ops",
    "writes",
    "native_read_ms",
    "matview_read_ms",
    "ivm_read_ms",
    "aux_read_ms",
    "touch_ms",
    "refresh_ms",
    "ivm_write_ms",
    "aux_write_ms",
    "aux_maint_action",
    "native_workload_ms",
    "matview_workload_ms",
    "ivm_workload_ms",
    "aux_workload_ms",
    "matview_speedup_vs_native",
    "ivm_speedup_vs_native",
    "aux_speedup_vs_native",
    "aux_speedup_vs_matview",
    "aux_speedup_vs_ivm",
    "ivm_speedup_vs_matview",
    "aux_build_ms",
    "ivm_build_ms",
    "ivm_status",
]


def matview_dir(query: int) -> Path:
    return ROOT / "eval" / "bench" / "postgres" / "baselines" / "tpch" / f"q{query}" / "matview"


def immv_dir(query: int) -> Path:
    return ROOT / "eval" / "bench" / "postgres" / "baselines" / "tpch" / f"q{query}" / "immv"


def immv_supported(query: int) -> bool:
    d = immv_dir(query)
    return (d / "create.sql").is_file() and not (d / "unsupported.txt").is_file()


def read_sql(path: Path) -> str:
    return path.read_text(encoding="utf-8").strip().rstrip(";")


def drop_immv(cur, query: int) -> None:
    cur.execute(f"DROP TABLE IF EXISTS accel_tpch_q{query}_immv CASCADE")


def ensure_pg_ivm(cur) -> None:
    cur.execute("CREATE EXTENSION IF NOT EXISTS pg_ivm")


def create_immv_from_file(cur, query: int) -> float:
    """Run create.sql (pgivm.create_immv) and return wall ms."""
    sql = (immv_dir(query) / "create.sql").read_text(encoding="utf-8")
    return timed_exec(cur, sql)

def connect(dbname: str):
    return psycopg2.connect(
        host=os.environ.get("PGHOST", "127.0.0.1"),
        port=int(os.environ.get("PGPORT", "5434")),
        user=os.environ.get("PGUSER", "postgres"),
        password=os.environ.get("PGPASSWORD", "postgres"),
        dbname=dbname,
    )


def timed_exec(cur, sql: str) -> float:
    t0 = time.perf_counter()
    cur.execute(sql)
    # Drain results so timing includes materialization/fetch of the plan output.
    if cur.description is not None:
        cur.fetchall()
    return (time.perf_counter() - t0) * 1000.0


def median_ms(cur, sql: str, warmup: int, samples: int) -> float:
    for _ in range(warmup):
        timed_exec(cur, sql)
    xs = [timed_exec(cur, sql) for _ in range(samples)]
    return float(statistics.median(xs))


def ensure_extension(cur, so_path: str) -> None:
    cur.execute(f"LOAD '{so_path}'")


def speedup(base: float, fast: float) -> float | None:
    if fast and fast > 0:
        return base / fast
    return None


def should_write_at_index(query_index: int, write_frequency_pct: int) -> bool:
    if write_frequency_pct <= 0:
        return False
    if write_frequency_pct == 1:
        return query_index % 100 == 0
    if write_frequency_pct == 10:
        return query_index % 10 == 0
    if write_frequency_pct == 50:
        return query_index % 2 == 0
    stride = 100 // write_frequency_pct
    return stride > 0 and (query_index % stride) == 0


def count_writes(ops: int, write_frequency_pct: int) -> int:
    return sum(1 for i in range(ops) if should_write_at_index(i, write_frequency_pct))


def trigger_names(cur) -> list[str]:
    cur.execute(
        """
        SELECT tgname FROM pg_trigger
        WHERE NOT tgisinternal AND tgname LIKE 'accel_codegen_tpch_%'
        ORDER BY 1
        """
    )
    return [r[0] for r in cur.fetchall()]


def drop_tpch_mutation_triggers(cur) -> None:
    cur.execute(
        """
        SELECT format('DROP TRIGGER IF EXISTS %I ON %I', t.tgname, c.relname)
        FROM pg_trigger t
        JOIN pg_class c ON c.oid = t.tgrelid
        WHERE NOT t.tgisinternal AND t.tgname LIKE 'accel_codegen_tpch_%'
        """
    )
    for (stmt,) in cur.fetchall():
        cur.execute(stmt)


def install_tpch_mutation_triggers(cur) -> None:
    # Shared statement-level capture (lineitem=1 … region=8).
    sql_path = ROOT / "scripts" / "postgres_accel_mutation_triggers.sql"
    cur.execute(sql_path.read_text(encoding="utf-8"))


def last_maint_action(cur) -> str | None:
    try:
        cur.execute("SELECT accel_get_last_maintenance_action()")
        row = cur.fetchone()
        return None if not row else row[0]
    except Exception:
        return None


def set_force_apply_delta(cur) -> None:
    try:
        cur.execute("SELECT accel_set_force_maintenance_action('ApplyDelta')")
        cur.fetchone()
    except Exception:
        pass


def bench_query_readonly(
    query: int,
    dbname: str,
    *,
    warmup: int,
    samples: int,
    measure_refresh: bool,
    so_path: str,
) -> dict:
    mv = matview_dir(query)
    ref_sql = f"SELECT * FROM ({read_sql(mv / 'reference.sql')}) AS _ref"
    mv_sql = f"SELECT * FROM ({read_sql(mv / 'query.sql')}) AS _mv"
    aux_sql = AUX_SQL[query]
    rel = f"accel_tpch_q{query}_matview"
    use_ivm = immv_supported(query)
    ivm_sql = None
    if use_ivm:
        ivm_sql = f"SELECT * FROM ({read_sql(immv_dir(query) / 'query.sql')}) AS _ivm"

    conn = connect(dbname)
    conn.autocommit = True
    cur = conn.cursor()
    ensure_extension(cur, so_path)

    ivm_build_ms = None
    ivm_ms = None
    ivm_status = "unsupported"
    if use_ivm:
        ensure_pg_ivm(cur)
        drop_immv(cur, query)
        ivm_build_ms = create_immv_from_file(cur, query)
        ivm_status = "ok"

    aux_build_ms = timed_exec(cur, f"SELECT COUNT(*) FROM ({aux_sql}) AS _a")
    native_ms = median_ms(cur, ref_sql, warmup, samples)
    matview_ms = median_ms(cur, mv_sql, warmup, samples)
    if use_ivm and ivm_sql is not None:
        ivm_ms = median_ms(cur, ivm_sql, warmup, samples)
    aux_ms = median_ms(cur, f"SELECT * FROM ({aux_sql}) AS _a", warmup, samples)

    refresh_ms = None
    if measure_refresh:
        refresh_ms = timed_exec(cur, f"REFRESH MATERIALIZED VIEW {rel}")

    cur.close()
    conn.close()

    return {
        "query": query,
        "native_median_ms": native_ms,
        "matview_median_ms": matview_ms,
        "ivm_median_ms": ivm_ms,
        "aux_median_ms": aux_ms,
        "aux_build_ms": aux_build_ms,
        "ivm_build_ms": ivm_build_ms,
        "refresh_ms": refresh_ms,
        "ivm_status": ivm_status,
        "matview_speedup_vs_native": speedup(native_ms, matview_ms),
        "ivm_speedup_vs_native": speedup(native_ms, ivm_ms) if ivm_ms else None,
        "aux_speedup_vs_native": speedup(native_ms, aux_ms),
        "aux_speedup_vs_matview": speedup(matview_ms, aux_ms),
        "aux_speedup_vs_ivm": speedup(ivm_ms, aux_ms) if ivm_ms else None,
        "ivm_speedup_vs_matview": speedup(matview_ms, ivm_ms) if ivm_ms else None,
    }


def measure_write_mix_components(
    query: int,
    dbname: str,
    *,
    warmup: int,
    samples: int,
    so_path: str,
    skip_native: bool = False,
    refresh_warmup: int | None = None,
    refresh_samples: int | None = None,
) -> dict:
    mv = matview_dir(query)
    ref_sql = f"SELECT * FROM ({read_sql(mv / 'reference.sql')}) AS _ref"
    mv_sql = f"SELECT * FROM ({read_sql(mv / 'query.sql')}) AS _mv"
    aux_sql = AUX_SQL[query]
    rel = f"accel_tpch_q{query}_matview"
    use_ivm = immv_supported(query)
    ivm_sql = None
    if use_ivm:
        ivm_sql = f"SELECT * FROM ({read_sql(immv_dir(query) / 'query.sql')}) AS _ivm"

    conn = connect(dbname)
    conn.autocommit = True
    cur = conn.cursor()
    ensure_extension(cur, so_path)
    touch_sql = prepare_touch_sql(cur, query)

    # Pure touch: no accel triggers and no IMMV (so IVM triggers are not included).
    drop_tpch_mutation_triggers(cur)
    drop_immv(cur, query)
    touch_ms = median_ms(cur, touch_sql, max(1, warmup), samples)

    ivm_build_ms = None
    ivm_write_ms = None
    ivm_ms = None
    ivm_status = "unsupported"
    if use_ivm:
        ensure_pg_ivm(cur)
        ivm_build_ms = create_immv_from_file(cur, query)
        # Steady-state IMMV maintenance on the same touch UPDATE.
        timed_exec(cur, touch_sql)
        for _ in range(max(1, warmup)):
            timed_exec(cur, touch_sql)
        ivm_write_ms = float(
            statistics.median([timed_exec(cur, touch_sql) for _ in range(samples)])
        )
        ivm_ms = median_ms(cur, ivm_sql, warmup, samples)  # type: ignore[arg-type]
        ivm_status = "ok"
        # Drop IMMV before aux/matview write measurements so IVM triggers do not
        # inflate aux ApplyDelta or pollute subsequent queries in the sweep.
        drop_immv(cur, query)

    # Aux path: restore triggers, force ApplyDelta, build, then time touch+maint.
    install_tpch_mutation_triggers(cur)
    set_force_apply_delta(cur)
    aux_build_ms = timed_exec(cur, f"SELECT COUNT(*) FROM ({aux_sql}) AS _a")
    # One discarded touch so first measured sample is steady-state ApplyDelta.
    timed_exec(cur, touch_sql)
    set_force_apply_delta(cur)
    aux_write_samples: list[float] = []
    maint_actions: list[str] = []
    for _ in range(max(1, warmup)):
        timed_exec(cur, touch_sql)
        set_force_apply_delta(cur)
    for _ in range(samples):
        aux_write_samples.append(timed_exec(cur, touch_sql))
        action = last_maint_action(cur)
        if action:
            maint_actions.append(action)
        set_force_apply_delta(cur)
    aux_write_ms = float(statistics.median(aux_write_samples))
    aux_maint_action = (
        max(set(maint_actions), key=maint_actions.count) if maint_actions else None
    )

    # Native SQL over the full base tables is O(SF); skippable at SF3000.
    native_ms = None if skip_native else median_ms(cur, ref_sql, warmup, samples)
    matview_ms = median_ms(cur, mv_sql, warmup, samples)
    aux_ms = median_ms(cur, f"SELECT * FROM ({aux_sql}) AS _a", warmup, samples)
    # REFRESH rescans the base tables; at SF3000 one timed refresh is all we can afford.
    refresh_ms = median_ms(
        cur,
        f"REFRESH MATERIALIZED VIEW {rel}",
        max(0, warmup - 1) if refresh_warmup is None else refresh_warmup,
        max(1, samples) if refresh_samples is None else refresh_samples,
    )

    cur.close()
    conn.close()

    return {
        "native_read_ms": native_ms,
        "matview_read_ms": matview_ms,
        "ivm_read_ms": ivm_ms,
        "aux_read_ms": aux_ms,
        "touch_ms": touch_ms,
        "refresh_ms": refresh_ms,
        "ivm_write_ms": ivm_write_ms,
        "aux_write_ms": aux_write_ms,
        "aux_maint_action": aux_maint_action,
        "aux_build_ms": aux_build_ms,
        "ivm_build_ms": ivm_build_ms,
        "ivm_status": ivm_status,
    }


def compose_write_mix_rows(
    query: int,
    components: dict,
    write_pcts: list[int],
    ops: int,
) -> list[dict]:
    rows: list[dict] = []
    native_r = components["native_read_ms"]
    native_r = float(native_r) if native_r is not None else None
    mv_r = float(components["matview_read_ms"])
    aux_r = float(components["aux_read_ms"])
    touch = float(components["touch_ms"])
    refresh = float(components["refresh_ms"])
    aux_w = float(components["aux_write_ms"])
    mv_write = touch + refresh
    ivm_r = components.get("ivm_read_ms")
    ivm_w = components.get("ivm_write_ms")
    ivm_ok = ivm_r is not None and ivm_w is not None

    for pct in write_pcts:
        writes = count_writes(ops, pct)
        native_wl = ops * native_r + writes * touch if native_r is not None else None
        mv_wl = ops * mv_r + writes * mv_write
        aux_wl = ops * aux_r + writes * aux_w
        ivm_wl = None
        if ivm_ok:
            ivm_wl = ops * float(ivm_r) + writes * float(ivm_w)
        rows.append(
            {
                "query": query,
                "write_frequency_pct": pct,
                "ops": ops,
                "writes": writes,
                "native_read_ms": native_r,
                "matview_read_ms": mv_r,
                "ivm_read_ms": ivm_r,
                "aux_read_ms": aux_r,
                "touch_ms": touch,
                "refresh_ms": refresh,
                "ivm_write_ms": ivm_w,
                "aux_write_ms": aux_w,
                "aux_maint_action": components.get("aux_maint_action"),
                "native_workload_ms": native_wl,
                "matview_workload_ms": mv_wl,
                "ivm_workload_ms": ivm_wl,
                "aux_workload_ms": aux_wl,
                "matview_speedup_vs_native": speedup(native_wl, mv_wl) if native_wl else None,
                "ivm_speedup_vs_native": speedup(native_wl, ivm_wl) if native_wl and ivm_wl else None,
                "aux_speedup_vs_native": speedup(native_wl, aux_wl) if native_wl else None,
                "aux_speedup_vs_matview": speedup(mv_wl, aux_wl),
                "aux_speedup_vs_ivm": speedup(ivm_wl, aux_wl) if ivm_wl else None,
                "ivm_speedup_vs_matview": speedup(mv_wl, ivm_wl) if ivm_wl else None,
                "aux_build_ms": components["aux_build_ms"],
                "ivm_build_ms": components.get("ivm_build_ms"),
                "ivm_status": components.get("ivm_status", "unsupported"),
            }
        )
    return rows


def append_csv(path: Path, rows: list[dict], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    write_header = not path.exists()
    with path.open("a", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        if write_header:
            w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k) for k in fieldnames})


def parse_pcts(raw: str) -> list[int]:
    pcts: list[int] = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        pct = int(part)
        if pct < 0 or pct > 100:
            raise ValueError(f"write pct out of range: {pct}")
        pcts.append(pct)
    if not pcts:
        raise ValueError("empty --write-pcts")
    return pcts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--query", type=int, required=True)
    ap.add_argument("--db", default="accel_tpch_sf20")
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--no-refresh", action="store_true")
    ap.add_argument(
        "--write-pcts",
        default="",
        help="Comma-separated write frequencies (e.g. 0,1,10,50). "
        "When set, emit composed write-mix rows instead of read-only.",
    )
    ap.add_argument(
        "--ops",
        type=int,
        default=100,
        help="Query slots used when composing write-mix workload times (default 100).",
    )
    ap.add_argument(
        "--so",
        default="/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so",
    )
    ap.add_argument(
        "--skip-native",
        action="store_true",
        default=os.environ.get("ACCEL_PG_SKIP_NATIVE", "0") == "1",
        help="Write-mix only: skip the full-table native SQL read (O(SF)).",
    )
    ap.add_argument(
        "--refresh-samples",
        type=int,
        default=int(os.environ["ACCEL_PG_REFRESH_SAMPLES"])
        if "ACCEL_PG_REFRESH_SAMPLES" in os.environ
        else None,
        help="Write-mix only: timed REFRESH MATERIALIZED VIEW runs (default: --samples). "
        "Uses 0 warmup when set.",
    )
    ap.add_argument("--json-out", type=Path)
    ap.add_argument("--csv-out", type=Path)
    args = ap.parse_args()

    if args.query not in AUX_SQL:
        print(f"unsupported query {args.query}", file=sys.stderr)
        return 2

    if args.write_pcts.strip():
        pcts = parse_pcts(args.write_pcts)
        components = measure_write_mix_components(
            args.query,
            args.db,
            warmup=args.warmup,
            samples=args.samples,
            so_path=args.so,
            skip_native=args.skip_native,
            refresh_warmup=0 if args.refresh_samples is not None else None,
            refresh_samples=args.refresh_samples,
        )
        rows = compose_write_mix_rows(args.query, components, pcts, args.ops)
        payload = {"query": args.query, "components": components, "rows": rows}
        print(json.dumps(payload, indent=2))
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        if args.csv_out:
            append_csv(args.csv_out, rows, WRITE_MIX_FIELDNAMES)
        return 0

    row = bench_query_readonly(
        args.query,
        args.db,
        warmup=args.warmup,
        samples=args.samples,
        measure_refresh=not args.no_refresh,
        so_path=args.so,
    )
    print(json.dumps(row, indent=2))
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(row, indent=2) + "\n", encoding="utf-8")
    if args.csv_out:
        append_csv(args.csv_out, [row], list(row.keys()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
