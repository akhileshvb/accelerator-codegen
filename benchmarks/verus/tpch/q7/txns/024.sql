INSERT INTO nation SELECT (SELECT max(n_nationkey)+1 FROM nation), 'TEST COAST', n_regionkey, 'q7 territory' FROM nation ORDER BY n_nationkey LIMIT 1;

UPDATE supplier SET s_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='TEST COAST' ORDER BY n_nationkey LIMIT 1) WHERE s_comment='q7 supplier A';

UPDATE customer SET c_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='TEST COAST' ORDER BY n_nationkey LIMIT 1) WHERE c_comment='q7 customer B';
