INSERT INTO part
SELECT (SELECT max(p_partkey) FROM part) + 1, 'Verus catalog 13', p_mfgr,
       'Brand#45', 'PROMO PLATED STEEL', 23, p_container, 950.00, 'v16 new catalog'
FROM part ORDER BY p_partkey LIMIT 1;
INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 150, 60.00, 'v16 new catalog edge'
FROM part p CROSS JOIN supplier s
WHERE p.p_name = 'Verus catalog 13'
  AND s.s_name IN ('Supplier#V16-08', 'Supplier#V16-09');
