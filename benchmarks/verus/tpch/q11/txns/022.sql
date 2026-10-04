UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'FRANCE')
WHERE s_comment = 'q11-depot-b';
DELETE FROM partsupp WHERE ps_comment = 'q11-restock-batch';
