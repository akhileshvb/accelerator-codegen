UPDATE partsupp SET ps_availqty = ps_availqty - 35
WHERE ps_comment IN ('q11-new-stock-1', 'q11-new-stock-2');
