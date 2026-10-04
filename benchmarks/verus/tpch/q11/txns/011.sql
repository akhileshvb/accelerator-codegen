UPDATE partsupp SET ps_availqty = ps_availqty + 10 WHERE ps_comment = 'q11-new-depot-stock';
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'FRANCE')
WHERE s_comment = 'q11-new-depot';
