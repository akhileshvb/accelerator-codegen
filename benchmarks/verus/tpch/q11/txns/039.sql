UPDATE partsupp SET ps_supplycost = 9996.99, ps_availqty = 1 WHERE ps_comment = 'q11-edge-large';
UPDATE partsupp SET ps_supplycost = -2.00, ps_availqty = 1 WHERE ps_comment = 'q11-edge-negative';
UPDATE partsupp SET ps_supplycost = 0.99 WHERE ps_comment = 'q11-edge-below';
UPDATE partsupp SET ps_supplycost = 1.01 WHERE ps_comment = 'q11-edge-above';
DELETE FROM partsupp WHERE ps_comment IN ('q11-edge-cancel', 'q11-extreme-zero');
UPDATE nation SET n_name = 'GERMANY ' WHERE n_name = 'GERMANY';
UPDATE nation SET n_name = 'germany' WHERE n_name = ' GERMANY';
