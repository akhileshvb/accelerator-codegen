UPDATE supplier SET s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1) WHERE s_comment='q7 supplier A';
