INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 120, 24.50, 'v16 supplier reinstated'
FROM part p CROSS JOIN supplier s
WHERE p.p_name = 'Verus catalog 01' AND s.s_name = 'Supplier#V16-02';
