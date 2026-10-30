"""Materialized-view baseline for the TPC-H accelerators on Postgres: one parameter-agnostic view set per query.

Each query gets views that answer EVERY parameter set of the workload's TPC-H substitution domain
(gen_tpch_params.py), not one fixed set: the views group by the columns the parameters filter on, and the
read (READ[q], the same $n verus arguments as the kit's SQL) filters / aggregates the views.  Views pg_ivm can
maintain are IMMVs (pgivm.create_immv: maintained by triggers inside every writing statement).  The rest
(Q21, Q22: NOT EXISTS, aggregate subqueries) are plain materialized views that the writer refreshes in the
same transaction as every commit that writes a table they read (FULL[...]["deps"]), so their reads are as
fresh as the IMMVs' and the accelerators'.

  python mv_defs.py create --schema public [--queries "1 2 ..."] [--jobs 8]   # build (IMMVs + MVs + indexes)
  python mv_defs.py drop   --schema public
  python mv_defs.py check  --schema mvdev --seed 35489017 --sets 2           # MV answers vs the base query

The views live in the base tables' schema (names mv_q*), so a session reads them with search_path = schema.
"""
import argparse, os, re, sys, time
from concurrent.futures import ThreadPoolExecutor

# static helper tables (never written by the workload): the LIKE domains of Q9 and Q13
COLORS = None  # filled from gen_tpch_params.COLORS
AUX = {
    "mv_q9_colors": ("color text PRIMARY KEY", lambda: [(c,) for c in COLORS]),
    "mv_q13_patterns": ("pat text PRIMARY KEY", lambda: [(f"%{a}%{b}%",) for a in W1 for b in W2]),
}
W1, W2 = None, None

# a packed single-column group key (see mv_q9): pg_ivm matches every group-key column NULL-safely, so only an
# index on ONE column makes its per-group lookup exact
PK = lambda *cols: "(" + " || '|' || ".join(f"({c})::text" for c in cols) + ")"
YM = lambda col: f"(EXTRACT(year FROM {col}) * 12 + EXTRACT(month FROM {col}))"  # month index, immutable on date

