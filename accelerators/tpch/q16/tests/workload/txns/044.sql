UPDATE partsupp SET ps_partkey = (SELECT p_partkey FROM part WHERE p_name = 'Verus catalog 09')
WHERE ps_comment = 'v16 rekey part';
UPDATE partsupp SET ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-07')
WHERE ps_comment = 'v16 rekey supplier';
INSERT INTO partsupp
SELECT (SELECT max(p_partkey) FROM part) + 10, s_suppkey, 1, 0.01, 'v16 orphan edge'
FROM supplier WHERE s_name = 'Supplier#V16-09';
UPDATE part SET p_type = 'SMALL BRUSHED' WHERE p_name = 'Verus catalog 08';
INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 5, 1.00, 'v16 excluded new edge'
FROM part p CROSS JOIN supplier s
WHERE p.p_name = 'Verus catalog 08' AND s.s_name = 'Supplier#V16-08';
