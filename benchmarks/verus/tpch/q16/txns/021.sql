INSERT INTO part
SELECT (SELECT max(p_partkey) FROM part) + v.n, 'Verus catalog ' || CAST(v.n + 13 AS VARCHAR),
       p.p_mfgr, 'Brand#35', 'LARGE ANODIZED BRASS', v.sz,
       p.p_container, 1200.00, 'v16 catalog batch'
FROM (SELECT * FROM part ORDER BY p_partkey LIMIT 1) p
CROSS JOIN (VALUES (1, 36), (2, 45)) v(n, sz);
INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 70, 55.00, 'v16 catalog batch edge'
FROM part p CROSS JOIN supplier s
WHERE p.p_name IN ('Verus catalog 14', 'Verus catalog 15')
  AND s.s_name IN ('Supplier#V16-01', 'Supplier#V16-08', 'Supplier#V16-09');
