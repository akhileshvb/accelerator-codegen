UPDATE customer SET c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'JAPAN') WHERE c_comment = 'q8-customer-a';