# IMMVs: name -> (query, [index columns lists]).  pg_ivm also creates a unique index on the group keys.
IMMV = {
    1: {"mv_q1": (f"""SELECT {PK('l_returnflag', 'l_linestatus', "l_shipdate - DATE '1970-01-01'")} AS k,
                       l_returnflag, l_linestatus, l_shipdate, SUM(l_quantity) AS sq, SUM(l_extendedprice) AS sp,
                       SUM(l_extendedprice * (1 - l_discount)) AS sdp, SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sc,
                       SUM(l_discount) AS sd, COUNT(*) AS cnt
                     FROM lineitem GROUP BY {PK('l_returnflag', 'l_linestatus', "l_shipdate - DATE '1970-01-01'")}, l_returnflag, l_linestatus, l_shipdate""",
                  [["l_shipdate"], ["k"]])},
    2: {"mv_q2": ("""SELECT p_partkey, p_size, p_type, p_mfgr, ps_partkey, ps_suppkey, ps_supplycost, s_suppkey, s_acctbal, s_name,
                       s_address, s_phone, s_comment, n_nationkey, n_name, r_regionkey, r_name
                     FROM part, partsupp, supplier, nation, region
                     WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey
                       AND n_regionkey = r_regionkey""", [["r_name", "p_size"], ["p_partkey", "r_name", "ps_supplycost"]])},
    3: {"mv_q3": ("""SELECT l_orderkey, o_orderdate, o_shippriority, c_mktsegment, l_shipdate,
                       SUM(l_extendedprice * (1 - l_discount)) AS rev
                     FROM customer, orders, lineitem
                     WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey
                       AND o_orderdate < DATE '1995-03-31' AND l_shipdate > DATE '1995-03-01'
                     GROUP BY l_orderkey, o_orderdate, o_shippriority, c_mktsegment, l_shipdate""",
                  [["c_mktsegment", "o_orderdate"], ["l_orderkey"]])},
    # one row per order with a late line (pg_ivm maintains an EXISTS view by scanning all orders per change; a join
    # grouped by the order is maintained by key lookups)
    4: {"mv_q4": ("""SELECT o_orderkey, o_orderpriority, o_orderdate, COUNT(*) AS late FROM orders, lineitem
                     WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate
                     GROUP BY o_orderkey, o_orderpriority, o_orderdate""",
                  ["(o_orderdate) INCLUDE (o_orderpriority)", ["o_orderkey"]])},
    5: {"mv_q5": ("""SELECT n_name, r_name, o_orderdate, SUM(l_extendedprice * (1 - l_discount)) AS rev
                     FROM customer, orders, lineitem, supplier, nation, region
                     WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey
                       AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
                       AND o_orderdate >= DATE '1993-01-01' AND o_orderdate < DATE '1998-01-01'
                     GROUP BY n_name, r_name, o_orderdate""", [["r_name", "o_orderdate"], ["o_orderdate"]])},
    6: {"mv_q6": (f"""SELECT {PK("l_shipdate - DATE '1970-01-01'", 'l_discount', 'l_quantity')} AS k, l_shipdate, l_discount, l_quantity, SUM(l_extendedprice * l_discount) AS rev FROM lineitem
                     WHERE l_shipdate >= DATE '1993-01-01' AND l_shipdate < DATE '1998-01-01'
                       AND l_discount BETWEEN 0.01 AND 0.10 AND l_quantity < 25
                     GROUP BY {PK("l_shipdate - DATE '1970-01-01'", 'l_discount', 'l_quantity')}, l_shipdate, l_discount, l_quantity""",
                  [["l_shipdate"], ["k"]])},
    7: {"mv_q7": ("""SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, EXTRACT(year FROM l_shipdate) AS l_year,
                       SUM(l_extendedprice * (1 - l_discount)) AS revenue
                     FROM supplier, lineitem, orders, customer, nation n1, nation n2
                     WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey AND c_custkey = o_custkey
                       AND s_nationkey = n1.n_nationkey AND c_nationkey = n2.n_nationkey
                       AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
                     GROUP BY n1.n_name, n2.n_name, EXTRACT(year FROM l_shipdate)""", [])},
    8: {"mv_q8": (f"""SELECT {PK('EXTRACT(year FROM o_orderdate)', 'n2.n_name', 'r_name', 'p_type')} AS k,
                       EXTRACT(year FROM o_orderdate) AS o_year, n2.n_name AS nation, r_name, p_type,
                       SUM(l_extendedprice * (1 - l_discount)) AS volume
                     FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region
                     WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey AND l_orderkey = o_orderkey
                       AND o_custkey = c_custkey AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey
                       AND s_nationkey = n2.n_nationkey AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
                     GROUP BY {PK('EXTRACT(year FROM o_orderdate)', 'n2.n_name', 'r_name', 'p_type')},
                       EXTRACT(year FROM o_orderdate), n2.n_name, r_name, p_type""", [["r_name", "p_type"], ["k"]])},
    # k packs the group key into one column: pg_ivm matches each key NULL-safely, so only a single-column index
    # makes its per-group lookup exact (color alone is ~175 rows per probe)
    9: {"mv_q9": ("""SELECT c.color || '|' || n_name || '|' || EXTRACT(year FROM o_orderdate)::text AS k,
                       c.color, n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year,
                       SUM(l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity) AS amount
                     FROM part, supplier, lineitem, partsupp, orders, nation, mv_q9_colors c
                     WHERE s_suppkey = l_suppkey AND ps_suppkey = l_suppkey AND ps_partkey = l_partkey
                       AND p_partkey = l_partkey AND o_orderkey = l_orderkey AND s_nationkey = n_nationkey
                       AND p_name LIKE '%' || c.color || '%'
                     GROUP BY c.color || '|' || n_name || '|' || EXTRACT(year FROM o_orderdate)::text,
                       c.color, n_name, EXTRACT(year FROM o_orderdate)""", [["color"], ["k"]])},
    10: {"mv_q10": (f"""SELECT o_custkey, {YM('o_orderdate')} AS ym, SUM(l_extendedprice * (1 - l_discount)) AS rev
                      FROM orders, lineitem
                      WHERE l_orderkey = o_orderkey AND l_returnflag = 'R'
                        AND o_orderdate >= DATE '1993-02-01' AND o_orderdate < DATE '1995-04-01'
                      GROUP BY o_custkey, {YM('o_orderdate')}""", [["ym"], ["o_custkey"]])},
    11: {"mv_q11": ("""SELECT ps_partkey, n_name, SUM(ps_supplycost * ps_availqty) AS value
                       FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
                       GROUP BY ps_partkey, n_name""", [["n_name", "value"], ["ps_partkey"]]),
         "mv_q11n": ("""SELECT n_name, SUM(ps_supplycost * ps_availqty) AS total
                        FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
                        GROUP BY n_name""", [])},
    12: {"mv_q12": ("""SELECT l_shipmode, l_receiptdate,
                         SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) AS high,
                         SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) AS low
                       FROM orders, lineitem
                       WHERE o_orderkey = l_orderkey AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate
                       GROUP BY l_shipmode, l_receiptdate""", [["l_shipmode", "l_receiptdate"], ["l_receiptdate"]])},
    13: {"mv_q13_all": ("SELECT o_custkey, COUNT(*) AS cnt FROM orders GROUP BY o_custkey", []),
         "mv_q13_pat": ("""SELECT p.pat, o_custkey, COUNT(*) AS cnt FROM orders, mv_q13_patterns p
                           WHERE o_comment LIKE p.pat GROUP BY p.pat, o_custkey""", [["pat"], ["o_custkey"]]),
         "mv_q13_ncust": ("SELECT COUNT(*) AS cnt FROM customer", [])},
    14: {"mv_q14": ("""SELECT l_shipdate,
                         SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END) AS promo,
                         SUM(l_extendedprice * (1 - l_discount)) AS total
                       FROM lineitem, part WHERE l_partkey = p_partkey GROUP BY l_shipdate""", [["l_shipdate"]])},
    15: {"mv_q15": (f"""SELECT {PK('l_suppkey', YM('l_shipdate'))} AS k, l_suppkey, {YM('l_shipdate')} AS ym, SUM(l_extendedprice * (1 - l_discount)) AS rev
                      FROM lineitem WHERE l_shipdate >= DATE '1993-01-01' AND l_shipdate < DATE '1998-01-01'
                      GROUP BY {PK('l_suppkey', YM('l_shipdate'))}, l_suppkey, {YM('l_shipdate')}""", [["ym"], ["k"]])},
    16: {"mv_q16": ("""SELECT p_brand, p_type, p_size, ps_suppkey, COUNT(*) AS cnt
                       FROM partsupp, part, supplier
                       WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey
                         AND s_comment NOT LIKE '%Customer%Complaints%'
                       GROUP BY p_brand, p_type, p_size, ps_suppkey""",
                    ["(p_size) INCLUDE (p_brand, p_type, ps_suppkey)", ["ps_suppkey"]])},
    17: {"mv_q17": (f"""SELECT {PK('l_partkey', 'l_quantity')} AS k, l_partkey, p_brand, p_container, l_quantity,
                         SUM(l_extendedprice) AS sp, COUNT(*) AS cnt
                       FROM lineitem, part WHERE p_partkey = l_partkey
                       GROUP BY {PK('l_partkey', 'l_quantity')}, l_partkey, p_brand, p_container, l_quantity""",
                    [["p_brand", "p_container"], ["k"]])},
    18: {"mv_q18": ("SELECT l_orderkey, SUM(l_quantity) AS sum_qty FROM lineitem GROUP BY l_orderkey", [["sum_qty"]])},
    19: {"mv_q19": ("""SELECT p_brand, p_container, p_size, l_quantity, SUM(l_extendedprice * (1 - l_discount)) AS rev
                       FROM lineitem, part
                       WHERE p_partkey = l_partkey AND l_shipmode IN ('AIR', 'AIR REG')
                         AND l_shipinstruct = 'DELIVER IN PERSON' AND p_size BETWEEN 1 AND 15
                         AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG', 'MED BAG', 'MED BOX', 'MED PKG',
                                             'MED PACK', 'LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
                       GROUP BY p_brand, p_container, p_size, l_quantity""", [["p_brand"]])},
    20: {"mv_q20": (f"""SELECT {PK('l_partkey', 'l_suppkey', 'EXTRACT(year FROM l_shipdate)')} AS k,
                         l_partkey, l_suppkey, p_name, EXTRACT(year FROM l_shipdate) AS yr, SUM(l_quantity) AS sq
                       FROM lineitem, part
                       WHERE p_partkey = l_partkey AND l_shipdate >= DATE '1993-01-01' AND l_shipdate < DATE '1998-01-01'
                       GROUP BY {PK('l_partkey', 'l_suppkey', 'EXTRACT(year FROM l_shipdate)')},
                         l_partkey, l_suppkey, p_name, EXTRACT(year FROM l_shipdate)""",
                    [["p_name text_pattern_ops", "yr"], ["k"]])},
    22: {"mv_q22_avg": ("""SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, SUM(c_acctbal) AS s, COUNT(*) AS n
                           FROM customer WHERE c_acctbal > 0.00 GROUP BY SUBSTRING(c_phone FROM 1 FOR 2)""", [])},
}

