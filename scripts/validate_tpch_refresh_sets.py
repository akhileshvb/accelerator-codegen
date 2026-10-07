#!/usr/bin/env python3
"""Check TPC-H update sets (dbgen -U output) against the DuckDB database they will be applied to (read-only):
the delete keys exist, the insert keys do not, no key is deleted or inserted twice across sets, and every set has
the same size.  Exit status 1 if any check fails.  Needs the duckdb module.

  python3 scripts/validate_tpch_refresh_sets.py --db /flash1/akhilvb/tpch/tpch_sf20.duckdb --dir /flash1/akhilvb/tpch/refresh_sf20 --sets 40
"""
import argparse
import sys

import duckdb

ap = argparse.ArgumentParser()
ap.add_argument("--db", required=True)
ap.add_argument("--dir", required=True)
ap.add_argument("--sets", type=int, required=True)
a = ap.parse_args()

d = duckdb.connect(a.db, read_only=True)
cols = lambda n: "{" + ",".join(f"'c{i}':'VARCHAR'" for i in range(n)) + "}"
ins = " union all ".join(f"select {i} as s, c0::bigint as k from read_csv('{a.dir}/orders.tbl.u{i}', delim='|', header=false, columns={cols(10)})"
                         for i in range(1, a.sets + 1))
dele = " union all ".join(f"select {i} as s, c0::bigint as k from read_csv('{a.dir}/delete.{i}', delim='|', header=false, columns={cols(2)})"
                          for i in range(1, a.sets + 1))
d.execute(f"create temp table ins as {ins}")
d.execute(f"create temp table del as {dele}")
q = lambda sql: d.execute(sql).fetchall()[0]
n_ins, n_ins_distinct = q("select count(*), count(distinct k) from ins")
n_del, n_del_distinct = q("select count(*), count(distinct k) from del")
sizes = q("select min(c), max(c) from (select s, count(*) c from ins group by s)") + q("select min(c), max(c) from (select s, count(*) c from del group by s)")
bad = {
    "insert keys already in orders": q("select count(*) from ins join orders o on o.o_orderkey = ins.k")[0],
    "delete keys missing from orders": q("select count(*) from del left join orders o on o.o_orderkey = del.k where o.o_orderkey is null")[0],
    "keys both inserted and deleted": q("select count(*) from del join ins using (k)")[0],
    "duplicate insert keys": n_ins - n_ins_distinct,
    "duplicate delete keys": n_del - n_del_distinct,
    "sets of different size": int(sizes[0] != sizes[1] or sizes[2] != sizes[3]),
}
print(f"{a.sets} sets: {sizes[0]} orders each; deleted orders have {q('select count(*) from lineitem l join del on l.l_orderkey = del.k')[0]} lineitems in total")
for k, v in bad.items():
    print(f"  {k}: {v}")
sys.exit(1 if any(bad.values()) else 0)
