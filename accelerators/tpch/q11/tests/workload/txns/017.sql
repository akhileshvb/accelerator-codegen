UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'GERMANY')
WHERE s_comment = 'q11-depot-a';
UPDATE partsupp SET ps_availqty = ps_availqty + 30 WHERE ps_comment = 'q11-stock-a2';
