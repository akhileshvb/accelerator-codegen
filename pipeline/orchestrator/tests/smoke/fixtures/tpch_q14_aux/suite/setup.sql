-- Q14's fixture, run once before the accelerator is built (eval/harness/suite.hpp, RunSuiteDifferential):
-- the transactions of txns/ all target rows by subquery, so they need the query window
-- [1995-06-01, 1995-07-01) to hold at least one lineitem on a PROMO part and one on a part that is
-- not PROMO. Both magnitudes hold that at SF1 and in the test subset, but a suite must apply on any
-- dataset the gates point it at, so this one statement adds one window line per class only when the
-- class is missing. It is a plain schema-valid INSERT (no keys are declared, no foreign keys).
-- One statement, so no harness has to split it.
INSERT INTO lineitem
    (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
     l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode,
     l_comment)
SELECT (SELECT COALESCE(MAX(l_orderkey), 0) FROM lineitem) + 1,
       f.p_partkey, f.s_suppkey, 1, 5.00, 1000.00, 0.05, 0.02, 'N', 'O',
       DATE '1995-06-10', DATE '1995-06-15', DATE '1995-06-20', 'NONE', 'AIR', 'Q14FIXTURE'
FROM (SELECT p.p_partkey, (SELECT MIN(s_suppkey) FROM supplier) AS s_suppkey
      FROM part p WHERE p.p_type LIKE 'PROMO%' ORDER BY p.p_partkey LIMIT 1) f
WHERE NOT EXISTS (SELECT 1 FROM lineitem l JOIN part q ON l.l_partkey = q.p_partkey
                  WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
                    AND q.p_type LIKE 'PROMO%')
UNION ALL
SELECT (SELECT COALESCE(MAX(l_orderkey), 0) FROM lineitem) + 2,
       f.p_partkey, f.s_suppkey, 1, 5.00, 1000.00, 0.05, 0.02, 'N', 'O',
       DATE '1995-06-10', DATE '1995-06-15', DATE '1995-06-20', 'NONE', 'AIR', 'Q14FIXTURE'
FROM (SELECT p.p_partkey, (SELECT MIN(s_suppkey) FROM supplier) AS s_suppkey
      FROM part p WHERE NOT (p.p_type LIKE 'PROMO%') ORDER BY p.p_partkey LIMIT 1) f
WHERE NOT EXISTS (SELECT 1 FROM lineitem l JOIN part q ON l.l_partkey = q.p_partkey
                  WHERE l.l_shipdate >= DATE '1995-06-01' AND l.l_shipdate < DATE '1995-07-01'
                    AND NOT (q.p_type LIKE 'PROMO%'));
