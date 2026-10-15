-- Small TPC-H subset for fast shared-interface tests (tpch_test.duckdb).
--
-- Run in a fresh DuckDB database with the source (SF1) database attached
-- read-only as `src`. Built by scripts/create_tpch_test_db.py and, for ctest,
-- by make_tpch_test_db (eval/tests/make_tpch_test_db.cpp) into the build dir.
--
-- seed_lineitems picks lineitems that give each shared query's catalog probe
-- (probe_select_sql) a row to update; the tables then keep every row those
-- lineitems reference.
CREATE TEMP TABLE seed_lineitems AS
SELECT DISTINCT l_orderkey, l_linenumber FROM (
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
     LIMIT 120)
    UNION
    (SELECT l.l_orderkey, l.l_linenumber
     FROM src.lineitem l
     JOIN src.orders o ON l.l_orderkey = o.o_orderkey
     JOIN src.customer c ON o.o_custkey = c.c_custkey
     WHERE c.c_mktsegment = 'BUILDING'
       AND o.o_orderdate < DATE '1995-03-15'
       AND l.l_shipdate > DATE '1995-03-15'
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipdate >= DATE '1995-01-01'
       AND l_shipdate < DATE '1996-01-01'
       AND l_discount BETWEEN 0.02 AND 0.04
       AND l_quantity < 25
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_returnflag = 'R'
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipmode IN ('MAIL', 'SHIP')
       AND l_receiptdate >= DATE '1995-01-01'
       AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipdate >= DATE '1995-06-01'
       AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month
     LIMIT 120)
    UNION
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     JOIN src.part p ON l.l_partkey = p.p_partkey
     WHERE p.p_size = 10 AND p.p_type LIKE '%BRASS%' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem l
     JOIN src.orders o ON l.l_orderkey = o.o_orderkey
     WHERE o.o_orderdate >= DATE '1996-01-01'
       AND o.o_orderdate < DATE '1997-01-01' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem
     WHERE l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem
     WHERE l_shipdate >= DATE '1994-11-01'
       AND l_shipdate < DATE '1995-02-01' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem
     WHERE l_quantity > 300 LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem l
     JOIN src.part p ON l.l_partkey = p.p_partkey
     WHERE p.p_brand IN ('Brand#31', 'Brand#12', 'Brand#51') LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem l
     JOIN src.orders o ON l.l_orderkey = o.o_orderkey
     WHERE o.o_orderstatus = 'F' LIMIT 80)
    UNION
    -- Q2 (size 15, %BRASS, EUROPE): lineitems for parts with several European
    -- suppliers, so some partsupp row has a cost above the part's minimum.
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     JOIN (SELECT ps_partkey, ps_suppkey FROM src.partsupp
           JOIN src.supplier ON s_suppkey = ps_suppkey
           JOIN src.nation ON n_nationkey = s_nationkey
           JOIN src.region ON r_regionkey = n_regionkey
           JOIN src.part ON p_partkey = ps_partkey
           WHERE r_name = 'EUROPE' AND p_size = 15 AND p_type LIKE '%BRASS'
           QUALIFY count(*) OVER (PARTITION BY ps_partkey) >= 2) q2
       ON l.l_partkey = q2.ps_partkey AND l.l_suppkey = q2.ps_suppkey
     ORDER BY l.l_partkey, l.l_suppkey LIMIT 80)
    UNION
    -- Q8 (INDIA, ASIA, PROMO BRUSHED BRASS)
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     JOIN src.part p ON p.p_partkey = l.l_partkey AND p.p_type = 'PROMO BRUSHED BRASS'
     JOIN src.orders o ON o.o_orderkey = l.l_orderkey
     JOIN src.customer c ON c.c_custkey = o.o_custkey
     JOIN src.nation n1 ON n1.n_nationkey = c.c_nationkey
     JOIN src.region r ON r.r_regionkey = n1.n_regionkey AND r.r_name = 'ASIA'
     WHERE o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
     ORDER BY l.l_orderkey, l.l_linenumber LIMIT 80)
    UNION
    -- Q17 (Brand#34, LG CASE): a part's extreme quantities, so some line is
    -- below 0.2 * the average quantity computed over the subset.
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     JOIN (SELECT p_partkey FROM src.part
           JOIN src.lineitem ON l_partkey = p_partkey
           WHERE p_brand = 'Brand#34' AND p_container = 'LG CASE'
           GROUP BY p_partkey
           HAVING min(l_quantity) <= 2 AND max(l_quantity) >= 40
           ORDER BY p_partkey LIMIT 10) q17 ON l.l_partkey = q17.p_partkey
     WHERE l.l_quantity <= 2 OR l.l_quantity >= 40
     ORDER BY l.l_partkey, l.l_orderkey, l.l_linenumber LIMIT 80)
    UNION
    -- Q18 (quantity threshold 312): every line of a few large orders
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     WHERE l.l_orderkey IN (SELECT l_orderkey FROM src.lineitem
                            GROUP BY l_orderkey HAVING SUM(l_quantity) > 312
                            ORDER BY l_orderkey LIMIT 10)
     ORDER BY l.l_orderkey, l.l_linenumber LIMIT 80)
    UNION
    -- Q19 (Brand#31 SM containers, quantity 4..14, size 1..5)
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     JOIN src.part p ON p.p_partkey = l.l_partkey
     WHERE p.p_brand = 'Brand#31'
       AND p.p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
       AND l.l_quantity BETWEEN 4 AND 14 AND p.p_size BETWEEN 1 AND 5
       AND l.l_shipmode IN ('AIR', 'AIR REG')
       AND l.l_shipinstruct = 'DELIVER IN PERSON'
     ORDER BY l.l_orderkey, l.l_linenumber LIMIT 80)
    UNION
    -- Q20 (1997, gainsboro%, RUSSIA)
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     JOIN src.part p ON p.p_partkey = l.l_partkey AND p.p_name LIKE 'gainsboro%'
     JOIN src.supplier s ON s.s_suppkey = l.l_suppkey
     JOIN src.nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'RUSSIA'
     WHERE l.l_shipdate >= DATE '1997-01-01' AND l.l_shipdate < DATE '1998-01-01'
     ORDER BY l.l_orderkey, l.l_linenumber LIMIT 80)
) seeds;
CREATE TABLE lineitem AS
        SELECT *
        FROM src.lineitem
        WHERE l_orderkey IN (SELECT DISTINCT l_orderkey FROM seed_lineitems);