# plain materialized views, refreshed by the writer after each commit that writes one of deps
FULL = {
    # counted per supplier key first, then joined to supplier / nation: written as Q21 is, the planner estimates
    # the NOT EXISTS anti join at 1 row and nests every supplier over its (millions of rows) result
    21: {"mv_q21": ("""SELECT s_name, n_name, SUM(w.numwait) AS numwait
                       FROM (SELECT l1.l_suppkey, COUNT(*) AS numwait
                             FROM lineitem l1, orders
                             WHERE o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
                               AND l1.l_receiptdate > l1.l_commitdate
                               AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey
                                           AND l2.l_suppkey <> l1.l_suppkey)
                               AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey
                                               AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
                             GROUP BY l1.l_suppkey) w, supplier, nation
                       WHERE s_suppkey = w.l_suppkey AND s_nationkey = n_nationkey
                       GROUP BY s_name, n_name""", [["n_name"], "UNIQUE (s_name, n_name)"],
                    {"supplier", "lineitem", "orders", "nation"})},
    22: {"mv_q22": ("""SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_custkey, c_acctbal FROM customer
                       WHERE NOT EXISTS (SELECT 1 FROM orders WHERE o_custkey = c_custkey)""",
                    [["cntrycode", "c_acctbal"], "UNIQUE (c_custkey)"], {"customer", "orders"})},
}

