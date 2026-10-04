UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.01 WHERE ps_comment = 'q11-edge-large';
UPDATE partsupp SET ps_supplycost = ps_supplycost - 0.02 WHERE ps_comment = 'q11-edge-large';
