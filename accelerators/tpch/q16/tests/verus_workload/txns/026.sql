INSERT INTO part
SELECT (SELECT max(p_partkey) FROM part) + 1, 'Verus catalog 06', p_mfgr,
       'Brand#22', 'ECONOMY ANODIZED TIN', 36, p_container, 800.00, 'v16 catalog restored'
FROM part ORDER BY p_partkey LIMIT 1;
INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 90, 33.00, 'v16 restored edge'
FROM part p CROSS JOIN supplier s
WHERE p.p_name = 'Verus catalog 06' AND s.s_name IN ('Supplier#V16-04', 'Supplier#V16-05');