# the reads: the kit's $n (verus) arguments
IN7 = "$1, $2, $3, $4, $5, $6, $7"
READ = {
    1: """SELECT l_returnflag, l_linestatus, SUM(sq) AS sum_qty, SUM(sp) AS sum_base_price, SUM(sdp) AS sum_disc_price,
            SUM(sc) AS sum_charge, SUM(sq) / SUM(cnt) AS avg_qty, SUM(sp) / SUM(cnt) AS avg_price,
            SUM(sd) / SUM(cnt) AS avg_disc, SUM(cnt) AS count_order
          FROM mv_q1 WHERE l_shipdate <= DATE '1998-12-01' - $1 * INTERVAL '1' DAY
          GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus""",
    2: """SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
          FROM mv_q2 m WHERE p_size = $1 AND p_type LIKE $2 AND r_name = $3
            AND ps_supplycost = (SELECT MIN(m2.ps_supplycost) FROM mv_q2 m2 WHERE m2.p_partkey = m.p_partkey AND m2.r_name = $3)
          ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100""",
    3: """SELECT l_orderkey, SUM(rev) AS revenue, o_orderdate, o_shippriority FROM mv_q3
          WHERE c_mktsegment = $1 AND o_orderdate < $2 AND l_shipdate > $2
          GROUP BY l_orderkey, o_orderdate, o_shippriority ORDER BY revenue DESC, o_orderdate LIMIT 10""",
    4: """SELECT o_orderpriority, COUNT(*) AS order_count FROM mv_q4 WHERE o_orderdate >= $1 AND o_orderdate < $2
          GROUP BY o_orderpriority ORDER BY o_orderpriority""",
    5: """SELECT n_name, SUM(rev) AS revenue FROM mv_q5 WHERE r_name = $1 AND o_orderdate >= $2 AND o_orderdate < $3
          GROUP BY n_name ORDER BY revenue DESC""",
    6: """SELECT SUM(rev) AS revenue FROM mv_q6 WHERE l_shipdate >= $1 AND l_shipdate < $4
            AND l_discount BETWEEN $2 - 0.01 AND $2 + 0.01 AND l_quantity < $3""",
    7: """SELECT supp_nation, cust_nation, l_year, SUM(revenue) AS revenue FROM mv_q7
          WHERE (supp_nation = $1 AND cust_nation = $2) OR (supp_nation = $2 AND cust_nation = $1)
          GROUP BY supp_nation, cust_nation, l_year ORDER BY supp_nation, cust_nation, l_year""",
    8: """SELECT o_year, SUM(CASE WHEN nation = $1 THEN volume ELSE 0 END) / SUM(volume) AS mkt_share FROM mv_q8
          WHERE r_name = $2 AND p_type = $3 GROUP BY o_year ORDER BY o_year""",
    9: """SELECT nation, o_year, SUM(amount) AS sum_profit FROM mv_q9 WHERE color = btrim($1, '%')
          GROUP BY nation, o_year ORDER BY nation, o_year DESC""",
    10: f"""WITH t AS (SELECT o_custkey, SUM(rev) AS revenue FROM mv_q10
                     WHERE ym >= {YM('$1')} AND ym < {YM('$2')} GROUP BY o_custkey)
           SELECT c_custkey, c_name, t.revenue, c_acctbal, n_name, c_address, c_phone, c_comment
           FROM t JOIN customer ON c_custkey = t.o_custkey JOIN nation ON c_nationkey = n_nationkey
           ORDER BY t.revenue DESC, c_custkey LIMIT 20""",
    11: """SELECT ps_partkey, value FROM mv_q11
           WHERE n_name = $1 AND value > (SELECT total * $2 FROM mv_q11n WHERE n_name = $1) ORDER BY value DESC""",
    12: """SELECT l_shipmode, SUM(high) AS high_line_count, SUM(low) AS low_line_count FROM mv_q12
           WHERE l_shipmode IN ($1, $2) AND l_receiptdate >= $3 AND l_receiptdate < $4
           GROUP BY l_shipmode ORDER BY l_shipmode""",
    13: """WITH h AS (SELECT a.cnt - COALESCE(b.cnt, 0) AS c_count, COUNT(*) AS n
                     FROM mv_q13_all a LEFT JOIN mv_q13_pat b ON b.o_custkey = a.o_custkey AND b.pat = $1
                     GROUP BY 1
                     UNION ALL
                     SELECT 0, (SELECT cnt FROM mv_q13_ncust) - (SELECT COUNT(*) FROM mv_q13_all))
           SELECT c_count, SUM(n) AS custdist FROM h GROUP BY c_count HAVING SUM(n) > 0
           ORDER BY custdist DESC, c_count DESC""",
    14: """SELECT 100.00 * SUM(promo) / SUM(total) AS promo_revenue FROM mv_q14 WHERE l_shipdate >= $1 AND l_shipdate < $2""",
    15: f"""WITH r AS (SELECT l_suppkey AS supplier_no, SUM(rev) AS total_revenue FROM mv_q15
                     WHERE ym >= {YM('$1')} AND ym < {YM('$2')} GROUP BY l_suppkey)
           SELECT s_suppkey, s_name, s_address, s_phone, total_revenue FROM supplier, r
           WHERE s_suppkey = supplier_no AND total_revenue = (SELECT MAX(total_revenue) FROM r) ORDER BY s_suppkey""",
    16: """SELECT p_brand, p_type, p_size, COUNT(*) AS supplier_cnt FROM mv_q16
           WHERE p_brand <> $1 AND p_type NOT LIKE $2 AND p_size IN ($3, $4, $5, $6, $7, $8, $9, $10)
           GROUP BY p_brand, p_type, p_size ORDER BY supplier_cnt DESC, p_brand, p_type, p_size""",
    17: """WITH m AS (SELECT l_partkey, l_quantity, sp, cnt FROM mv_q17 WHERE p_brand = $1 AND p_container = $2),
                a AS (SELECT l_partkey, 0.2 * (SUM(l_quantity * cnt) / SUM(cnt)) AS avg_q FROM m GROUP BY l_partkey)
           SELECT SUM(m.sp) / 7.0 AS avg_yearly FROM m JOIN a ON m.l_partkey = a.l_partkey WHERE m.l_quantity < a.avg_q""",
    18: """SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, m.sum_qty
           FROM mv_q18 m JOIN orders ON o_orderkey = m.l_orderkey JOIN customer ON c_custkey = o_custkey
           WHERE m.sum_qty > $1 ORDER BY o_totalprice DESC, o_orderdate""",
    19: """SELECT SUM(rev) AS revenue FROM mv_q19
           WHERE (p_brand = $1 AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
                  AND l_quantity >= $2 AND l_quantity - 10 <= $2 AND p_size BETWEEN 1 AND 5)
              OR (p_brand = $3 AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
                  AND l_quantity >= $4 AND l_quantity - 10 <= $4 AND p_size BETWEEN 1 AND 10)
              OR (p_brand = $5 AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
                  AND l_quantity >= $6 AND l_quantity - 10 <= $6 AND p_size BETWEEN 1 AND 15)""",
    # (the qualifying supplier keys first, then supplier by key: as an IN, the planner estimates the subquery at a
    # few rows and tests every supplier of the nation against all of it, ~30 s)
    20: """WITH sk AS MATERIALIZED (SELECT DISTINCT ps_suppkey FROM partsupp, mv_q20 m
                                    WHERE m.p_name LIKE $2 AND m.yr = EXTRACT(year FROM $1)
                                      AND ps_partkey = m.l_partkey AND ps_suppkey = m.l_suppkey AND ps_availqty > 0.5 * m.sq)
           SELECT s_name, s_address FROM sk JOIN supplier ON s_suppkey = sk.ps_suppkey JOIN nation ON s_nationkey = n_nationkey
           WHERE n_name = $3 ORDER BY s_name""",
    21: """SELECT s_name, numwait FROM mv_q21 WHERE n_name = $1 ORDER BY numwait DESC, s_name LIMIT 100""",
    22: f"""WITH a AS (SELECT SUM(s) / SUM(n) AS avgbal FROM mv_q22_avg WHERE cntrycode IN ({IN7}))
           SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal FROM mv_q22, a
           WHERE cntrycode IN ({IN7}) AND c_acctbal > a.avgbal GROUP BY cntrycode ORDER BY cntrycode""",
}

