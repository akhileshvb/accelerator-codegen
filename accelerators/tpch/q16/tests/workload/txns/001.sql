INSERT INTO supplier
SELECT (SELECT max(s_suppkey) FROM supplier) + 1, 'Supplier#V16-09',
       s_address, s_nationkey, s_phone, 1500.00, 'v16 new supplier approved'
FROM supplier ORDER BY s_suppkey LIMIT 1;
INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 250, 42.00, 'v16 supplier onboarding'
FROM part p CROSS JOIN supplier s
WHERE p.p_name = 'Verus catalog 01' AND s.s_name = 'Supplier#V16-09';
