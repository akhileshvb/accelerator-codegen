UPDATE partsupp SET ps_availqty = ps_availqty + 40, ps_supplycost = ps_supplycost - 1.25
WHERE ps_comment IN ('q11-stock-a1', 'q11-stock-a2', 'q11-stock-b1', 'q11-stock-b2');