assert sorted(READ) == list(range(1, 23))
assert all(q in IMMV or q in FULL for q in READ)

# ---------------------------------------------------------------------------------------------
# v2 designs (read-optimized; MV_DESIGN picks them: "v1" none, "v2" all, or a list of query numbers "3 10 15").
# A window parameter becomes a row of a static helper table that the view joins, so the read is an index lookup
# of one window instead of re-aggregating millions of rows; pg_ivm then maintains every window a row falls in.
# ---------------------------------------------------------------------------------------------
_MONTHS = [f"{1993 + m // 12}-{m % 12 + 1:02d}-01" for m in range(58)]  # 1993-01 .. 1997-10: every 3-month window start
AUX_V2 = {
    "mv_win3": ("ws date PRIMARY KEY, we date NOT NULL",
                lambda: [(s, f"{int(s[:4]) + (int(s[5:7]) + 2) // 12}-{(int(s[5:7]) + 2) % 12 + 1:02d}-01") for s in _MONTHS]),
    "mv_q3_days": ("d date PRIMARY KEY", lambda: [(f"1995-03-{i:02d}",) for i in range(1, 32)]),
}
IMMV_V2 = {
    # Q2: the type's last word as a column (p_type LIKE '%' || word) and the region's minimum cost per part as
    # its own view, so the read is index lookups instead of a correlated MIN per candidate
    2: {"mv_q2t": ("""SELECT p_partkey, p_size, p_type, substring(p_type from '[^ ]+$') AS t3, p_mfgr, ps_partkey, ps_suppkey,
                       ps_supplycost, s_suppkey, s_acctbal, s_name, s_address, s_phone, s_comment, n_nationkey, n_name,
                       r_regionkey, r_name
                     FROM part, partsupp, supplier, nation, region
                     WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey
                       AND n_regionkey = r_regionkey""", [["r_name", "p_size", "t3"]]),
        "mv_q2min": (f"""SELECT {PK('ps_partkey', 'r_name')} AS k, ps_partkey, r_name, MIN(ps_supplycost) AS mc
                         FROM partsupp, supplier, nation, region
                         WHERE s_suppkey = ps_suppkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
                         GROUP BY {PK('ps_partkey', 'r_name')}, ps_partkey, r_name""", [["k"]])},
    # Q3: one row per (day of March 1995, order) that the day splits (o_orderdate < day < l_shipdate)
    3: {"mv_q3d": (f"""SELECT {PK("d.d - DATE '1970-01-01'", 'l_orderkey')} AS k, d.d, l_orderkey, o_orderdate, o_shippriority, c_mktsegment,
                        SUM(l_extendedprice * (1 - l_discount)) AS rev
                      FROM customer, orders, lineitem, mv_q3_days d
                      WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate < d.d AND l_shipdate > d.d
                      GROUP BY {PK("d.d - DATE '1970-01-01'", 'l_orderkey')}, d.d, l_orderkey, o_orderdate, o_shippriority, c_mktsegment""",
                   [["k"], ["d", "c_mktsegment", "rev DESC", "o_orderdate"]])},
    # Q10 / Q15: one row per (3-month window start, customer / supplier)
    10: {"mv_q10w": (f"""SELECT {PK("w.ws - DATE '1970-01-01'", 'o_custkey')} AS k, w.ws, o_custkey, SUM(l_extendedprice * (1 - l_discount)) AS rev
                        FROM orders, lineitem, mv_win3 w
                        WHERE l_orderkey = o_orderkey AND l_returnflag = 'R' AND o_orderdate >= w.ws AND o_orderdate < w.we
                          AND w.ws >= DATE '1993-02-01' AND w.ws <= DATE '1995-01-01'
                        GROUP BY {PK("w.ws - DATE '1970-01-01'", 'o_custkey')}, w.ws, o_custkey""", [["k"], ["ws", "rev DESC", "o_custkey"]])},
    15: {"mv_q15w": (f"""SELECT {PK("w.ws - DATE '1970-01-01'", 'l_suppkey')} AS k, w.ws, l_suppkey, SUM(l_extendedprice * (1 - l_discount)) AS rev
                        FROM lineitem, mv_win3 w WHERE l_shipdate >= w.ws AND l_shipdate < w.we
                        GROUP BY {PK("w.ws - DATE '1970-01-01'", 'l_suppkey')}, w.ws, l_suppkey""", [["k"], ["ws", "rev DESC"]])},
    # Q21 without a refreshed view: both (NOT) EXISTS tests depend only on (order, supplier), so per-order and
    # per-(order, supplier) line / late-line counts decide them: another supplier exists <=> n(o) > n(o,s), no other
    # supplier is late <=> late(o) = late(o,s); numwait(s) sums late(o,s).  Only 'F' orders can qualify, and every
    # line of an order shares its status, so both views keep 'F' orders only.
    21: {"mv_q21_os": (f"""SELECT {PK('l_orderkey', 'l_suppkey')} AS k, l_orderkey, l_suppkey, COUNT(*) AS n,
                            SUM(CASE WHEN l_receiptdate > l_commitdate THEN 1 ELSE 0 END) AS late
                          FROM lineitem, orders WHERE o_orderkey = l_orderkey AND o_orderstatus = 'F'
                          GROUP BY {PK('l_orderkey', 'l_suppkey')}, l_orderkey, l_suppkey""",
                       [["k"], "(l_suppkey) INCLUDE (l_orderkey, n, late) WHERE late > 0"]),
         "mv_q21_o": ("""SELECT l_orderkey, COUNT(*) AS n, SUM(CASE WHEN l_receiptdate > l_commitdate THEN 1 ELSE 0 END) AS late
                         FROM lineitem, orders WHERE o_orderkey = l_orderkey AND o_orderstatus = 'F'
                         GROUP BY l_orderkey""", ["(l_orderkey) INCLUDE (n, late)"])},
    # Q22 without a refreshed view: customers with orders as an IMMV (a group disappears with its last order);
    # the read anti-joins customer to it
    22: {"mv_q22_avg": IMMV[22]["mv_q22_avg"],
         "mv_q22_cnt": ("SELECT o_custkey, COUNT(*) AS n FROM orders GROUP BY o_custkey", [])},
}
READ_V2 = {
    2: """SELECT m.s_acctbal, m.s_name, m.n_name, m.p_partkey, m.p_mfgr, m.s_address, m.s_phone, m.s_comment
          FROM mv_q2t m JOIN mv_q2min x ON x.k = ((m.p_partkey)::text || '|' || (m.r_name)::text) AND x.mc = m.ps_supplycost
          WHERE m.r_name = $3 AND m.p_size = $1 AND m.t3 = btrim($2, '%') AND m.p_type LIKE $2
          ORDER BY m.s_acctbal DESC, m.n_name, m.s_name, m.p_partkey LIMIT 100""",
    3: """SELECT l_orderkey, rev AS revenue, o_orderdate, o_shippriority FROM mv_q3d
          WHERE d = $2 AND c_mktsegment = $1 ORDER BY rev DESC, o_orderdate LIMIT 10""",
    10: """WITH t AS (SELECT o_custkey, rev FROM mv_q10w WHERE ws = $1 ORDER BY rev DESC, o_custkey LIMIT 20)
           SELECT c_custkey, c_name, t.rev AS revenue, c_acctbal, n_name, c_address, c_phone, c_comment
           FROM t JOIN customer ON c_custkey = t.o_custkey JOIN nation ON c_nationkey = n_nationkey
           ORDER BY t.rev DESC, c_custkey LIMIT 20""",
    15: """SELECT s_suppkey, s_name, s_address, s_phone, w.rev AS total_revenue FROM mv_q15w w JOIN supplier ON s_suppkey = w.l_suppkey
           WHERE w.ws = $1 AND w.rev = (SELECT MAX(rev) FROM mv_q15w WHERE ws = $1) ORDER BY s_suppkey""",
    21: """SELECT s_name, SUM(os.late) AS numwait
           FROM nation JOIN supplier ON s_nationkey = n_nationkey
             JOIN mv_q21_os os ON os.l_suppkey = s_suppkey AND os.late > 0
             JOIN mv_q21_o o ON o.l_orderkey = os.l_orderkey AND o.n > os.n AND o.late = os.late
           WHERE n_name = $1 GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100""",
    22: f"""WITH a AS (SELECT SUM(s) / SUM(n) AS avgbal FROM mv_q22_avg WHERE cntrycode IN ({IN7}))
           SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal
           FROM customer, a
           WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ({IN7}) AND c_acctbal > a.avgbal
             AND NOT EXISTS (SELECT 1 FROM mv_q22_cnt WHERE o_custkey = c_custkey)
           GROUP BY 1 ORDER BY 1""",
}
V2_QUERIES = sorted(IMMV_V2)


