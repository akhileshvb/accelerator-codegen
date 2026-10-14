UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'CANADA' AND n_comment <> 'q20-territory') WHERE s_nationkey IN (SELECT n_nationkey FROM nation WHERE n_comment = 'q20-territory');
DELETE FROM nation WHERE n_comment = 'q20-territory';
