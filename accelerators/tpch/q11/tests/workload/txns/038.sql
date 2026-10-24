UPDATE partsupp SET ps_supplycost = 9999999999999.99, ps_availqty = 2147483647
WHERE ps_comment = 'q11-edge-large';
UPDATE partsupp SET ps_availqty = -2147483648, ps_supplycost = -9999999999999.99
WHERE ps_comment = 'q11-edge-negative';
INSERT INTO partsupp
SELECT (SELECT min(ps_partkey) - 1 FROM partsupp), ps_suppkey, 0, 9999999999999.99, 'q11-extreme-zero'
FROM partsupp WHERE ps_comment = 'q11-edge-large';