def _apply_design(design):
    qs = set(V2_QUERIES) if design == "v2" else set() if design in ("", "v1") else {int(x) for x in design.split()}
    for q in sorted(qs):
        IMMV[q] = IMMV_V2[q]
        FULL.pop(q, None)  # (Q21 / Q22 v2: incremental views replace the refreshed ones)
        if q in READ_V2:
            READ[q] = READ_V2[q]
    for name, spec in AUX_V2.items():
        if any(name in sql for q in qs for sql, _ in IMMV[q].values()):
            AUX[name] = spec
    return sorted(qs)


ALL_NAMES = ({n for d in (IMMV, IMMV_V2) for q in d for n in d[q]}, {n for q in FULL for n in FULL[q]})  # (immv, full), every design
DESIGN = _apply_design(os.environ.get("MV_DESIGN", "v1").strip())


def mv_sql(q, verus_args):
    """The view read for query q with the kit's verus arguments (as base_sql substitutes them)."""
    import run_sql_backend as R
    args = R.split_args(verus_args)
    return re.sub(r"\$(\d+)", lambda m: args[int(m.group(1)) - 1], READ[q])


def full_views():
    """[(name, deps)] of every plain (refreshed) view."""
    return [(n, deps) for q in FULL for n, (_, _, deps) in FULL[q].items()]


# commit kinds (run_sql_backend.Stream) that write a view's tables but no column it reads: no refresh needed
UNAFFECTED = {
    "mv_q21": {"supplier update"},  # sets s_acctbal and s_comment; Q21 reads s_name, s_nationkey
}
# the plain views' plans: the planner estimates Q21's NOT EXISTS anti join at 1 row and nests 200k suppliers over
# its millions of rows (30+ min); hash joins compute it in ~30 s.  SET LOCAL in the writer's transaction
# The writer's refreshes may use the whole machine's parallel workers (the accelerator's maintenance may use all 80
# threads); they come from the server's shared pool (max_parallel_workers), so readers and refresh together stay within it.
REFRESH_SETTINGS = ["SET LOCAL enable_nestloop = off",
                    f"SET LOCAL max_parallel_workers_per_gather = {int(os.environ.get('MV_REFRESH_WORKERS', os.cpu_count()))}"]


