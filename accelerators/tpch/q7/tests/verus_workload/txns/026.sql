UPDATE customer SET c_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1) WHERE c_comment='q7 customer B';

UPDATE supplier SET s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1) WHERE s_comment='q7 supplier A';

DELETE FROM nation WHERE n_comment='q7 territory';