CREATE TABLE orders AS
        SELECT *
        FROM src.orders
        WHERE o_orderkey IN (SELECT DISTINCT l_orderkey FROM lineitem);
CREATE TABLE customer AS
        SELECT *
        FROM src.customer
        WHERE c_custkey IN (SELECT DISTINCT o_custkey FROM orders)
           -- Q22 needs customers (country codes 30,19,14,28,11,22,32, high
           -- balance) that have no orders in the subset
           OR c_custkey IN (
               SELECT c_custkey FROM src.customer
               WHERE substring(c_phone, 1, 2) IN ('30', '19', '14', '28', '11', '22', '32')
                 AND c_acctbal > 8000
               ORDER BY c_custkey LIMIT 40);
CREATE TABLE supplier AS
        SELECT *
        FROM src.supplier
        WHERE s_suppkey IN (SELECT DISTINCT l_suppkey FROM lineitem);
CREATE TABLE nation AS
        SELECT *
        FROM src.nation
        WHERE n_nationkey IN (
            SELECT DISTINCT c_nationkey FROM customer
            UNION
            SELECT DISTINCT s_nationkey FROM supplier
        );
CREATE TABLE region AS
        SELECT *
        FROM src.region
        WHERE r_regionkey IN (SELECT DISTINCT n_regionkey FROM nation);
CREATE TABLE part AS
        SELECT *
        FROM src.part
        WHERE p_partkey IN (SELECT DISTINCT l_partkey FROM lineitem);
CREATE TABLE partsupp AS
        SELECT *
        FROM src.partsupp
        WHERE (ps_partkey, ps_suppkey) IN (
            SELECT l_partkey, l_suppkey FROM lineitem
        );