# CONCURRENTLY (default): readers keep reading the old contents while the writer's transaction refreshes (a plain
# REFRESH holds an ACCESS EXCLUSIVE lock to commit, so every Q21 / Q22 read waits for the whole write); needs the
# views' unique indexes.  MV_REFRESH=plain for the blocking form.
REFRESH_MODE = os.environ.get("MV_REFRESH", "concurrent")


def refresh_for(tables, kind=None, queries=None):
    """REFRESH statements for the plain views reading any of tables (minus those commit `kind` cannot change), of the
    queries being run (default all: a run over a subset needs only its own views fresh, and the others may not exist)."""
    how = "" if REFRESH_MODE == "plain" else "CONCURRENTLY "
    own = {n for q in FULL if queries is None or q in queries for n in FULL[q]}
    return [f"REFRESH MATERIALIZED VIEW {how}{n}" for n, deps in full_views() if n in own
            if deps & set(tables) and kind not in UNAFFECTED.get(n, ())]


def _load_domains():
    global COLORS, W1, W2
    import contextlib, io
    argv, sys.argv = sys.argv, sys.argv[:1]  # the generator parses sys.argv and prints a draw at import
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            import gen_tpch_params as G
    finally:
        sys.argv = argv
    COLORS, W1, W2 = G.COLORS, G.WORD1, G.WORD2


def connect(a, schema):
    import psycopg2
    c = psycopg2.connect(host="127.0.0.1", port=int(os.environ.get("PG_PORT", 5436)), user=os.environ.get("PGUSER", "postgres"),
                         password=os.environ.get("PGPASSWORD", "postgres"), dbname=a.db)
    c.autocommit = True
    cur = c.cursor()
    cur.execute("LOAD 'pg_ivm'")
    cur.execute(f"SET search_path = {schema}, pgivm, public")
    cur.execute(f"SET max_parallel_workers_per_gather = {a.workers}")
    cur.execute("SET maintenance_work_mem = '4GB'")
    cur.execute("SET work_mem = '1GB'")
    cur.execute("SET jit = off")  # JIT compiles pg_ivm's small delta statements for ~0.3 s each (inflated costs)
    return c, cur


def index_ddl(name, cols):
    if isinstance(cols, str):  # raw, e.g. "(o_orderdate) INCLUDE (o_orderpriority)" or "UNIQUE (c_custkey)"
        return f"CREATE UNIQUE INDEX ON {name} {cols[7:]}" if cols.startswith("UNIQUE ") else f"CREATE INDEX ON {name} {cols}"
    return f"CREATE INDEX ON {name} ({', '.join(cols)})"


def build_one(a, schema, kind, name, sql, idx):
    import psycopg2
    c, cur = connect(a, schema)
    t = time.perf_counter()
    for attempt in range(20):
        try:
            if kind == "immv":
                cur.execute("SELECT pgivm.create_immv(%s, %s)", (name, re.sub(r"\s+", " ", sql)))
            else:
                cur.execute("SET enable_nestloop = off")  # (REFRESH_SETTINGS)
                cur.execute(f"CREATE MATERIALIZED VIEW {name} AS {sql}")
            break
        except psycopg2.errors.DeadlockDetected:
            # concurrent create_immv calls lock the same base tables to add their triggers: retry
            print(f"  {name}: deadlock with another build, retrying", flush=True)
            time.sleep(1 + attempt)
    for cols in idx:
        cur.execute(index_ddl(name, cols))
    cur.execute(f"ANALYZE {name}")
    cur.execute("SELECT pg_total_relation_size(%s)", (name,))
    size = cur.fetchone()[0]
    c.close()
    dt = time.perf_counter() - t
    print(f"  {kind:4} {name:14} {dt:7.1f} s  {size / 2**20:9.1f} MB", flush=True)
    return name, dt, size


def cmd_create(a):
    _load_domains()
    qs = [int(x) for x in a.queries.split()]
    c, cur = connect(a, a.schema)
    cur.execute("CREATE EXTENSION IF NOT EXISTS pg_ivm")
    exists = lambda n: cur.execute("SELECT to_regclass(%s)", (f"{a.schema}.{n}",)) or cur.fetchone()[0] is not None
    for name, (cols, rows) in AUX.items():
        if not exists(name):
            cur.execute(f"CREATE TABLE {name} ({cols})")
            data = rows()
            cur.executemany(f"INSERT INTO {name} VALUES ({', '.join(['%s'] * len(data[0]))})", data)
            cur.execute(f"ANALYZE {name}")
    jobs = [("immv", n, s, i) for q in qs for n, (s, i) in IMMV.get(q, {}).items()] + \
           [("full", n, s, i) for q in qs for n, (s, i, _) in FULL.get(q, {}).items()]
    skip = [j[1] for j in jobs if exists(j[1])]
    if skip:
        print("already built (kept):", " ".join(skip), flush=True)
    jobs = [j for j in jobs if j[1] not in skip]
    c.close()
    # biggest first, so the long builds overlap
    t = time.perf_counter()
    with ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(lambda j: build_one(a, a.schema, *j), jobs))
    print(f"built {len(res)} views in {time.perf_counter() - t:.0f} s, {sum(r[2] for r in res) / 2**30:.1f} GB", flush=True)


