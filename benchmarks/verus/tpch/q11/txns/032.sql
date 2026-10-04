UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.01 WHERE ps_comment = 'q11-edge-large';
UPDATE partsupp SET ps_supplycost = -10.00 WHERE ps_comment = 'q11-edge-large';
UPDATE partsupp SET ps_supplycost = 1.00 WHERE ps_comment IN ('q11-edge-below', 'q11-edge-above');
INSERT INTO partsupp
SELECT ps_partkey, ps_suppkey, 1, -1.00, 'q11-edge-cancel'
FROM partsupp WHERE ps_comment = 'q11-edge-positive';
