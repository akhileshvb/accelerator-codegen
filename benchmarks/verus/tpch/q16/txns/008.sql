INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 80, 32.50, 'v16 second source'
FROM part p CROSS JOIN supplier s
WHERE p.p_name BETWEEN 'Verus catalog 01' AND 'Verus catalog 03'
  AND s.s_name IN ('Supplier#V16-05', 'Supplier#V16-06');
