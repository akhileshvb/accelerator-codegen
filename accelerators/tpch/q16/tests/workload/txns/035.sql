INSERT INTO supplier
SELECT (SELECT max(s_suppkey) FROM supplier) + 1, 'Supplier#V16-09',
       s_address, s_nationkey, s_phone, 2200.00, 'v16 supplier contract renewed'
FROM supplier ORDER BY s_suppkey LIMIT 1;
INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 150, 48.00, 'v16 renewed sourcing'
FROM part p CROSS JOIN supplier s
WHERE p.p_name LIKE 'Verus catalog %' AND s.s_name = 'Supplier#V16-09';
