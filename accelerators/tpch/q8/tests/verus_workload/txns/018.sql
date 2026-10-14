UPDATE nation SET n_regionkey = (SELECT r_regionkey FROM region WHERE r_name = 'EUROPE') WHERE n_name = 'BRAZIL';
UPDATE customer SET c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL') WHERE c_comment = 'q8-customer-a';
