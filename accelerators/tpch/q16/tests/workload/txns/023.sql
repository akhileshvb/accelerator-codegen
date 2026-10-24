UPDATE supplier SET s_comment = 'v16 Customer billing Complaints open' WHERE s_name = 'Supplier#V16-08';
UPDATE part SET p_type = 'SMALL BRUSHED BRASS' WHERE p_name = 'Verus catalog 05';
INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 40, 28.00, 'v16 mixed write edge'
FROM part p CROSS JOIN supplier s
WHERE p.p_name IN ('Verus catalog 05', 'Verus catalog 07') AND s.s_name = 'Supplier#V16-09';