def cmd_drop(a):
    _load_domains()
    c, cur = connect(a, a.schema)
    for n in sorted(ALL_NAMES[0]):  # every design's views, so switching designs leaves no stale triggers
        cur.execute(f"DROP TABLE IF EXISTS {a.schema}.{n} CASCADE")  # an IMMV is a table; dropping it drops its triggers
    for n in sorted(ALL_NAMES[1]):
        cur.execute(f"DROP MATERIALIZED VIEW IF EXISTS {a.schema}.{n} CASCADE")
    for n in {**AUX, **AUX_V2}:
        cur.execute(f"DROP TABLE IF EXISTS {a.schema}.{n} CASCADE")
    cur.execute("SELECT count(*) FROM pg_trigger t JOIN pg_class r ON r.oid = t.tgrelid JOIN pg_namespace s ON s.oid = r.relnamespace "
                "WHERE s.nspname = %s AND t.tgname LIKE 'IVM_trigger%%'", (a.schema,))
    print("dropped; IVM triggers left in", a.schema, ":", cur.fetchone()[0])
    c.close()


def edge_sets(q):
    """Parameter sets at the ends of the substitution domain (gen_tpch_params.py), where a view's built-in
    filters (date windows, discount / quantity ranges) are tightest: [{"params", "verus"}]."""
    D = lambda s: f"DATE '{s}'"
    m3 = lambda s: f"{int(s[:4]) + (int(s[5:7]) + 2) // 12}-{(int(s[5:7]) + 2) % 12 + 1:02d}-01"
    m1 = lambda s: f"{int(s[:4]) + int(s[5:7]) // 12}-{int(s[5:7]) % 12 + 1:02d}-01"
    y1 = lambda s: f"{int(s[:4]) + 1}{s[4:]}"
    out = []
    add = lambda p, v: out.append({"params": p, "verus": v})
    if q == 1:
        for x in (60, 120): add({"delta": x}, f"{x}")
    elif q == 3:
        for day in ("1995-03-01", "1995-03-31"):
            for seg in ("AUTOMOBILE", "MACHINERY"): add({"segment": seg, "date": day}, f"'{seg}', {D(day)}")
    elif q in (4, 10, 15):
        lo, hi = {4: ("1993-01-01", "1997-10-01"), 10: ("1993-02-01", "1995-01-01"), 15: ("1993-01-01", "1997-10-01")}[q]
        for day in (lo, hi): add({"date": day}, f"{D(day)}, {D(m3(day))}")
    elif q == 5:
        for day in ("1993-01-01", "1997-01-01"): add({"region": "ASIA", "date": day}, f"'ASIA', {D(day)}, {D(y1(day))}")
    elif q == 6:
        for day, disc, qty in (("1993-01-01", "0.02", 24), ("1997-01-01", "0.09", 25)):
            add({"date": day, "discount": disc, "quantity": qty}, f"{D(day)}, {disc}, {qty}.00, {D(y1(day))}")
    elif q == 12:
        for day in ("1993-01-01", "1997-01-01"):
            add({"shipmode1": "MAIL", "shipmode2": "SHIP", "date": day}, f"'MAIL', 'SHIP', {D(day)}, {D(y1(day))}")
    elif q == 14:
        for day in ("1993-01-01", "1997-12-01"): add({"date": day}, f"{D(day)}, {D(m1(day))}")
    elif q == 18:
        for x in (312, 315): add({"quantity": x}, f"{x}")
    elif q == 20:
        for day in ("1993-01-01", "1997-01-01"):
            add({"date": day, "color": "forest", "nation": "CANADA"}, f"{D(day)}, 'forest%', 'CANADA', {D(y1(day))}")
    return out


def cmd_check(a):
    """Every query's view answer vs the base query, for --sets parameter sets of the given seed's draw
    (--edges: also the ends of each parameter domain)."""
    import types
    import run_sql_backend as R
    import run_compare
    qs = [int(x) for x in a.queries.split()]
    d = types.SimpleNamespace(seed=a.seed, repeats=1, sets=a.sets, sf=a.sf, queries=qs, out=a.out)
    _, _, spec = run_compare.draws(d)[0]
    c, cur = connect(a, a.schema)
    bad = 0
    for q in qs:
        for s in spec[str(q)]["sets"] + (edge_sets(q) if a.edges else []):
            t = time.perf_counter(); cur.execute(R.base_sql(q, s["verus"], "postgres")); b = cur.fetchall(); tb = time.perf_counter() - t
            t = time.perf_counter(); cur.execute(mv_sql(q, s["verus"])); m = cur.fetchall(); tm = time.perf_counter() - t
            ok = R.same(R.canon("postgres", b, False), R.canon("postgres", m, False))
            bad += not ok
            print(f"Q{q:<3} {'ok  ' if ok else 'DIFF'} rows {len(b):5}/{len(m):<5} base {tb * 1e3:9.1f} ms  mv {tm * 1e3:8.2f} ms  {s['params']}", flush=True)
            if not ok and a.verbose:
                print("   base:", b[:5], "\n   mv:  ", m[:5])
    print("mismatches:", bad)
    c.close()
    return bad


def main():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("create", "drop", "check"))
    ap.add_argument("--db", default=os.environ.get("PG_DB", "accel_tpch_sf20"))
    ap.add_argument("--schema", required=True)
    ap.add_argument("--queries", default=" ".join(map(str, range(1, 23))))
    ap.add_argument("--workers", type=int, default=9, help="max_parallel_workers_per_gather for builds / checks")
    ap.add_argument("--jobs", type=int, default=6, help="views built concurrently")
    ap.add_argument("--seed", type=int, default=35489017)
    ap.add_argument("--sets", type=int, default=2)
    ap.add_argument("--sf", type=float, default=20)
    ap.add_argument("--out", default="/tmp/mv_check_draws")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--edges", action="store_true", help="check: also the ends of each parameter domain")
    a = ap.parse_args()
    sys.exit({"create": cmd_create, "drop": cmd_drop, "check": cmd_check}[a.mode](a) and 1 or 0)


if __name__ == "__main__":
    main()
