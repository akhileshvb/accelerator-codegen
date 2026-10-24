UPDATE customer SET c_nationkey=(SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1) WHERE c_comment='q7 customer A';
